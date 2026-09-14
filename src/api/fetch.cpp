#include "api/api.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"

#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <span>
#include <array>

#include <curl/curl.h>

extern "C" void bronze_fetch_helpers_main();

namespace brokit::api {

// ---------------------------------------------------------------------------
// Per-request state
// ---------------------------------------------------------------------------
struct FetchRequest {
    CURL* easy = nullptr;

    PersistentSlot promise;

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
    PersistentSlot waitCallback;

    // A file:, data: or blob: response, already built, waiting for the next
    // tick to settle the promise. Held here rather than resolved in fetch()
    // itself so an abort that lands between the call and the tick still wins.
    PersistentSlot localResponse;

    ~FetchRequest() {
        if (requestHeaders) curl_slist_free_all(requestHeaders);
    }
};

// ---------------------------------------------------------------------------
// Per-thread state
// ---------------------------------------------------------------------------
namespace {

struct FetchState {
    CURLM* multi = nullptr;
    std::unordered_map<int, std::unique_ptr<FetchRequest>> streams;
    std::vector<FetchRequest*> pending; // non-owning view into streams
    std::vector<int> localPending;      // stream ids with a localResponse to settle
    int nextStreamId = 1;
};

static thread_local FetchState g_fetchState;

void removePending(FetchState& s, FetchRequest* req) {
    for (auto it = s.pending.begin(); it != s.pending.end(); ++it) {
        if (*it == req) { s.pending.erase(it); return; }
    }
}

void removeLocalPending(FetchState& s, int streamId) {
    for (auto it = s.localPending.begin(); it != s.localPending.end(); ++it) {
        if (*it == streamId) { s.localPending.erase(it); return; }
    }
}

static thread_local std::vector<std::string> g_fetchBasePaths;

} // namespace

void addFetchBasePath(const std::string& path)
{
    g_fetchBasePaths.push_back(path);
}

// ---------------------------------------------------------------------------
// Helpers backed by JS factories from fetch_helpers.js
// ---------------------------------------------------------------------------
static bronze::Value callInternal(const char* fnName, std::span<const bronze::Value> args)
{
    bronze::Value internals = ev::getGlobal("__brokit_fetch_internals");
    if (!ev::isObject(internals)) {
        return ev::throwError("fetch: internals not installed");
    }
    bronze::Value fn = ev::getProperty(internals, fnName);
    if (!ev::isFunction(fn)) {
        return ev::throwError(std::string("fetch: missing internal helper '") + fnName + "'");
    }
    auto ret = ev::call(fn, ev::undefined(), args);
    return ret.value;
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

static bool isBlobUrl(const std::string& url)
{
    return url.size() >= 5 && url.compare(0, 5, "blob:") == 0;
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

extern std::string resolveBrokitPrefixMount(const std::string& path);

static std::string resolveLocalPath(const std::string& url)
{
    std::string mounted = resolveBrokitPrefixMount(url);
    if (!mounted.empty()) return mounted;

    std::string clean = url;
    if (clean.size() >= 2 && clean[0] == '.' && clean[1] == '/')
        clean = clean.substr(2);

    if (clean.size() >= 2 && clean[1] == ':') return clean;
    if (!clean.empty() && (clean[0] == '/' || clean[0] == '\\')) {
        std::ifstream absTest(clean, std::ios::binary);
        if (absTest.good()) return clean;
        clean = clean.substr(1);
    }

    auto g = ev::globalValue("globalThis");
    if (g.found && ev::isObject(g.value)) {
        auto arr = ev::getProperty(g.value, "__brokit_fetch_base_paths");
        if (ev::isObject(arr)) {
            auto lenVal = ev::getProperty(arr, "length");
            int32_t len = static_cast<int32_t>(ev::toDouble(lenVal));
            for (int32_t i = len - 1; i >= 0; --i) {
                auto elem = ev::getElement(arr, static_cast<uint32_t>(i));
                if (ev::isString(elem)) {
                    std::string candidate = ev::toUtf8(elem);
                    if (!candidate.empty() && candidate.back() != '/' && candidate.back() != '\\')
                        candidate += '/';
                    candidate += clean;
                    std::ifstream test(candidate, std::ios::binary);
                    if (test.good()) return candidate;
                }
            }
        }
    }

    for (int i = static_cast<int>(g_fetchBasePaths.size()) - 1; i >= 0; i--) {
        std::string candidate = g_fetchBasePaths[i];
        if (!candidate.empty() && candidate.back() != '/' && candidate.back() != '\\')
            candidate += '/';
        candidate += clean;
        std::ifstream test(candidate, std::ios::binary);
        if (test.good()) return candidate;
    }

    return clean;
}

// Build a Headers-like JS object from a flat header list ("name: value" lines).
static bronze::Value buildHeaders(const std::vector<std::string>& headers)
{
    ObjectBuilder hdrs;
    for (auto& h : headers) {
        auto colon = h.find(':');
        if (colon != std::string::npos) {
            std::string name = h.substr(0, colon);
            std::string value = h.substr(colon + 1);
            while (!value.empty() && value[0] == ' ') value.erase(0, 1);
            for (auto& c : name) c = static_cast<char>(tolower(c));
            hdrs.set(name, value);
        }
    }

    bronze::Value hdrsVal = hdrs.build();
    return callInternal("headers", std::array<bronze::Value, 1>{hdrsVal});
}

// Build a Response for a data: URL — RFC 2397.
static bronze::Value buildDataUrlResponse(const std::string& url)
{
    std::string rest = url.substr(5); // strip "data:"
    auto comma = rest.find(',');

    std::string meta;
    std::string payload;
    if (comma == std::string::npos) {
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
        static const int8_t tbl[128] = {
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

    std::vector<std::string> headerLines = {
        "content-type: " + mime,
        "content-length: " + std::to_string(data.size()),
    };

    ObjectBuilder resp;
    resp.set("status", 200.0);
    resp.set("statusText", "OK");
    resp.set("ok", true);
    resp.set("url", url);
    resp.set("headers", buildHeaders(headerLines));
    resp.set("__body", ev::createArrayBuffer(std::span<const uint8_t>(data.data(), data.size())));

    bronze::Value respVal = resp.build();
    callInternal("applyFileBody", std::array<bronze::Value, 1>{respVal});
    return respVal;
}

// Build a Response for a blob: URL from the URL.createObjectURL registry.
// `found` is false for a URL that was never minted or has been revoked.
static bronze::Value buildBlobUrlResponse(const std::string& url, bool* found)
{
    bronze::Value blob = ev::undefined();
    if (!blobByObjectURL(url, &blob)) {
        *found = false;
        return ev::undefined();
    }
    *found = true;

    // The bytes live in the Blob's own C++ storage, which no collection moves,
    // so the span stays valid across the allocations below.
    const uint8_t* data = nullptr;
    size_t len = 0;
    std::string type;
    blobBytes(blob, &data, &len, &type);

    std::vector<std::string> headerLines;
    if (!type.empty()) headerLines.push_back("content-type: " + type);
    headerLines.push_back("content-length: " + std::to_string(len));

    ObjectBuilder resp;
    resp.set("status", 200.0);
    resp.set("statusText", "OK");
    resp.set("ok", true);
    resp.set("url", url);
    resp.set("headers", buildHeaders(headerLines));
    resp.set("__body", ev::createArrayBuffer(std::span<const uint8_t>(data, len)));

    bronze::Value respVal = resp.build();
    callInternal("applyFileBody", std::array<bronze::Value, 1>{respVal});
    return respVal;
}

// Build a Response for a local file read
static bronze::Value buildFileResponse(const std::string& url, const std::string& resolvedPath)
{
    std::ifstream file(resolvedPath, std::ios::in | std::ios::binary | std::ios::ate);
    if (!file) {
        ObjectBuilder resp;
        resp.set("status", 404.0);
        resp.set("statusText", "Not Found");
        resp.set("ok", false);
        resp.set("url", url);
        resp.set("headers", buildHeaders({}));

        bronze::Value respVal = resp.build();
        callInternal("applyNotFoundBody", std::array<bronze::Value, 1>{respVal});
        return respVal;
    }

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);

    std::string mime = detectMimeType(resolvedPath);
    std::vector<std::string> headerLines = {
        "content-type: " + mime,
        "content-length: " + std::to_string(data.size()),
    };

    ObjectBuilder resp;
    resp.set("status", 200.0);
    resp.set("statusText", "OK");
    resp.set("ok", true);
    resp.set("url", url);
    resp.set("headers", buildHeaders(headerLines));
    resp.set("__body", ev::createArrayBuffer(std::span<const uint8_t>(data.data(), data.size())));

    bronze::Value respVal = resp.build();
    callInternal("applyFileBody", std::array<bronze::Value, 1>{respVal});
    return respVal;
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

// Build a Response object for a streaming response
static bronze::Value buildStreamingResponse(FetchRequest* req)
{
    ObjectBuilder resp;
    resp.set("status", static_cast<double>(req->statusCode));
    resp.set("statusText", req->statusText);
    resp.set("ok", req->statusCode >= 200 && req->statusCode < 300);
    resp.set("url", req->url);
    resp.set("headers", buildHeaders(req->headers));
    resp.set("__streamId", static_cast<double>(req->streamId));

    bronze::Value respVal = resp.build();
    callInternal("applyStreamingBody", std::array<bronze::Value, 1>{respVal});
    return respVal;
}

// Build a Response for a completed response
static bronze::Value buildCompleteResponse(FetchRequest* req)
{
    ObjectBuilder resp;
    resp.set("status", static_cast<double>(req->statusCode));
    resp.set("statusText", req->statusText);
    resp.set("ok", req->statusCode >= 200 && req->statusCode < 300);
    resp.set("url", req->url);
    resp.set("headers", buildHeaders(req->headers));
    resp.set("__body", ev::createArrayBuffer(std::span<const uint8_t>(req->body.data(), req->body.size())));
    resp.set("__streamId", static_cast<double>(req->streamId));

    bronze::Value respVal = resp.build();
    callInternal("applyCompleteBody", std::array<bronze::Value, 1>{respVal});
    return respVal;
}

// ---------------------------------------------------------------------------
// Stream natives
// ---------------------------------------------------------------------------

// __brokit_fetch_stream_read(streamId) → {value: Uint8Array, done: false} | {done: true} | null
static bronze::Value js_fetch_stream_read(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty()) return ev::null();
    ArgReader reader(args);
    int streamId = reader.getInt(0, 0);

    auto& s = g_fetchState;
    auto it = s.streams.find(streamId);
    if (it == s.streams.end() || !it->second) {
        ObjectBuilder obj;
        obj.set("done", true);
        obj.set("value", ev::undefined());
        return obj.build();
    }

    FetchRequest* req = it->second.get();

    if (!req->chunks.empty()) {
        auto chunk = std::move(req->chunks.front());
        req->chunks.erase(req->chunks.begin());

        ObjectBuilder obj;
        obj.set("done", false);
        bronze::Value ab = ev::createArrayBuffer(std::span<const uint8_t>(chunk.data(), chunk.size()));
        bronze::Value u8 = ev::createTypedArrayView(elements::Uint8, ab, 0, static_cast<uint32_t>(chunk.size()));
        obj.set("value", u8);
        return obj.build();
    }

    if (req->bodyComplete) {
        ObjectBuilder obj;
        obj.set("done", true);
        obj.set("value", ev::undefined());
        if (req->headersResolved && !req->easy) {
            s.streams.erase(it);
        }
        return obj.build();
    }

    return ev::null();
}

// __brokit_fetch_stream_wait(streamId, callback)
static bronze::Value js_fetch_stream_wait(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 2) return ev::undefined();
    ArgReader reader(args);
    int streamId = reader.getInt(0, 0);
    bronze::Value cb = reader.get(1);

    auto& s = g_fetchState;
    auto it = s.streams.find(streamId);
    if (it == s.streams.end()) return ev::undefined();

    FetchRequest* req = it->second.get();
    req->waitCallback.set(cb);
    return ev::undefined();
}

// ---------------------------------------------------------------------------
// AbortSignal support
// ---------------------------------------------------------------------------
static bronze::Value makeAbortError()
{
    auto res = ev::construct(ev::getGlobal("Error"),
        std::array<bronze::Value, 1>{ev::fromUtf8("The operation was aborted.")});
    bronze::Value err = res.value;
    ev::setProperty(err, "name", ev::fromUtf8("AbortError"));
    return err;
}

static bool signalIsAborted(bronze::Value signal)
{
    bronze::Value abortedVal = ev::getProperty(signal, "aborted");
    return ev::isBool(abortedVal) && ev::toBool(abortedVal);
}

static void abortRequest(int streamId)
{
    auto& s = g_fetchState;
    auto it = s.streams.find(streamId);
    if (it == s.streams.end() || !it->second) return;
    FetchRequest* req = it->second.get();

    if (req->easy) {
        removePending(s, req);
        if (s.multi) curl_multi_remove_handle(s.multi, req->easy);
        curl_easy_cleanup(req->easy);
        req->easy = nullptr;
    }
    removeLocalPending(s, streamId);
    req->localResponse.reset();
    req->bodyComplete = true;
    req->chunks.clear();

    const bool hadPromise = !req->headersResolved;
    if (hadPromise) {
        if (req->promise.valid()) {
            bronze::Value err = makeAbortError();
            ev::rejectPromise(req->promise.get(), err);
            req->promise.reset();
        }
        req->headersResolved = true;
    }
    if (req->waitCallback.valid()) {
        PersistentSlot cb = std::move(req->waitCallback);
        req->waitCallback.reset();
        ev::call(cb.get(), ev::undefined(), {});
    }
    if (hadPromise) s.streams.erase(it);
}

// Subscribe `abortRequest(streamId)` to the signal's abort event.
static void attachAbortListener(bronze::Value signal, int streamId)
{
    if (!ev::isObject(signal)) return;
    ev::Persistent signalRoot(signal);
    ev::Persistent handler(ev::makeFunction([streamId](bronze::Value, std::span<const bronze::Value>) -> bronze::Value {
        abortRequest(streamId);
        return ev::undefined();
    }, 0, "abortHandler"));

    bronze::Value addFn = ev::getProperty(signalRoot.get(), "addEventListener");
    if (ev::isFunction(addFn)) {
        std::array<bronze::Value, 2> lArgs = { ev::fromUtf8("abort"), handler.get() };
        ev::call(addFn, signalRoot.get(), lArgs);
    }
}

// Park an already-built local response until the next tick, where it settles
// the promise unless an abort got there first.
static bronze::Value queueLocalResponse(FetchState& s, const std::string& url,
                                        bronze::Value response, bronze::Value signal)
{
    ev::Persistent responseRoot(response);
    ev::Persistent signalRoot(signal);

    auto req = std::make_unique<FetchRequest>();
    req->url = url;
    req->streamId = s.nextStreamId++;
    req->bodyComplete = true;
    req->localResponse.set(responseRoot.get());
    req->promise.set(ev::createPromise());

    int streamId = req->streamId;
    FetchRequest* raw = req.get();
    s.streams.emplace(streamId, std::move(req));
    s.localPending.push_back(streamId);

    attachAbortListener(signalRoot.get(), streamId);
    return raw->promise.get();
}

// Settle every parked local response. Runs no JS itself (settling only queues
// reaction jobs), so the id list can be walked without re-entrancy concerns.
static int settleLocalResponses(FetchState& s)
{
    if (s.localPending.empty()) return 0;
    std::vector<int> ids;
    ids.swap(s.localPending);

    int completed = 0;
    for (int id : ids) {
        auto it = s.streams.find(id);
        if (it == s.streams.end() || !it->second) continue;
        FetchRequest* req = it->second.get();
        if (req->promise.valid()) {
            ev::resolvePromise(req->promise.get(), req->localResponse.get());
            req->promise.reset();
        }
        req->localResponse.reset();
        req->headersResolved = true;
        s.streams.erase(it);
        completed++;
    }
    return completed;
}

// ---------------------------------------------------------------------------
// Tick: pump curl_multi, resolve streaming responses, notify waiting readers
// ---------------------------------------------------------------------------
static bronze::Value js_fetch_tick(bronze::Value, std::span<const bronze::Value>)
{
    auto& s = g_fetchState;
    int completed = settleLocalResponses(s);
    if (!s.multi || s.pending.empty()) return ev::fromDouble(completed);

    curl_multi_poll(s.multi, nullptr, 0, 50, nullptr);

    int running = 0;
    curl_multi_perform(s.multi, &running);

    // Phase 1: resolve streaming responses that have received data.
    for (FetchRequest* req : s.pending) {
        if (!req->headersResolved && req->hasReceivedData) {
            curl_easy_getinfo(req->easy, CURLINFO_RESPONSE_CODE, &req->statusCode);
            char* effectiveUrl = nullptr;
            curl_easy_getinfo(req->easy, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
            if (effectiveUrl) req->url = effectiveUrl;

            bronze::Value response = buildStreamingResponse(req);
            if (req->promise.valid()) {
                ev::resolvePromise(req->promise.get(), response);
                req->promise.reset();
            }
            req->headersResolved = true;
        }
    }

    // Phase 2: notify waiting stream readers.
    for (auto& [id, reqOwn] : s.streams) {
        FetchRequest* req = reqOwn.get();
        if (!req) continue;
        if ((!req->chunks.empty() || req->bodyComplete) && req->waitCallback.valid()) {
            PersistentSlot cb = std::move(req->waitCallback);
            req->waitCallback.reset();
            ev::call(cb.get(), ev::undefined(), {});
        }
    }

    // Phase 3: handle completed requests.
    CURLMsg* msg;
    int msgs_in_queue;
    while ((msg = curl_multi_info_read(s.multi, &msgs_in_queue))) {
        if (msg->msg != CURLMSG_DONE) continue;

        CURL* easy = msg->easy_handle;
        FetchRequest* req = nullptr;
        for (FetchRequest* p : s.pending) {
            if (p->easy == easy) { req = p; break; }
        }
        if (!req) continue;
        removePending(s, req);

        curl_multi_remove_handle(s.multi, easy);
        req->bodyComplete = true;

        bool wasStreaming = req->headersResolved;
        if (!req->headersResolved) {
            if (msg->data.result == CURLE_OK) {
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &req->statusCode);
                char* effectiveUrl = nullptr;
                curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
                if (effectiveUrl) req->url = effectiveUrl;

                bronze::Value response = buildCompleteResponse(req);
                if (req->promise.valid()) {
                    ev::resolvePromise(req->promise.get(), response);
                    req->promise.reset();
                }
            } else {
                const char* errMsg = curl_easy_strerror(msg->data.result);
                auto errRes = ev::construct(ev::getGlobal("Error"),
                    std::array<bronze::Value, 1>{ev::fromUtf8(errMsg ? errMsg : "fetch failed")});
                if (req->promise.valid()) {
                    ev::rejectPromise(req->promise.get(), errRes.value);
                    req->promise.reset();
                }
            }
            req->headersResolved = true;
        }

        if (req->waitCallback.valid()) {
            PersistentSlot cb = std::move(req->waitCallback);
            req->waitCallback.reset();
            ev::call(cb.get(), ev::undefined(), {});
        }

        curl_easy_cleanup(easy);
        req->easy = nullptr;

        if (!wasStreaming) {
            s.streams.erase(req->streamId);
        }

        completed++;
    }

    return ev::fromDouble(completed);
}

// ---------------------------------------------------------------------------
// fetch(url, options?) — returns Promise
// ---------------------------------------------------------------------------
static bronze::Value js_fetch(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty()) return ev::throwTypeError("fetch: URL required");

    ArgReader reader(args);
    std::string url = reader.getString(0, "");

    auto& s = g_fetchState;

    // The signal comes first, for every scheme: a signal that is already
    // aborted rejects before any I/O, and a live one is subscribed to below.
    ev::Persistent signal;
    if (args.size() >= 2 && ev::isObject(args[1])) {
        bronze::Value sig = ev::getProperty(args[1], "signal");
        if (ev::isObject(sig)) {
            if (signalIsAborted(sig)) {
                bronze::Value p = ev::createPromise();
                ev::rejectPromise(p, makeAbortError());
                return p;
            }
            signal.set(sig);
        }
    }

    if (!isHttpUrl(url)) {
        bronze::Value response;
        if (isDataUrl(url)) {
            response = buildDataUrlResponse(url);
        } else if (isBlobUrl(url)) {
            bool found = false;
            response = buildBlobUrlResponse(url, &found);
            if (!found) {
                // A URL that was never minted, or has been revoked, is a
                // network error: the TypeError fetch rejects with on the web.
                auto err = ev::construct(ev::getGlobal("TypeError"),
                    std::array<bronze::Value, 1>{ev::fromUtf8("fetch: unknown blob URL " + url)});
                bronze::Value p = ev::createPromise();
                ev::rejectPromise(p, err.value);
                return p;
            }
        } else {
            std::string resolved = resolveLocalPath(url);
            response = buildFileResponse(url, resolved);
        }
        return queueLocalResponse(s, url, response, signal.get());
    }

    auto req = std::make_unique<FetchRequest>();
    req->url = url;
    req->streamId = s.nextStreamId++;

    req->easy = curl_easy_init();
    if (!req->easy) {
        return ev::throwError("fetch: curl_easy_init failed");
    }

    curl_easy_setopt(req->easy, CURLOPT_URL, req->url.c_str());
    curl_easy_setopt(req->easy, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(req->easy, CURLOPT_WRITEDATA, req.get());
    curl_easy_setopt(req->easy, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(req->easy, CURLOPT_HEADERDATA, req.get());
    curl_easy_setopt(req->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(req->easy, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(req->easy, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(req->easy, CURLOPT_USERAGENT, "brokit/0.5");
    curl_easy_setopt(req->easy, CURLOPT_ACCEPT_ENCODING, "");

    if (args.size() >= 2 && ev::isObject(args[1])) {
        bronze::Value opt = args[1];
        bronze::Value methodVal = ev::getProperty(opt, "method");
        if (ev::isString(methodVal)) {
            std::string method = ev::toUtf8(methodVal);
            curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, method.c_str());
            if (method == "POST" || method == "PUT" || method == "PATCH") {
                curl_easy_setopt(req->easy, CURLOPT_POST, 1L);
            }
        }

        bronze::Value headersVal = ev::getProperty(opt, "headers");
        if (ev::isObject(headersVal)) {
            bronze::Value objCtor = ev::getGlobal("Object");
            bronze::Value keysFn = ev::getProperty(objCtor, "keys");
            auto keysRes = ev::call(keysFn, ev::undefined(), std::array<bronze::Value, 1>{headersVal});
            if (!keysRes.thrown) {
                bronze::Value keysArr = keysRes.value;
                bronze::Value lenVal = ev::getProperty(keysArr, "length");
                int len = ev::isDouble(lenVal) ? static_cast<int>(ev::toDouble(lenVal)) : 0;
                for (int i = 0; i < len; ++i) {
                    bronze::Value k = ev::getElement(keysArr, i);
                    if (ev::isString(k)) {
                        std::string key = ev::toUtf8(k);
                        bronze::Value val = ev::getProperty(headersVal, key);
                        if (ev::isString(val)) {
                            std::string line = key + ": " + ev::toUtf8(val);
                            req->requestHeaders = curl_slist_append(req->requestHeaders, line.c_str());
                        }
                    }
                }
            }
        }

        bronze::Value bodyVal = ev::getProperty(opt, "body");
        if (ev::isString(bodyVal)) {
            std::string body = ev::toUtf8(bodyVal);
            req->requestBody.assign(body.begin(), body.end());
        } else if (auto info = ev::typedArrayInfo(bodyVal)) {
            req->requestBody.assign(info.data, info.data + info.byteLength);
        } else if (auto info = ev::arrayBufferInfo(bodyVal)) {
            req->requestBody.assign(info.data, info.data + info.byteLength);
        }

        if (req->requestHeaders) {
            curl_easy_setopt(req->easy, CURLOPT_HTTPHEADER, req->requestHeaders);
        }

        if (!req->requestBody.empty()) {
            curl_easy_setopt(req->easy, CURLOPT_POSTFIELDS, req->requestBody.data());
            curl_easy_setopt(req->easy, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(req->requestBody.size()));
        }
    }

    req->promise.set(ev::createPromise());

    if (!s.multi) {
        s.multi = curl_multi_init();
    }
    curl_multi_add_handle(s.multi, req->easy);

    int streamId = req->streamId;
    FetchRequest* raw = req.get();

    s.streams.emplace(streamId, std::move(req));
    s.pending.push_back(raw);

    attachAbortListener(signal.get(), streamId);

    return raw->promise.get();
}

static bronze::Value js_fetch_has_pending(bronze::Value, std::span<const bronze::Value>)
{
    auto& s = g_fetchState;
    if (!s.pending.empty() || !s.localPending.empty()) return ev::fromBool(true);
    for (auto& [id, req] : s.streams) {
        if (req && req->waitCallback.valid()) return ev::fromBool(true);
    }
    return ev::fromBool(false);
}

void installFetch()
{
    static bool curlInited = false;
    if (!curlInited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curlInited = true;
    }

    ev::registerFunction("fetch", js_fetch);
    ev::registerFunction("__brokit_fetch_tick", js_fetch_tick);
    ev::registerFunction("__brokit_fetch_has_pending", js_fetch_has_pending);
    ev::registerFunction("__brokit_fetch_stream_read", js_fetch_stream_read);
    ev::registerFunction("__brokit_fetch_stream_wait", js_fetch_stream_wait);

    bronze::embed::runEntry(bronze_fetch_helpers_main);
}

void uninstallFetch()
{
    auto& s = g_fetchState;

    for (auto& [id, reqOwn] : s.streams) {
        FetchRequest* req = reqOwn.get();
        if (!req) continue;
        if (req->easy) {
            if (s.multi) curl_multi_remove_handle(s.multi, req->easy);
            curl_easy_cleanup(req->easy);
            req->easy = nullptr;
        }
        req->promise.reset();
        req->waitCallback.reset();
        req->localResponse.reset();
    }
    s.pending.clear();
    s.localPending.clear();
    s.streams.clear();

    if (s.multi) {
        curl_multi_cleanup(s.multi);
        s.multi = nullptr;
    }
}

} // namespace brokit::api
