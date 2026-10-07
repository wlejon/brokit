#include "api/api.h"
#include "api/arg_reader.h"
#include "api/fetch_internal.h"
#include "api/object_builder.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
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
    std::string mode = "cors";
    std::string credentials = "same-origin";
    std::string cache = "default";
    std::string redirect = "follow";
    std::string referrer;

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

} // namespace

// Response construction — the fetch_helpers.js bridge, headers and the
// file:, data: and blob: responses — lives in fetch_response.cpp.

// ---------------------------------------------------------------------------
// curl callbacks and HTTP responses
// ---------------------------------------------------------------------------
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

    callInternal("applyStreamingBody", std::array<bronze::Value, 1>{resp.build()});
    return resp.build();
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

    callInternal("applyCompleteBody", std::array<bronze::Value, 1>{resp.build()});
    return resp.build();
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
    ev::Persistent err{newError("Error", "The operation was aborted.")};
    ev::Persistent name{ev::fromUtf8("AbortError")};
    err.set(ev::setProperty(err.get(), "name", name.get()));
    return err.get();
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

    ev::Persistent addFn{ev::getProperty(signalRoot.get(), "addEventListener")};
    if (ev::isFunction(addFn.get())) {
        std::array<bronze::Value, 2> lArgs = { ev::fromUtf8("abort"), handler.get() };
        ev::call(addFn.get(), signalRoot.get(), lArgs);
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
                // A network error rejects fetch() with a TypeError (Fetch
                // spec; the blob: path above does the same).
                const char* errMsg = curl_easy_strerror(msg->data.result);
                if (req->promise.valid()) {
                    bronze::Value err = newError("TypeError", errMsg ? errMsg : "fetch failed");
                    ev::rejectPromise(req->promise.get(), err);
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
        signal.set(ev::getProperty(args[1], "signal"));
        if (!ev::isObject(signal.get())) {
            signal.set(ev::undefined());
        } else if (signalIsAborted(signal.get())) {
            ev::Persistent p{ev::createPromise()};
            ev::Persistent err{makeAbortError()};
            ev::rejectPromise(p.get(), err.get());
            return p.get();
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
                // Fetch's blob scheme fetch: a URL with no entry in the blob
                // URL store (revoked, or never minted) is a network error,
                // which fetch() reports as a TypeError rejection, not a 404.
                ev::Persistent p(ev::createPromise());
                ev::Persistent msg(ev::fromUtf8(
                    "fetch: " + url + " is not a live object URL (revoked, or never created)"));
                ev::Persistent ctor(ev::getGlobal("TypeError"));
                auto err = ev::construct(ctor.get(), std::array<bronze::Value, 1>{msg.get()});
                ev::Persistent errP(err.value);
                ev::rejectPromise(p.get(), errP.get());
                return p.get();
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
        // A reference into the rooted args span: it stays current across the
        // allocating calls below, where a copy would go stale.
        const bronze::Value& opt = args[1];

        bronze::Value mVal = ev::getProperty(opt, "mode");
        if (ev::isString(mVal)) {
            std::string m = ev::toUtf8(mVal);
            if (m != "cors" && m != "no-cors" && m != "same-origin" && m != "navigate")
                return ev::throwTypeError("fetch: invalid mode '" + m + "'");
            req->mode = m;
        }
        bronze::Value cVal = ev::getProperty(opt, "credentials");
        if (ev::isString(cVal)) {
            std::string c = ev::toUtf8(cVal);
            if (c != "omit" && c != "same-origin" && c != "include")
                return ev::throwTypeError("fetch: invalid credentials '" + c + "'");
            req->credentials = c;
            if (c == "omit") curl_easy_setopt(req->easy, CURLOPT_COOKIE, nullptr);
        }
        bronze::Value caVal = ev::getProperty(opt, "cache");
        if (ev::isString(caVal)) {
            std::string ca = ev::toUtf8(caVal);
            if (ca != "default" && ca != "no-store" && ca != "reload" && ca != "no-cache" &&
                ca != "force-cache" && ca != "only-if-cached")
                return ev::throwTypeError("fetch: invalid cache '" + ca + "'");
            req->cache = ca;
            if (ca == "no-store" || ca == "reload") curl_easy_setopt(req->easy, CURLOPT_FRESH_CONNECT, 1L);
        }
        bronze::Value rVal = ev::getProperty(opt, "redirect");
        if (ev::isString(rVal)) {
            std::string r = ev::toUtf8(rVal);
            if (r != "follow" && r != "error" && r != "manual")
                return ev::throwTypeError("fetch: invalid redirect '" + r + "'");
            req->redirect = r;
            if (r == "error" || r == "manual") curl_easy_setopt(req->easy, CURLOPT_FOLLOWLOCATION, 0L);
        }
        bronze::Value refVal = ev::getProperty(opt, "referrer");
        if (ev::isUndefined(refVal)) refVal = ev::getProperty(opt, "referer");
        if (ev::isString(refVal)) {
            req->referrer = ev::toUtf8(refVal);
            if (!req->referrer.empty() && req->referrer != "no-referrer") {
                curl_easy_setopt(req->easy, CURLOPT_REFERER, req->referrer.c_str());
            } else if (req->referrer == "no-referrer") {
                curl_easy_setopt(req->easy, CURLOPT_REFERER, nullptr);
            }
        }

        bronze::Value methodVal = ev::getProperty(opt, "method");
        if (ev::isString(methodVal)) {
            std::string method = ev::toUtf8(methodVal);
            curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, method.c_str());
            if (method == "POST" || method == "PUT" || method == "PATCH") {
                curl_easy_setopt(req->easy, CURLOPT_POST, 1L);
            }
        }

        ev::Persistent headersVal{ev::getProperty(opt, "headers")};
        if (ev::isObject(headersVal.get())) {
            ev::Persistent objCtor{ev::getGlobal("Object")};
            ev::Persistent keysFn{ev::getProperty(objCtor.get(), "keys")};
            auto keysRes = ev::call(keysFn.get(), ev::undefined(), std::array<bronze::Value, 1>{headersVal.get()});
            if (!keysRes.thrown) {
                ev::Persistent keysArr{keysRes.value};
                bronze::Value lenVal = ev::getProperty(keysArr.get(), "length");
                int len = ev::isDouble(lenVal) ? saturateI32(ev::toDouble(lenVal)) : 0;
                for (int i = 0; i < len; ++i) {
                    bronze::Value k = ev::getElement(keysArr.get(), i);
                    if (ev::isString(k)) {
                        std::string key = ev::toUtf8(k);
                        bronze::Value val = ev::getProperty(headersVal.get(), key);
                        if (ev::isString(val)) {
                            std::string line = key + ": " + ev::toUtf8(val);
                            req->requestHeaders = curl_slist_append(req->requestHeaders, line.c_str());
                        }
                    }
                }
            }
        }

        bronze::Value bodyVal = ev::getProperty(opt, "body");
        if (auto info = ev::typedArrayInfo(bodyVal)) {
            req->requestBody.assign(info.data, info.data + info.byteLength);
        } else if (auto info = ev::arrayBufferInfo(bodyVal)) {
            req->requestBody.assign(info.data, info.data + info.byteLength);
        } else if (ev::isString(bodyVal)) {
            std::string body = ev::toUtf8(bodyVal);
            req->requestBody.assign(body.begin(), body.end());
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


int settleLocalFetches() { return settleLocalResponses(g_fetchState); }

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
