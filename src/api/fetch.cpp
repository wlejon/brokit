#include "api/api.h"
#include "runtime/runtime.h"

#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

#include <curl/curl.h>

namespace brokit::api {

// ---------------------------------------------------------------------------
// Per-request state
// ---------------------------------------------------------------------------
struct FetchRequest {
    // Curl handle
    CURL* easy = nullptr;

    // Promise resolve/reject functions
    JSValue resolving[2] = { JS_UNDEFINED, JS_UNDEFINED };
    JSContext* ctx = nullptr;

    // Response data
    std::vector<uint8_t> body;
    std::vector<std::string> headers;
    long statusCode = 0;
    std::string statusText;
    std::string url;

    // Request body for POST etc.
    std::string requestBody;
    struct curl_slist* requestHeaders = nullptr;

    ~FetchRequest() {
        if (requestHeaders) curl_slist_free_all(requestHeaders);
    }
};

// ---------------------------------------------------------------------------
// Global multi handle + pending requests
// ---------------------------------------------------------------------------
static CURLM* g_multi = nullptr;
static std::vector<FetchRequest*> g_pending;

static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* req = static_cast<FetchRequest*>(userdata);
    size_t bytes = size * nmemb;
    req->body.insert(req->body.end(), ptr, ptr + bytes);
    return bytes;
}

static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata)
{
    auto* req = static_cast<FetchRequest*>(userdata);
    size_t bytes = size * nitems;
    std::string line(buffer, bytes);
    // Strip trailing \r\n
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    if (!line.empty()) {
        // First line is the status line
        if (line.find("HTTP/") == 0) {
            // Parse status text: "HTTP/1.1 200 OK"
            auto spacePos = line.find(' ');
            if (spacePos != std::string::npos) {
                auto secondSpace = line.find(' ', spacePos + 1);
                if (secondSpace != std::string::npos) {
                    req->statusText = line.substr(secondSpace + 1);
                }
            }
        } else {
            req->headers.push_back(line);
        }
    }
    return bytes;
}

// Build a JS Response object from a completed FetchRequest
static JSValue buildResponse(JSContext* ctx, FetchRequest* req)
{
    JSValue resp = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, static_cast<int>(req->statusCode)));
    JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, req->statusText.c_str()));
    JS_SetPropertyStr(ctx, resp, "ok", JS_NewBool(ctx, req->statusCode >= 200 && req->statusCode < 300));
    JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, req->url.c_str()));

    // Headers as a simple object
    JSValue hdrs = JS_NewObject(ctx);
    for (auto& h : req->headers) {
        auto colon = h.find(':');
        if (colon != std::string::npos) {
            std::string name = h.substr(0, colon);
            std::string value = h.substr(colon + 1);
            // Trim leading whitespace from value
            while (!value.empty() && value[0] == ' ') value.erase(0, 1);
            // Lowercase the header name for consistency
            for (auto& c : name) c = static_cast<char>(tolower(c));
            JS_SetPropertyStr(ctx, hdrs, name.c_str(), JS_NewString(ctx, value.c_str()));
        }
    }

    // headers.get(name) method
    const char* headersGetPolyfill = R"JS(
(function(hdrs) {
    var entries = hdrs;
    var obj = {
        get: function(name) {
            return entries[name.toLowerCase()] || null;
        },
        has: function(name) {
            return entries[name.toLowerCase()] !== undefined;
        },
        forEach: function(cb) {
            var keys = Object.keys(entries);
            for (var i = 0; i < keys.length; i++) {
                cb(entries[keys[i]], keys[i], this);
            }
        },
        entries: function() {
            var keys = Object.keys(entries);
            var i = 0;
            return { next: function() {
                if (i >= keys.length) return { done: true };
                var k = keys[i++];
                return { done: false, value: [k, entries[k]] };
            }, [Symbol.iterator]: function() { return this; } };
        },
        keys: function() {
            var keys = Object.keys(entries);
            var i = 0;
            return { next: function() {
                if (i >= keys.length) return { done: true };
                return { done: false, value: keys[i++] };
            }, [Symbol.iterator]: function() { return this; } };
        },
        values: function() {
            var keys = Object.keys(entries);
            var i = 0;
            return { next: function() {
                if (i >= keys.length) return { done: true };
                return { done: false, value: entries[keys[i++]] };
            }, [Symbol.iterator]: function() { return this; } };
        }
    };
    obj[Symbol.iterator] = obj.entries;
    return obj;
})
)JS";

    JSValue headersFn = JS_Eval(ctx, headersGetPolyfill, strlen(headersGetPolyfill),
                                 "<fetch-headers>", JS_EVAL_TYPE_GLOBAL);
    JSValue headersObj = JS_Call(ctx, headersFn, JS_UNDEFINED, 1, &hdrs);
    JS_FreeValue(ctx, headersFn);
    JS_FreeValue(ctx, hdrs);
    JS_SetPropertyStr(ctx, resp, "headers", headersObj);

    // Store body bytes as an ArrayBuffer on __body for text()/json()/arrayBuffer()
    JSValue bodyAB = JS_NewArrayBufferCopy(ctx, req->body.data(), req->body.size());
    JS_SetPropertyStr(ctx, resp, "__body", bodyAB);

    // text() — returns Promise<string>
    // json() — returns Promise<parsed>
    // arrayBuffer() — returns Promise<ArrayBuffer>
    const char* bodyMethods = R"JS(
