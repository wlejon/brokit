#include "api/api.h"
#include "runtime/runtime.h"
#include "fetch_helpers.js.h"

#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <fstream>
#include <algorithm>

#include <curl/curl.h>

namespace brokit::api {

// ---------------------------------------------------------------------------
// Per-request state
// ---------------------------------------------------------------------------
struct FetchRequest {
    CURL* easy = nullptr;

    JSValue resolving[2] = { JS_UNDEFINED, JS_UNDEFINED };
    JSContext* ctx = nullptr;

    std::vector<uint8_t> body;
    std::vector<std::string> headers;
    long statusCode = 0;
    std::string statusText;
    std::string url;

    std::vector<uint8_t> requestBody;
    struct curl_slist* requestHeaders = nullptr;

    int streamId = 0;
    bool headersResolved = false;
    bool bodyComplete = false;
    bool hasReceivedData = false;
    std::vector<std::vector<uint8_t>> chunks;
    JSValue waitCallback = JS_UNDEFINED;

    ~FetchRequest() {
        if (requestHeaders) curl_slist_free_all(requestHeaders);
    }
};

// ---------------------------------------------------------------------------
// Per-context state. Mirrors the pattern used in fs_watch.cpp.
// ---------------------------------------------------------------------------
namespace {

// Ownership model: `streams` owns every live FetchRequest (keyed by streamId).
// `pending` is a non-owning working set of in-flight curl handles consulted
// each tick(); entries are removed from `pending` when curl reports DONE,
// but the request stays in `streams` until JS finishes draining the body.
struct CtxState {
    CURLM* multi = nullptr;
    std::unordered_map<int, std::unique_ptr<FetchRequest>> streams;
    std::vector<FetchRequest*> pending; // non-owning view into streams
    int nextStreamId = 1;
};

static std::unordered_map<JSContext*, CtxState> g_state;

CtxState& stateOf(JSContext* ctx) { return g_state[ctx]; }
CtxState* findState(JSContext* ctx) {
    auto it = g_state.find(ctx);
    return it == g_state.end() ? nullptr : &it->second;
}

void removePending(CtxState& s, FetchRequest* req) {
    for (auto it = s.pending.begin(); it != s.pending.end(); ++it) {
        if (*it == req) { s.pending.erase(it); return; }
    }
}

} // namespace

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
// Helpers backed by JS factories from fetch_helpers.js
// ---------------------------------------------------------------------------
static JSValue callInternal(JSContext* ctx, const char* fnName,
                            int argc, JSValueConst* argv)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue internals = JS_GetPropertyStr(ctx, global, "__brokit_fetch_internals");
    JS_FreeValue(ctx, global);
    if (!JS_IsObject(internals)) {
        JS_FreeValue(ctx, internals);
        return JS_ThrowInternalError(ctx, "fetch: internals not installed");
    }
    JSValue fn = JS_GetPropertyStr(ctx, internals, fnName);
    JS_FreeValue(ctx, internals);
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        return JS_ThrowInternalError(ctx, "fetch: missing internal helper '%s'", fnName);
    }
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
    JS_FreeValue(ctx, fn);
    return ret;
}

// ---------------------------------------------------------------------------
// Local file helpers
// ---------------------------------------------------------------------------
static bool isHttpUrl(const std::string& url)
{
    return url.size() > 7 &&
           (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0);
}

