#include "api/api.h"
#include "runtime/runtime.h"

#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <algorithm>

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

    // Response data — full body accumulation (for .text()/.json() shortcut)
    std::vector<uint8_t> body;
    std::vector<std::string> headers;
    long statusCode = 0;
    std::string statusText;
    std::string url;

    // Request body for POST etc.
    std::string requestBody;
    struct curl_slist* requestHeaders = nullptr;

    // Streaming support
    int streamId = 0;
    bool headersResolved = false;
    bool bodyComplete = false;
    bool hasReceivedData = false;
    std::vector<std::vector<uint8_t>> chunks;  // individual chunks for streaming
    JSValue waitCallback = JS_UNDEFINED;        // called when data/done available

    ~FetchRequest() {
        if (requestHeaders) curl_slist_free_all(requestHeaders);
    }
};

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static CURLM* g_multi = nullptr;
static std::vector<FetchRequest*> g_pending;        // active curl handles
static std::unordered_map<int, FetchRequest*> g_streams; // stream ID -> request
static int g_nextStreamId = 1;

// ---------------------------------------------------------------------------
// Per-context fetch base path stack (last added = checked first)
// ---------------------------------------------------------------------------
static const char* kFetchBasePathsKey = "__brokit_fetch_base_paths";

static std::vector<std::string> getBasePaths(JSContext* ctx)
{
    std::vector<std::string> paths;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue arr = JS_GetPropertyStr(ctx, global, kFetchBasePathsKey);
    if (JS_IsArray(arr)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, arr, i);
            const char* s = JS_ToCString(ctx, elem);
            if (s) { paths.emplace_back(s); JS_FreeCString(ctx, s); }
            JS_FreeValue(ctx, elem);
        }
    }
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, global);
    return paths;
}

// ---------------------------------------------------------------------------
// Local file helpers
// ---------------------------------------------------------------------------
static bool isHttpUrl(const std::string& url)
{
    return url.size() > 7 &&
           (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0);
}

static std::string detectMimeType(const std::string& path)
{
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css")  return "text/css; charset=utf-8";
    if (ext == "js" || ext == "mjs") return "application/javascript; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")  return "image/gif";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "webp") return "image/webp";
    if (ext == "woff") return "font/woff";
    if (ext == "woff2") return "font/woff2";
    if (ext == "ttf")  return "font/ttf";
    if (ext == "otf")  return "font/otf";
    if (ext == "txt")  return "text/plain; charset=utf-8";
    if (ext == "xml")  return "application/xml";
    if (ext == "wasm") return "application/wasm";
    return "application/octet-stream";
}

static std::string resolveLocalPath(JSContext* ctx, const std::string& url)
{
    // Strip leading ./
    std::string clean = url;
    if (clean.size() >= 2 && clean[0] == '.' && clean[1] == '/')
        clean = clean.substr(2);

    // Already absolute? (Windows drive letter or Unix root)
    if (clean.size() >= 2 && clean[1] == ':') return clean;
    if (!clean.empty() && (clean[0] == '/' || clean[0] == '\\')) {
        // Absolute path — still search base paths (treat as relative to roots)
        clean = clean.substr(1);
    }

    // Search base paths (last added first = overlay)
    auto paths = getBasePaths(ctx);
    for (int i = static_cast<int>(paths.size()) - 1; i >= 0; i--) {
        std::string candidate = paths[i];
        if (!candidate.empty() && candidate.back() != '/' && candidate.back() != '\\')
            candidate += '/';
        candidate += clean;
        std::ifstream test(candidate, std::ios::binary);
        if (test.good()) return candidate;
    }

    // Fallback: return as-is (will fail to open)
    return clean;
}

// Forward declaration — defined later in the file
static JSValue buildHeaders(JSContext* ctx, FetchRequest* req);