(function(resp) {
    resp.text = function() {
        var decoder = new TextDecoder();
        var text = decoder.decode(new Uint8Array(this.__body));
        return Promise.resolve(text);
    };
    resp.json = function() {
        return this.text().then(function(t) { return JSON.parse(t); });
    };
    resp.arrayBuffer = function() {
        return Promise.resolve(this.__body.slice(0));
    };
    resp.blob = function() {
        var ct = this.headers.get('content-type') || '';
        return Promise.resolve(new Blob([new Uint8Array(this.__body)], { type: ct }));
    };
    resp.clone = function() {
        var r = Object.create(Object.getPrototypeOf(this));
        Object.assign(r, this);
        r.__body = this.__body.slice(0);
        return r;
    };
})
)JS";
    JSValue bodyFn = JS_Eval(ctx, bodyMethods, strlen(bodyMethods),
                              "<fetch-body>", JS_EVAL_TYPE_GLOBAL);
    JSValue ret = JS_Call(ctx, bodyFn, JS_UNDEFINED, 1, &resp);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, bodyFn);

    return resp;
}

// Tick: pump curl_multi and resolve completed requests
static JSValue js_fetch_tick(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (!g_multi || g_pending.empty()) return JS_NewInt32(ctx, 0);

    int running = 0;
    curl_multi_perform(g_multi, &running);

    int completed = 0;
    CURLMsg* msg;
    int msgs_in_queue;
    while ((msg = curl_multi_info_read(g_multi, &msgs_in_queue))) {
        if (msg->msg != CURLMSG_DONE) continue;

        CURL* easy = msg->easy_handle;
        FetchRequest* req = nullptr;

        // Find the request
        for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
            if ((*it)->easy == easy) {
                req = *it;
                g_pending.erase(it);
                break;
            }
        }
        if (!req) continue;

        curl_multi_remove_handle(g_multi, easy);

        if (msg->data.result == CURLE_OK) {
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &req->statusCode);
            char* effectiveUrl = nullptr;
            curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
            if (effectiveUrl) req->url = effectiveUrl;

            JSValue response = buildResponse(ctx, req);
            JSValue ret = JS_Call(ctx, req->resolving[0], JS_UNDEFINED, 1, &response);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, response);
        } else {
            const char* errMsg = curl_easy_strerror(msg->data.result);
            JSValue err = JS_NewError(ctx);
            JS_SetPropertyStr(ctx, err, "message",
                              JS_NewString(ctx, errMsg ? errMsg : "fetch failed"));
            JSValue ret = JS_Call(ctx, req->resolving[1], JS_UNDEFINED, 1, &err);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, err);
        }

        JS_FreeValue(ctx, req->resolving[0]);
        JS_FreeValue(ctx, req->resolving[1]);
        curl_easy_cleanup(easy);
        delete req;
        completed++;
    }

    return JS_NewInt32(ctx, completed);
}