static bool isDataUrl(const std::string& url)
{
    return url.size() >= 5 && url.compare(0, 5, "data:") == 0;
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

// Defined in fs.cpp — engine prefix mounts (e.g. /lib, /system).
extern std::string resolveBrokitPrefixMount(JSContext* ctx, const std::string& path);

static std::string resolveLocalPath(JSContext* ctx, const std::string& url)
{
    std::string mounted = resolveBrokitPrefixMount(ctx, url);
    if (!mounted.empty()) return mounted;

    std::string clean = url;
    if (clean.size() >= 2 && clean[0] == '.' && clean[1] == '/')
        clean = clean.substr(2);

    if (clean.size() >= 2 && clean[1] == ':') return clean;
    if (!clean.empty() && (clean[0] == '/' || clean[0] == '\\')) {
        clean = clean.substr(1);
    }

    auto paths = getBasePaths(ctx);
    for (int i = static_cast<int>(paths.size()) - 1; i >= 0; i--) {
        std::string candidate = paths[i];
        if (!candidate.empty() && candidate.back() != '/' && candidate.back() != '\\')
            candidate += '/';
        candidate += clean;
        std::ifstream test(candidate, std::ios::binary);
        if (test.good()) return candidate;
    }

    return clean;
}

// Build a Headers-like JS object from a flat header list ("name: value" lines).
static JSValue buildHeaders(JSContext* ctx, const std::vector<std::string>& headers)
{
    JSValue hdrs = JS_NewObject(ctx);
    for (auto& h : headers) {
        auto colon = h.find(':');
        if (colon != std::string::npos) {
            std::string name = h.substr(0, colon);
            std::string value = h.substr(colon + 1);
            while (!value.empty() && value[0] == ' ') value.erase(0, 1);
            for (auto& c : name) c = static_cast<char>(tolower(c));
            JS_SetPropertyStr(ctx, hdrs, name.c_str(), JS_NewString(ctx, value.c_str()));
        }
    }

    JSValueConst args[1] = { hdrs };
    JSValue result = callInternal(ctx, "headers", 1, args);
    JS_FreeValue(ctx, hdrs);
    return result;
}

// Build a Response for a data: URL — RFC 2397.
// Format: data:[<mediatype>][;base64],<data>
static JSValue buildDataUrlResponse(JSContext* ctx, const std::string& url)
{
    std::string rest = url.substr(5); // strip "data:"
    auto comma = rest.find(',');

    std::string meta;
    std::string payload;
    if (comma == std::string::npos) {
        // Malformed — no comma. Treat the whole thing as payload, no mime.
        payload = rest;
    } else {
        meta    = rest.substr(0, comma);
        payload = rest.substr(comma + 1);
    }

    bool isBase64 = false;
    std::string mime = "text/plain;charset=US-ASCII";
    if (!meta.empty()) {
        const std::string b64Tag = ";base64";
        if (meta.size() >= b64Tag.size() &&
            meta.compare(meta.size() - b64Tag.size(), b64Tag.size(), b64Tag) == 0) {
            isBase64 = true;
            meta = meta.substr(0, meta.size() - b64Tag.size());
        }
        if (!meta.empty()) mime = meta;
    }

    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::vector<uint8_t> data;
    if (isBase64) {
        // Strip whitespace and decode standard base64.
        static const int8_t tbl[128] = {
            // 0..63 mapping for A-Z, a-z, 0-9, +, /  (others -1)
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
            52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
            41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        };
        uint32_t buf = 0;
        int bits = 0;
        for (char c : payload) {
            if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
            unsigned uc = static_cast<unsigned char>(c);
            if (uc >= 128) continue;
            int v = tbl[uc];
            if (v < 0) continue;
            buf = (buf << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                data.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
            }
        }
    } else {
        data.reserve(payload.size());
        for (size_t i = 0; i < payload.size(); ++i) {
            if (payload[i] == '%' && i + 2 < payload.size()) {
                int hi = hexVal(payload[i+1]);
                int lo = hexVal(payload[i+2]);
                if (hi >= 0 && lo >= 0) {
                    data.push_back(static_cast<uint8_t>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            data.push_back(static_cast<uint8_t>(payload[i]));
        }
    }

    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, 200));
    JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, "OK"));
    JS_SetPropertyStr(ctx, resp, "ok", JS_TRUE);
    JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, url.c_str()));

    std::vector<std::string> headerLines = {
        "content-type: " + mime,
        "content-length: " + std::to_string(data.size()),
    };
    JS_SetPropertyStr(ctx, resp, "headers", buildHeaders(ctx, headerLines));

    JSValue bodyAB = JS_NewArrayBufferCopy(ctx, data.data(), data.size());
    JS_SetPropertyStr(ctx, resp, "__body", bodyAB);

    JSValueConst args[1] = { resp };
    JSValue ret = callInternal(ctx, "applyFileBody", 1, args);
    JS_FreeValue(ctx, ret);
    return resp;
}