// Build a Response for a local file read
static JSValue buildFileResponse(JSContext* ctx, const std::string& url,
                                  const std::string& resolvedPath)
{
    std::ifstream file(resolvedPath, std::ios::in | std::ios::binary | std::ios::ate);
    if (!file) {
        // 404 — resolve promise with a not-ok Response (matches browser behavior)
        JSValue resp = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, 404));
        JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, "Not Found"));
        JS_SetPropertyStr(ctx, resp, "ok", JS_FALSE);
        JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, url.c_str()));

        // Empty headers
        FetchRequest emptyReq;
        JS_SetPropertyStr(ctx, resp, "headers", buildHeaders(ctx, &emptyReq));

        // Body methods that reject
        const char* notFoundBody = R"JS(
(function(resp) {
    resp.bodyUsed = false;
    resp.body = null;
    resp.text = function() { return Promise.resolve(''); };
    resp.json = function() { return Promise.reject(new SyntaxError('Not Found')); };
    resp.arrayBuffer = function() { return Promise.resolve(new ArrayBuffer(0)); };
    resp.blob = function() { return Promise.resolve(new Blob([])); };
    resp.clone = function() { return Object.assign(Object.create(null), resp); };
})
)JS";
        JSValue fn = JS_Eval(ctx, notFoundBody, strlen(notFoundBody), "<fetch-404>", JS_EVAL_TYPE_GLOBAL);
        JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &resp);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, fn);
        return resp;
    }

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);

    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, 200));
    JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, "OK"));
    JS_SetPropertyStr(ctx, resp, "ok", JS_TRUE);
    JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, url.c_str()));

    // Build headers with content-type and content-length
    std::string mime = detectMimeType(resolvedPath);
    // We build a fake header list to reuse buildHeaders
    FetchRequest fakeReq;
    fakeReq.headers.push_back("content-type: " + mime);
    fakeReq.headers.push_back("content-length: " + std::to_string(data.size()));
    JS_SetPropertyStr(ctx, resp, "headers", buildHeaders(ctx, &fakeReq));

    JSValue bodyAB = JS_NewArrayBufferCopy(ctx, data.data(), data.size());
    JS_SetPropertyStr(ctx, resp, "__body", bodyAB);

    const char* bodyMethods = R"JS(
(function(resp) {
    resp.bodyUsed = false;
    resp.body = null;
    resp.text = function() {
        resp.bodyUsed = true;
        return Promise.resolve(new TextDecoder().decode(new Uint8Array(this.__body)));
    };
    resp.json = function() {
        return this.text().then(function(t) { return JSON.parse(t); });
    };
    resp.arrayBuffer = function() {
        resp.bodyUsed = true;
        return Promise.resolve(this.__body.slice(0));
    };
    resp.blob = function() {
        var ct = resp.headers.get('content-type') || '';
        resp.bodyUsed = true;
        return Promise.resolve(new Blob([new Uint8Array(this.__body)], { type: ct }));
    };
    resp.clone = function() {
        var r = Object.assign(Object.create(null), this);
        r.__body = this.__body.slice(0);
        return r;
    };
})
)JS";
    JSValue fn = JS_Eval(ctx, bodyMethods, strlen(bodyMethods), "<fetch-file-body>", JS_EVAL_TYPE_GLOBAL);
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &resp);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, fn);

    return resp;
}

static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* req = static_cast<FetchRequest*>(userdata);
    size_t bytes = size * nmemb;
    // Accumulate full body (for .text()/.json() on complete responses)
    req->body.insert(req->body.end(), ptr, ptr + bytes);
    // Push individual chunk for streaming
    req->chunks.emplace_back(ptr, ptr + bytes);
    req->hasReceivedData = true;
    return bytes;
}