// fetch(url, options?) — returns Promise
static JSValue js_fetch(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "fetch: URL required");

    const char* urlStr = JS_ToCString(ctx, argv[0]);
    if (!urlStr) return JS_EXCEPTION;

    auto* req = new FetchRequest();
    req->ctx = ctx;
    req->url = urlStr;
    JS_FreeCString(ctx, urlStr);

    req->easy = curl_easy_init();
    if (!req->easy) {
        delete req;
        return JS_ThrowInternalError(ctx, "fetch: curl_easy_init failed");
    }

    curl_easy_setopt(req->easy, CURLOPT_URL, req->url.c_str());
    curl_easy_setopt(req->easy, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(req->easy, CURLOPT_WRITEDATA, req);
    curl_easy_setopt(req->easy, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(req->easy, CURLOPT_HEADERDATA, req);
    curl_easy_setopt(req->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(req->easy, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(req->easy, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(req->easy, CURLOPT_USERAGENT, "brokit/0.2");
    // Accept-Encoding for automatic decompression
    curl_easy_setopt(req->easy, CURLOPT_ACCEPT_ENCODING, "");

    // Process options
    if (argc >= 2 && JS_IsObject(argv[1])) {
        // method
        JSValue methodVal = JS_GetPropertyStr(ctx, argv[1], "method");
        if (JS_IsString(methodVal)) {
            const char* method = JS_ToCString(ctx, methodVal);
            if (method) {
                curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, method);
                // For POST, set POST flag
                if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 ||
                    strcmp(method, "PATCH") == 0) {
                    curl_easy_setopt(req->easy, CURLOPT_POST, 1L);
                }
                JS_FreeCString(ctx, method);
            }
        }
        JS_FreeValue(ctx, methodVal);

        // headers
        JSValue headersVal = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (JS_IsObject(headersVal)) {
            // Iterate own properties
            JSPropertyEnum* props;
            uint32_t propCount;
            if (JS_GetOwnPropertyNames(ctx, &props, &propCount, headersVal,
                                        JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < propCount; i++) {
                    const char* key = JS_AtomToCString(ctx, props[i].atom);
                    JSValue val = JS_GetProperty(ctx, headersVal, props[i].atom);
                    const char* valStr = JS_ToCString(ctx, val);
                    if (key && valStr) {
                        std::string header = std::string(key) + ": " + valStr;
                        req->requestHeaders = curl_slist_append(req->requestHeaders, header.c_str());
                    }
                    if (valStr) JS_FreeCString(ctx, valStr);
                    if (key) JS_FreeCString(ctx, key);
                    JS_FreeValue(ctx, val);
                    JS_FreeAtom(ctx, props[i].atom);
                }
                js_free(ctx, props);
            }
        }
        JS_FreeValue(ctx, headersVal);

        // body
        JSValue bodyVal = JS_GetPropertyStr(ctx, argv[1], "body");
        if (JS_IsString(bodyVal)) {
            const char* body = JS_ToCString(ctx, bodyVal);
            if (body) {
                req->requestBody = body;
                JS_FreeCString(ctx, body);
            }
        }
        JS_FreeValue(ctx, bodyVal);

        if (req->requestHeaders) {
            curl_easy_setopt(req->easy, CURLOPT_HTTPHEADER, req->requestHeaders);
        }

        if (!req->requestBody.empty()) {
            curl_easy_setopt(req->easy, CURLOPT_POSTFIELDS, req->requestBody.c_str());
            curl_easy_setopt(req->easy, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(req->requestBody.size()));
        }
    }

    // Create Promise
    JSValue promise = JS_NewPromiseCapability(ctx, req->resolving);
    if (JS_IsException(promise)) {
        curl_easy_cleanup(req->easy);
        delete req;
        return promise;
    }

    // Add to multi handle
    if (!g_multi) {
        g_multi = curl_multi_init();
    }
    curl_multi_add_handle(g_multi, req->easy);
    g_pending.push_back(req);

    return promise;
}

// Check if there are pending fetch requests
static JSValue js_fetch_has_pending(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    return JS_NewBool(ctx, !g_pending.empty());
}

void installFetch(JSContext* ctx)
{
    static bool curlInited = false;
    if (!curlInited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curlInited = true;
    }

    JSValue global = JS_GetGlobalObject(ctx);

    JS_SetPropertyStr(ctx, global, "fetch",
                      JS_NewCFunction(ctx, js_fetch, "fetch", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_fetch_tick",
                      JS_NewCFunction(ctx, js_fetch_tick, "__brokit_fetch_tick", 0));
    JS_SetPropertyStr(ctx, global, "__brokit_fetch_has_pending",
                      JS_NewCFunction(ctx, js_fetch_has_pending, "__brokit_fetch_has_pending", 0));

    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