// Build a Response for a local file read
static JSValue buildFileResponse(JSContext* ctx, const std::string& url,
                                  const std::string& resolvedPath)
{
    std::ifstream file(resolvedPath, std::ios::in | std::ios::binary | std::ios::ate);
    if (!file) {
        // 404 — resolve promise with a not-ok Response (matches browser behavior
        // for HTTP missing resources).
        JSValue resp = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, 404));
        JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, "Not Found"));
        JS_SetPropertyStr(ctx, resp, "ok", JS_FALSE);
        JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, url.c_str()));
        JS_SetPropertyStr(ctx, resp, "headers", buildHeaders(ctx, {}));

        JSValueConst args[1] = { resp };
        JSValue ret = callInternal(ctx, "applyNotFoundBody", 1, args);
        JS_FreeValue(ctx, ret);
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

    std::string mime = detectMimeType(resolvedPath);
    std::vector<std::string> headerLines = {
        "content-type: " + mime,
        "content-length: " + std::to_string(data.size()),
    };
    JS_SetPropertyStr(ctx, resp, "headers", buildHeaders(ctx, headerLines));

    JSValue bodyAB = JS_NewArrayBufferCopy(ctx, data.data(), data.size());
    JS_SetPropertyStr(ctx, resp, "__body", bodyAB);

    JSValueConst args[1] = { resp };
    JSValue ret = callInternal(ctx, "applyFileBody", 1, args);
    JS_FreeValue(ctx, ret);
    return resp;
}

static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* req = static_cast<FetchRequest*>(userdata);
    size_t bytes = size * nmemb;
    req->body.insert(req->body.end(), ptr, ptr + bytes);
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

// Build a Response object for a streaming response (resolved early, body not complete)
static JSValue buildStreamingResponse(JSContext* ctx, FetchRequest* req)
{
    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, static_cast<int>(req->statusCode)));
    JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, req->statusText.c_str()));
    JS_SetPropertyStr(ctx, resp, "ok", JS_NewBool(ctx, req->statusCode >= 200 && req->statusCode < 300));
    JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, req->url.c_str()));
    JS_SetPropertyStr(ctx, resp, "headers", buildHeaders(ctx, req->headers));
    JS_SetPropertyStr(ctx, resp, "__streamId", JS_NewInt32(ctx, req->streamId));

    JSValueConst args[1] = { resp };
    JSValue ret = callInternal(ctx, "applyStreamingBody", 1, args);
    JS_FreeValue(ctx, ret);
    return resp;
}

// Build a Response for a completed response (all body available)
static JSValue buildCompleteResponse(JSContext* ctx, FetchRequest* req)
{
    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, static_cast<int>(req->statusCode)));
    JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, req->statusText.c_str()));
    JS_SetPropertyStr(ctx, resp, "ok", JS_NewBool(ctx, req->statusCode >= 200 && req->statusCode < 300));
    JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, req->url.c_str()));
    JS_SetPropertyStr(ctx, resp, "headers", buildHeaders(ctx, req->headers));

    JSValue bodyAB = JS_NewArrayBufferCopy(ctx, req->body.data(), req->body.size());
    JS_SetPropertyStr(ctx, resp, "__body", bodyAB);
    JS_SetPropertyStr(ctx, resp, "__streamId", JS_NewInt32(ctx, req->streamId));

    JSValueConst args[1] = { resp };
    JSValue ret = callInternal(ctx, "applyCompleteBody", 1, args);
    JS_FreeValue(ctx, ret);
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

    CtxState* s = findState(ctx);
    if (!s) return JS_NULL;

    auto it = s->streams.find(streamId);
    if (it == s->streams.end() || !it->second) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "done", JS_TRUE);
        JS_SetPropertyStr(ctx, obj, "value", JS_UNDEFINED);
        return obj;
    }

    FetchRequest* req = it->second.get();

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
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "done", JS_TRUE);
        JS_SetPropertyStr(ctx, obj, "value", JS_UNDEFINED);
        if (req->headersResolved && !req->easy) {
            // Fully done and curl cleaned up — drop ownership; unique_ptr frees it.
            s->streams.erase(it);
        }
        return obj;
    }

    return JS_NULL;
}