static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata)
{
    auto* req = static_cast<FetchRequest*>(userdata);
    size_t bytes = size * nitems;
    std::string line(buffer, bytes);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    if (!line.empty()) {
        if (line.find("HTTP/") == 0) {
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

// Build headers JS object (shared between streaming and complete responses)
static JSValue buildHeaders(JSContext* ctx, FetchRequest* req)
{
    JSValue hdrs = JS_NewObject(ctx);
    for (auto& h : req->headers) {
        auto colon = h.find(':');
        if (colon != std::string::npos) {
            std::string name = h.substr(0, colon);
            std::string value = h.substr(colon + 1);
            while (!value.empty() && value[0] == ' ') value.erase(0, 1);
            for (auto& c : name) c = static_cast<char>(tolower(c));
            JS_SetPropertyStr(ctx, hdrs, name.c_str(), JS_NewString(ctx, value.c_str()));
        }
    }

    const char* headersPolyfill = R"JS(
(function(entries) {
    var obj = {
        get: function(name) { return entries[name.toLowerCase()] || null; },
        has: function(name) { return entries[name.toLowerCase()] !== undefined; },
        forEach: function(cb) {
            var keys = Object.keys(entries);
            for (var i = 0; i < keys.length; i++) cb(entries[keys[i]], keys[i], this);
        },
        entries: function() {
            var keys = Object.keys(entries); var i = 0;
            return { next: function() {
                if (i >= keys.length) return { done: true };
                var k = keys[i++]; return { done: false, value: [k, entries[k]] };
            }, [Symbol.iterator]: function() { return this; } };
        },
        keys: function() {
            var keys = Object.keys(entries); var i = 0;
            return { next: function() {
                if (i >= keys.length) return { done: true };
                return { done: false, value: keys[i++] };
            }, [Symbol.iterator]: function() { return this; } };
        },
        values: function() {
            var keys = Object.keys(entries); var i = 0;
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

    JSValue headersFn = JS_Eval(ctx, headersPolyfill, strlen(headersPolyfill),
                                 "<fetch-headers>", JS_EVAL_TYPE_GLOBAL);
    JSValue headersObj = JS_Call(ctx, headersFn, JS_UNDEFINED, 1, &hdrs);
    JS_FreeValue(ctx, headersFn);
    JS_FreeValue(ctx, hdrs);
    return headersObj;
}

// Build a Response object for a streaming response (resolved early, body not complete)
static JSValue buildStreamingResponse(JSContext* ctx, FetchRequest* req)
{
    JSValue resp = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, static_cast<int>(req->statusCode)));
    JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, req->statusText.c_str()));
    JS_SetPropertyStr(ctx, resp, "ok", JS_NewBool(ctx, req->statusCode >= 200 && req->statusCode < 300));
    JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, req->url.c_str()));
    JS_SetPropertyStr(ctx, resp, "headers", buildHeaders(ctx, req));
    JS_SetPropertyStr(ctx, resp, "__streamId", JS_NewInt32(ctx, req->streamId));

    // Install body methods via JS — uses ReadableStream for body
    const char* bodySetup = R"JS(
(function(resp) {
    var streamId = resp.__streamId;

    // Create ReadableStream body that pulls from native chunk buffer
    resp.body = new ReadableStream({
        pull: function(controller) {
            return new Promise(function(resolve) {
                function tryRead() {
                    var result = globalThis.__brokit_fetch_stream_read(streamId);
                    if (result === null) {
                        // No data yet — register for notification
                        globalThis.__brokit_fetch_stream_wait(streamId, function() {
                            tryRead();
                        });
                        return;
                    }
                    if (result.done) {
                        controller.close();
                        resolve();
                        return;
                    }
                    controller.enqueue(result.value);
                    resolve();
                }
                tryRead();
            });
        },
        cancel: function() {
            // Best effort — data already in flight
        }
    });
    resp.bodyUsed = false;

    function consumeBody() {
        if (resp.bodyUsed) return Promise.reject(new TypeError('Body already consumed'));
        resp.bodyUsed = true;
        var reader = resp.body.getReader();
        var chunks = [];
        function pump() {
            return reader.read().then(function(result) {
                if (result.done) {
                    var totalLen = 0;
                    for (var i = 0; i < chunks.length; i++) totalLen += chunks[i].byteLength;
                    var merged = new Uint8Array(totalLen);
                    var offset = 0;
                    for (var i = 0; i < chunks.length; i++) {
                        merged.set(new Uint8Array(chunks[i].buffer || chunks[i]), offset);
                        offset += chunks[i].byteLength;
                    }
                    return merged;
                }
                chunks.push(result.value);
                return pump();
            });
        }
        return pump();
    }

    resp.text = function() {
        return consumeBody().then(function(bytes) {
            return new TextDecoder().decode(bytes);
        });
    };
    resp.json = function() {
        return resp.text().then(function(t) { return JSON.parse(t); });
    };
    resp.arrayBuffer = function() {
        return consumeBody().then(function(bytes) { return bytes.buffer; });
    };
    resp.blob = function() {
        var ct = resp.headers.get('content-type') || '';
        return consumeBody().then(function(bytes) {
            return new Blob([bytes], { type: ct });
        });
    };
    resp.clone = function() {
        throw new TypeError('Cannot clone a streaming response');
    };
})
)JS";

    JSValue bodyFn = JS_Eval(ctx, bodySetup, strlen(bodySetup),
                              "<fetch-stream-body>", JS_EVAL_TYPE_GLOBAL);
    JSValue ret = JS_Call(ctx, bodyFn, JS_UNDEFINED, 1, &resp);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, bodyFn);

    return resp;
}

// Build a Response for a completed response (all body available, backward compat)
static JSValue buildCompleteResponse(JSContext* ctx, FetchRequest* req)
{
    JSValue resp = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, static_cast<int>(req->statusCode)));
    JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, req->statusText.c_str()));
    JS_SetPropertyStr(ctx, resp, "ok", JS_NewBool(ctx, req->statusCode >= 200 && req->statusCode < 300));
    JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, req->url.c_str()));
    JS_SetPropertyStr(ctx, resp, "headers", buildHeaders(ctx, req));

    // Full body as ArrayBuffer
    JSValue bodyAB = JS_NewArrayBufferCopy(ctx, req->body.data(), req->body.size());
    JS_SetPropertyStr(ctx, resp, "__body", bodyAB);
    JS_SetPropertyStr(ctx, resp, "__streamId", JS_NewInt32(ctx, req->streamId));

    // Body methods: use __body for fast synchronous access, but also provide .body stream
    const char* bodyMethods = R"JS(
(function(resp) {
    var streamId = resp.__streamId;
    resp.bodyUsed = false;

    // body stream from already-complete data
    resp.body = new ReadableStream({
        pull: function(controller) {
            var result = globalThis.__brokit_fetch_stream_read(streamId);
            if (result === null || result.done) {
                controller.close();
                return;
            }
            controller.enqueue(result.value);
        }
    });

    resp.text = function() {
        resp.bodyUsed = true;
        var decoder = new TextDecoder();
        var text = decoder.decode(new Uint8Array(this.__body));
        return Promise.resolve(text);
    };
    resp.json = function() {
        return this.text().then(function(t) { return JSON.parse(t); });
    };
    resp.arrayBuffer = function() {
        resp.bodyUsed = true;
        return Promise.resolve(this.__body.slice(0));
    };
    resp.blob = function() {
        var ct = this.headers.get('content-type') || '';
        resp.bodyUsed = true;
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

// ---------------------------------------------------------------------------
// Stream natives
// ---------------------------------------------------------------------------

// __brokit_fetch_stream_read(streamId) → {value: Uint8Array, done: false} | {done: true} | null
static JSValue js_fetch_stream_read(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NULL;
    int streamId = 0;
    JS_ToInt32(ctx, &streamId, argv[0]);

    auto it = g_streams.find(streamId);
    if (it == g_streams.end()) {
        // Stream gone — return done
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "done", JS_TRUE);
        JS_SetPropertyStr(ctx, obj, "value", JS_UNDEFINED);
        return obj;
    }

    FetchRequest* req = it->second;

    if (!req->chunks.empty()) {
        auto chunk = std::move(req->chunks.front());
        req->chunks.erase(req->chunks.begin());

        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "done", JS_FALSE);
        JSValue u8 = JS_NewUint8ArrayCopy(ctx, chunk.data(), chunk.size());
        JS_SetPropertyStr(ctx, obj, "value", u8);
        return obj;
    }

    if (req->bodyComplete) {
        // All data consumed and body complete — done
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "done", JS_TRUE);
        JS_SetPropertyStr(ctx, obj, "value", JS_UNDEFINED);
        // Clean up stream
        if (req->headersResolved && !req->easy) {
            // Fully done and curl cleaned up — safe to delete
            g_streams.erase(it);
            delete req;
        }
        return obj;
    }

    // No data yet
    return JS_NULL;
}