// __brokit_fetch_stream_wait(streamId, callback) — register callback for when data arrives
static JSValue js_fetch_stream_wait(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_UNDEFINED;
    int streamId = 0;
    JS_ToInt32(ctx, &streamId, argv[0]);

    CtxState* s = findState(ctx);
    if (!s) return JS_UNDEFINED;

    auto it = s->streams.find(streamId);
    if (it == s->streams.end()) return JS_UNDEFINED;

    FetchRequest* req = it->second.get();
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
    CtxState* s = findState(ctx);
    if (!s || !s->multi || s->pending.empty()) return JS_NewInt32(ctx, 0);

    // Poll socket readiness for a small real-time window. curl_multi_perform
    // alone does not check FD signal state — without this, a non-blocking
    // connect that fails (e.g. connection refused) is never observed and the
    // request hangs forever. A 0ms poll is not enough either: the kernel
    // needs wall-clock time to detect TCP failure and update the socket
    // state. 50ms returns early on any activity, so the cost is only paid
    // when truly idle.
    curl_multi_poll(s->multi, nullptr, 0, 50, nullptr);

    int running = 0;
    curl_multi_perform(s->multi, &running);

    // Phase 1: resolve streaming responses that have received data.
    for (FetchRequest* req : s->pending) {
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

    // Phase 2: notify waiting stream readers.
    for (auto& [id, reqOwn] : s->streams) {
        FetchRequest* req = reqOwn.get();
        if (!req) continue;
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

    // Phase 3: handle completed requests.
    CURLMsg* msg;
    int msgs_in_queue;
    while ((msg = curl_multi_info_read(s->multi, &msgs_in_queue))) {
        if (msg->msg != CURLMSG_DONE) continue;

        CURL* easy = msg->easy_handle;
        FetchRequest* req = nullptr;
        for (FetchRequest* p : s->pending) {
            if (p->easy == easy) { req = p; break; }
        }
        if (!req) continue;
        removePending(*s, req);

        curl_multi_remove_handle(s->multi, easy);
        req->bodyComplete = true;

        // If a streaming Response was already handed to JS, JS owns body
        // draining via stream_read — we must keep the streams entry alive.
        // Otherwise everything finalizes here.
        bool wasStreaming = req->headersResolved;
        if (!req->headersResolved) {
            if (msg->data.result == CURLE_OK) {
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &req->statusCode);
                char* effectiveUrl = nullptr;
                curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
                if (effectiveUrl) req->url = effectiveUrl;

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

        if (JS_IsFunction(ctx, req->waitCallback)) {
            JSValue cb = req->waitCallback;
            req->waitCallback = JS_UNDEFINED;
            JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, cb);
        }

        curl_easy_cleanup(easy);
        req->easy = nullptr;

        // Drop the streams entry unless a streaming Response is still being
        // drained by JS via stream_read. In the non-streaming success case
        // the body lives in JS as `__body`; in the error case no Response
        // was produced. Either way the FetchRequest is safe to free now.
        if (!wasStreaming) {
            s->streams.erase(req->streamId);
        }

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

    if (!isHttpUrl(url)) {
        JSValue response;
        if (isDataUrl(url)) {
            response = buildDataUrlResponse(ctx, url);
        } else {
            std::string resolved = resolveLocalPath(ctx, url);
            response = buildFileResponse(ctx, url, resolved);
        }
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

    CtxState& s = stateOf(ctx);

    auto req = std::make_unique<FetchRequest>();
    req->ctx = ctx;
    req->url = url;
    req->streamId = s.nextStreamId++;

    req->easy = curl_easy_init();
    if (!req->easy) {
        return JS_ThrowInternalError(ctx, "fetch: curl_easy_init failed");
    }

    curl_easy_setopt(req->easy, CURLOPT_URL, req->url.c_str());
    curl_easy_setopt(req->easy, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(req->easy, CURLOPT_WRITEDATA, req.get());
    curl_easy_setopt(req->easy, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(req->easy, CURLOPT_HEADERDATA, req.get());
    curl_easy_setopt(req->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(req->easy, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(req->easy, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(req->easy, CURLOPT_USERAGENT, "brokit/0.3");
    curl_easy_setopt(req->easy, CURLOPT_ACCEPT_ENCODING, "");

    if (argc >= 2 && JS_IsObject(argv[1])) {
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

        JSValue bodyVal = JS_GetPropertyStr(ctx, argv[1], "body");
        if (JS_IsString(bodyVal)) {
            const char* body = JS_ToCString(ctx, bodyVal);
            if (body) {
                size_t len = strlen(body);
                req->requestBody.assign(
                    reinterpret_cast<const uint8_t*>(body),
                    reinterpret_cast<const uint8_t*>(body) + len);
                JS_FreeCString(ctx, body);
            }
        } else if (!JS_IsUndefined(bodyVal) && !JS_IsNull(bodyVal)) {
            size_t byte_offset = 0, byte_len = 0, bpe = 0;
            JSValue buf = JS_GetTypedArrayBuffer(ctx, bodyVal, &byte_offset, &byte_len, &bpe);
            if (!JS_IsException(buf)) {
                size_t abLen = 0;
                uint8_t* ptr = JS_GetArrayBuffer(ctx, &abLen, buf);
                if (ptr) {
                    req->requestBody.assign(ptr + byte_offset, ptr + byte_offset + byte_len);
                }
                JS_FreeValue(ctx, buf);
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
                size_t abLen = 0;
                uint8_t* ptr = JS_GetArrayBuffer(ctx, &abLen, bodyVal);
                if (ptr) {
                    req->requestBody.assign(ptr, ptr + abLen);
                }
            }
        }
        JS_FreeValue(ctx, bodyVal);

        if (req->requestHeaders) {
            curl_easy_setopt(req->easy, CURLOPT_HTTPHEADER, req->requestHeaders);
        }

        if (!req->requestBody.empty()) {
            curl_easy_setopt(req->easy, CURLOPT_POSTFIELDS, req->requestBody.data());
            curl_easy_setopt(req->easy, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(req->requestBody.size()));
        }
    }

    JSValue promise = JS_NewPromiseCapability(ctx, req->resolving);
    if (JS_IsException(promise)) {
        curl_easy_cleanup(req->easy);
        req->easy = nullptr;
        return promise;
    }

    if (!s.multi) {
        s.multi = curl_multi_init();
    }
    curl_multi_add_handle(s.multi, req->easy);

    int streamId = req->streamId;
    FetchRequest* raw = req.get();
    s.streams.emplace(streamId, std::move(req));
    s.pending.push_back(raw);

    return promise;
}

static JSValue js_fetch_has_pending(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    CtxState* s = findState(ctx);
    if (!s) return JS_NewBool(ctx, false);
    if (!s->pending.empty()) return JS_NewBool(ctx, true);
    for (auto& [id, req] : s->streams) {
        if (req && JS_IsFunction(ctx, req->waitCallback)) return JS_NewBool(ctx, true);
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

    // Touch the per-context state so it exists before any fetch() call.
    (void)stateOf(ctx);

    JSValue global = JS_GetGlobalObject(ctx);

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

    // Install the JS helpers — they expose globalThis.__brokit_fetch_internals
    // which callInternal() looks up on demand. Caching the JSValue in CtxState
    // would outlive JS_FreeContext and trip QuickJS's GC assertion.
    JSValue r = JS_Eval(ctx, js_fetch_helpers, strlen(js_fetch_helpers),
                        "<fetch_helpers>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    } else {
        JS_FreeValue(ctx, r);
    }
}

void uninstallFetch(JSContext* ctx)
{
    auto it = g_state.find(ctx);
    if (it == g_state.end()) return;
    CtxState& s = it->second;

    // Tear down curl handles still in flight, then free the JSValues we own
    // (promise resolvers, wait callbacks). After this loop, the unique_ptr
    // destruction below frees the FetchRequest objects themselves.
    for (auto& [id, reqOwn] : s.streams) {
        FetchRequest* req = reqOwn.get();
        if (!req) continue;
        if (req->easy) {
            if (s.multi) curl_multi_remove_handle(s.multi, req->easy);
            curl_easy_cleanup(req->easy);
            req->easy = nullptr;
        }
        if (!JS_IsUndefined(req->resolving[0])) JS_FreeValue(ctx, req->resolving[0]);
        if (!JS_IsUndefined(req->resolving[1])) JS_FreeValue(ctx, req->resolving[1]);
        if (JS_IsFunction(ctx, req->waitCallback)) JS_FreeValue(ctx, req->waitCallback);
        req->resolving[0] = JS_UNDEFINED;
        req->resolving[1] = JS_UNDEFINED;
        req->waitCallback = JS_UNDEFINED;
    }
    s.pending.clear();

    if (s.multi) {
        curl_multi_cleanup(s.multi);
        s.multi = nullptr;
    }

    g_state.erase(it);
}

void addFetchBasePath(JSContext* ctx, const std::string& path)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue arr = JS_GetPropertyStr(ctx, global, kFetchBasePathsKey);
    if (!JS_IsArray(arr)) {
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