// __brokit_fetch_stream_wait(streamId, callback) — register callback for when data arrives
static JSValue js_fetch_stream_wait(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_UNDEFINED;
    int streamId = 0;
    JS_ToInt32(ctx, &streamId, argv[0]);

    auto it = g_streams.find(streamId);
    if (it == g_streams.end()) return JS_UNDEFINED;

    FetchRequest* req = it->second;

    // Free previous callback if any
    if (JS_IsFunction(ctx, req->waitCallback)) {
        JS_FreeValue(ctx, req->waitCallback);
    }
    req->waitCallback = JS_DupValue(ctx, argv[1]);

    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Tick: pump curl_multi, resolve streaming responses, notify waiting readers
// ---------------------------------------------------------------------------
static JSValue js_fetch_tick(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (!g_multi || g_pending.empty()) return JS_NewInt32(ctx, 0);

    int running = 0;
    curl_multi_perform(g_multi, &running);

    // Phase 1: For streaming requests that have received data but haven't resolved yet,
    // resolve the fetch Promise with a streaming Response (early resolution)
    for (auto* req : g_pending) {
        if (!req->headersResolved && req->hasReceivedData) {
            curl_easy_getinfo(req->easy, CURLINFO_RESPONSE_CODE, &req->statusCode);
            char* effectiveUrl = nullptr;
            curl_easy_getinfo(req->easy, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
            if (effectiveUrl) req->url = effectiveUrl;

            JSValue response = buildStreamingResponse(ctx, req);
            JSValue ret = JS_Call(ctx, req->resolving[0], JS_UNDEFINED, 1, &response);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, response);
            JS_FreeValue(ctx, req->resolving[0]);
            JS_FreeValue(ctx, req->resolving[1]);
            req->resolving[0] = JS_UNDEFINED;
            req->resolving[1] = JS_UNDEFINED;
            req->headersResolved = true;
        }
    }

    // Phase 2: Notify waiting stream readers that data is available
    for (auto& [id, req] : g_streams) {
        if ((!req->chunks.empty() || req->bodyComplete) &&
            JS_IsFunction(ctx, req->waitCallback)) {
            JSValue cb = req->waitCallback;
            req->waitCallback = JS_UNDEFINED;
            JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, cb);
        }
    }

    int completed = 0;

    // Phase 3: Handle completed requests
    CURLMsg* msg;
    int msgs_in_queue;
    while ((msg = curl_multi_info_read(g_multi, &msgs_in_queue))) {
        if (msg->msg != CURLMSG_DONE) continue;

        CURL* easy = msg->easy_handle;
        FetchRequest* req = nullptr;

        // Find and remove from g_pending
        for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
            if ((*it)->easy == easy) {
                req = *it;
                g_pending.erase(it);
                break;
            }
        }
        if (!req) continue;

        curl_multi_remove_handle(g_multi, easy);
        req->bodyComplete = true;

        if (!req->headersResolved) {
            // Request completed before we resolved (small response, or error)
            if (msg->data.result == CURLE_OK) {
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &req->statusCode);
                char* effectiveUrl = nullptr;
                curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
                if (effectiveUrl) req->url = effectiveUrl;

                // Build complete response (backward compat — body fully available)
                JSValue response = buildCompleteResponse(ctx, req);
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
            req->resolving[0] = JS_UNDEFINED;
            req->resolving[1] = JS_UNDEFINED;
            req->headersResolved = true;
        }

        // Notify waiting stream reader that body is complete
        if (JS_IsFunction(ctx, req->waitCallback)) {
            JSValue cb = req->waitCallback;
            req->waitCallback = JS_UNDEFINED;
            JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, cb);
        }

        curl_easy_cleanup(easy);
        req->easy = nullptr;

        // If no stream was ever set up (error case), delete now
        if (g_streams.find(req->streamId) == g_streams.end()) {
            delete req;
        }
        // Otherwise, req stays alive in g_streams until JS drains it

        completed++;
    }

    return JS_NewInt32(ctx, completed);
}

// ---------------------------------------------------------------------------
// fetch(url, options?) — returns Promise
// ---------------------------------------------------------------------------
static JSValue js_fetch(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "fetch: URL required");

    const char* urlStr = JS_ToCString(ctx, argv[0]);
    if (!urlStr) return JS_EXCEPTION;
    std::string url(urlStr);
    JS_FreeCString(ctx, urlStr);

    // Local file fetch — anything that isn't http:// or https://
    if (!isHttpUrl(url)) {
        std::string resolved = resolveLocalPath(ctx, url);
        JSValue response = buildFileResponse(ctx, url, resolved);
        // Wrap in a resolved Promise to match fetch() API
        JSValue resolving[2];
        JSValue promise = JS_NewPromiseCapability(ctx, resolving);
        if (JS_IsException(promise)) {
            JS_FreeValue(ctx, response);
            return promise;
        }
        JSValue ret = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &response);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, response);
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        return promise;
    }

    // HTTP fetch via libcurl
    auto* req = new FetchRequest();
    req->ctx = ctx;
    req->url = url;

    // Assign stream ID and register
    req->streamId = g_nextStreamId++;
    g_streams[req->streamId] = req;

    req->easy = curl_easy_init();
    if (!req->easy) {
        g_streams.erase(req->streamId);
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
    curl_easy_setopt(req->easy, CURLOPT_USERAGENT, "brokit/0.3");
    curl_easy_setopt(req->easy, CURLOPT_ACCEPT_ENCODING, "");

    // Process options
    if (argc >= 2 && JS_IsObject(argv[1])) {
        // method
        JSValue methodVal = JS_GetPropertyStr(ctx, argv[1], "method");
        if (JS_IsString(methodVal)) {
            const char* method = JS_ToCString(ctx, methodVal);
            if (method) {
                curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, method);
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
        g_streams.erase(req->streamId);
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

// Check if there are pending fetch requests or active streams
static JSValue js_fetch_has_pending(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (!g_pending.empty()) return JS_NewBool(ctx, true);
    // Also check for streams with pending wait callbacks (reader still consuming)
    for (auto& [id, req] : g_streams) {
        if (JS_IsFunction(ctx, req->waitCallback)) return JS_NewBool(ctx, true);
    }
    return JS_NewBool(ctx, false);
}

void installFetch(JSContext* ctx)
{
    static bool curlInited = false;
    if (!curlInited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curlInited = true;
    }

    JSValue global = JS_GetGlobalObject(ctx);

    // Initialize the base path array
    JS_SetPropertyStr(ctx, global, kFetchBasePathsKey, JS_NewArray(ctx));

    JS_SetPropertyStr(ctx, global, "fetch",
                      JS_NewCFunction(ctx, js_fetch, "fetch", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_fetch_tick",
                      JS_NewCFunction(ctx, js_fetch_tick, "__brokit_fetch_tick", 0));
    JS_SetPropertyStr(ctx, global, "__brokit_fetch_has_pending",
                      JS_NewCFunction(ctx, js_fetch_has_pending, "__brokit_fetch_has_pending", 0));
    JS_SetPropertyStr(ctx, global, "__brokit_fetch_stream_read",
                      JS_NewCFunction(ctx, js_fetch_stream_read, "__brokit_fetch_stream_read", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_fetch_stream_wait",
                      JS_NewCFunction(ctx, js_fetch_stream_wait, "__brokit_fetch_stream_wait", 2));

    JS_FreeValue(ctx, global);
}

void addFetchBasePath(JSContext* ctx, const std::string& path)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue arr = JS_GetPropertyStr(ctx, global, kFetchBasePathsKey);
    if (!JS_IsArray(arr)) {
        // installFetch not called yet — create the array
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, kFetchBasePathsKey, JS_DupValue(ctx, arr));
    }
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    JS_SetPropertyUint32(ctx, arr, len, JS_NewString(ctx, path.c_str()));
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
