#include "api/api.h"
#include "runtime/runtime.h"

#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

#include <curl/curl.h>

namespace brokit::api {

// ---------------------------------------------------------------------------
// Per-connection state
// ---------------------------------------------------------------------------
struct WSConnection {
    CURL* easy = nullptr;
    JSContext* ctx = nullptr;
    int id = 0;

    // Connection state: 0=connecting, 1=open, 2=closing, 3=closed
    int state = 0;

    // Promise for initial connection
    JSValue resolving[2] = { JS_UNDEFINED, JS_UNDEFINED };

    // Buffered received messages (text or binary)
    struct Message {
        std::vector<uint8_t> data;
        bool binary = false;
    };
    std::vector<Message> inbox;

    // Partial frame accumulator
    std::vector<uint8_t> partialData;
    bool partialBinary = false;

    // Close info
    int closeCode = 0;
    std::string closeReason;

    // Whether connection was established (upgrade completed)
    bool connected = false;

    // Error message if connection failed
    std::string errorMsg;

    // Ticks a closed (state 3) connection has survived without a JS reader
    // claiming its close event. A connection wrapped by a WebSocket instance is
    // drained and erased in the same tick it closes; anything still counting up
    // here is orphaned (opened through the raw __brokit_ws_* bindings) and gets
    // reaped so it stops holding __brokit_ws_has_pending() true forever.
    int closedSweeps = 0;
};

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static CURLM* g_ws_multi = nullptr;
static std::unordered_map<int, WSConnection*> g_ws_conns;
static std::vector<WSConnection*> g_ws_connecting; // handles in curl_multi
static int g_ws_nextId = 1;

// Dummy write callback — curl WS with CONNECT_ONLY doesn't use it after upgrade
static size_t wsWriteCallback(char*, size_t size, size_t nmemb, void*)
{
    return size * nmemb;
}

// ---------------------------------------------------------------------------
// __brokit_ws_connect(url, protocols?) → { id, promise }
// ---------------------------------------------------------------------------
static JSValue js_ws_connect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "ws_connect: URL required");

    const char* urlStr = JS_ToCString(ctx, argv[0]);
    if (!urlStr) return JS_EXCEPTION;
    std::string url(urlStr);
    JS_FreeCString(ctx, urlStr);

    // Get optional protocols string
    std::string protocols;
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char* p = JS_ToCString(ctx, argv[1]);
        if (p) { protocols = p; JS_FreeCString(ctx, p); }
    }

    auto* conn = new WSConnection();
    conn->ctx = ctx;
    conn->id = g_ws_nextId++;
    conn->state = 0; // connecting

    conn->easy = curl_easy_init();
    if (!conn->easy) {
        delete conn;
        return JS_ThrowInternalError(ctx, "ws_connect: curl_easy_init failed");
    }

    curl_easy_setopt(conn->easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(conn->easy, CURLOPT_CONNECT_ONLY, 2L); // WebSocket upgrade
    curl_easy_setopt(conn->easy, CURLOPT_WRITEFUNCTION, wsWriteCallback);
    curl_easy_setopt(conn->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(conn->easy, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(conn->easy, CURLOPT_USERAGENT, "brokit/0.5");

    if (!protocols.empty()) {
        // Set WebSocket sub-protocols via headers
        std::string header = "Sec-WebSocket-Protocol: " + protocols;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, header.c_str());
        curl_easy_setopt(conn->easy, CURLOPT_HTTPHEADER, hdrs);
        // Note: curl_slist leak — in production we'd track and free this.
        // For now, it lives as long as the easy handle.
    }

    // Create promise for connection result
    JSValue promise = JS_NewPromiseCapability(ctx, conn->resolving);
    if (JS_IsException(promise)) {
        curl_easy_cleanup(conn->easy);
        delete conn;
        return promise;
    }

    // Add to multi
    if (!g_ws_multi) {
        g_ws_multi = curl_multi_init();
    }
    curl_multi_add_handle(g_ws_multi, conn->easy);
    g_ws_connecting.push_back(conn);
    g_ws_conns[conn->id] = conn;

    // Return { id, promise }
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "id", JS_NewInt32(ctx, conn->id));
    JS_SetPropertyStr(ctx, result, "promise", JS_DupValue(ctx, promise));
    JS_FreeValue(ctx, promise);
    return result;
}

// ---------------------------------------------------------------------------
// __brokit_ws_send(id, data, binary) → bool
// ---------------------------------------------------------------------------
static JSValue js_ws_send(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_FALSE;

    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);

    auto it = g_ws_conns.find(id);
    if (it == g_ws_conns.end() || it->second->state != 1)
        return JS_FALSE;

    WSConnection* conn = it->second;
    bool binary = (argc >= 3 && JS_ToBool(ctx, argv[2]));

    size_t sent = 0;
    CURLcode rc;

    if (binary) {
        // ArrayBuffer or Uint8Array
        size_t len = 0;
        uint8_t* buf = JS_GetUint8Array(ctx, &len, argv[1]);
        if (!buf) {
            // Try ArrayBuffer
            buf = JS_GetArrayBuffer(ctx, &len, argv[1]);
        }
        if (!buf) return JS_FALSE;

        rc = curl_ws_send(conn->easy, buf, len, &sent, 0, CURLWS_BINARY);
    } else {
        // Text string
        const char* str = JS_ToCString(ctx, argv[1]);
        if (!str) return JS_FALSE;
        size_t len = strlen(str);
        rc = curl_ws_send(conn->easy, str, len, &sent, 0, CURLWS_TEXT);
        JS_FreeCString(ctx, str);
    }

    return JS_NewBool(ctx, rc == CURLE_OK);
}

// ---------------------------------------------------------------------------
// __brokit_ws_close(id, code?, reason?) → bool
// ---------------------------------------------------------------------------
static JSValue js_ws_close(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_FALSE;

    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);

    auto it = g_ws_conns.find(id);
    if (it == g_ws_conns.end()) return JS_FALSE;

    WSConnection* conn = it->second;
    if (conn->state >= 2) return JS_FALSE; // already closing/closed

    // If still connecting, abort immediately
    if (conn->state == 0) {
        if (conn->easy && g_ws_multi) {
            curl_multi_remove_handle(g_ws_multi, conn->easy);
            // Remove from connecting list
            for (auto it2 = g_ws_connecting.begin(); it2 != g_ws_connecting.end(); ++it2) {
                if (*it2 == conn) { g_ws_connecting.erase(it2); break; }
            }
        }
        if (conn->easy) { curl_easy_cleanup(conn->easy); conn->easy = nullptr; }
        conn->state = 3; // closed
        conn->closeCode = 1006;
        // Free promise callbacks
        JS_FreeValue(ctx, conn->resolving[0]);
        JS_FreeValue(ctx, conn->resolving[1]);
        conn->resolving[0] = JS_UNDEFINED;
        conn->resolving[1] = JS_UNDEFINED;
        // Clean up from map
        g_ws_conns.erase(it);
        delete conn;
        return JS_TRUE;
    }

    conn->state = 2; // closing

    // Build close frame payload: 2-byte code + optional reason
    int code = 1000;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        JS_ToInt32(ctx, &code, argv[1]);
    }

    std::string reason;
    if (argc >= 3 && JS_IsString(argv[2])) {
        const char* r = JS_ToCString(ctx, argv[2]);
        if (r) { reason = r; JS_FreeCString(ctx, r); }
    }

    // Close frame: 2-byte network-order code + reason
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(code & 0xFF));
    payload.insert(payload.end(), reason.begin(), reason.end());

    size_t sent = 0;
    curl_ws_send(conn->easy, payload.data(), payload.size(), &sent, 0, CURLWS_CLOSE);

    conn->closeCode = code;
    conn->closeReason = reason;

    return JS_TRUE;
}

// ---------------------------------------------------------------------------
// __brokit_ws_recv(id) → { type, data, binary, code, reason } | null
// Poll for received messages.
// ---------------------------------------------------------------------------
static JSValue js_ws_recv(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NULL;

    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);

    auto it = g_ws_conns.find(id);
    if (it == g_ws_conns.end()) return JS_NULL;

    WSConnection* conn = it->second;

    if (!conn->inbox.empty()) {
        auto msg = std::move(conn->inbox.front());
        conn->inbox.erase(conn->inbox.begin());

        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "type", JS_NewString(ctx, "message"));
        JS_SetPropertyStr(ctx, result, "binary", JS_NewBool(ctx, msg.binary));

        if (msg.binary) {
            JSValue u8 = JS_NewUint8ArrayCopy(ctx, msg.data.data(), msg.data.size());
            JS_SetPropertyStr(ctx, result, "data", u8);
        } else {
            JS_SetPropertyStr(ctx, result, "data",
                JS_NewStringLen(ctx, reinterpret_cast<const char*>(msg.data.data()),
                                msg.data.size()));
        }
        return result;
    }

    // Check if connection has an error
    if (!conn->errorMsg.empty()) {
        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "type", JS_NewString(ctx, "error"));
        JS_SetPropertyStr(ctx, result, "data",
            JS_NewString(ctx, conn->errorMsg.c_str()));
        conn->errorMsg.clear();
        return result;
    }

    // Check if closed
    if (conn->state == 3) {
        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "type", JS_NewString(ctx, "close"));
        JS_SetPropertyStr(ctx, result, "code", JS_NewInt32(ctx, conn->closeCode));
        JS_SetPropertyStr(ctx, result, "reason",
            JS_NewString(ctx, conn->closeReason.c_str()));
        // Clean up
        g_ws_conns.erase(it);
        if (conn->easy) curl_easy_cleanup(conn->easy);
        delete conn;
        return result;
    }

    return JS_NULL;
}

// ---------------------------------------------------------------------------
// __brokit_ws_state(id) → int  (0=connecting, 1=open, 2=closing, 3=closed, -1=gone)
// ---------------------------------------------------------------------------
static JSValue js_ws_state(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NewInt32(ctx, -1);
    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    auto it = g_ws_conns.find(id);
    if (it == g_ws_conns.end()) return JS_NewInt32(ctx, -1);
    return JS_NewInt32(ctx, it->second->state);
}

// ---------------------------------------------------------------------------
// Tick: pump connecting handles and poll for incoming frames
// ---------------------------------------------------------------------------
static JSValue js_ws_tick(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (!g_ws_multi && g_ws_conns.empty()) return JS_NewInt32(ctx, 0);

    // Pump curl_multi for connecting handles
    if (g_ws_multi && !g_ws_connecting.empty()) {
        int running = 0;
        curl_multi_perform(g_ws_multi, &running);

        CURLMsg* msg;
        int msgs_in_queue;
        while ((msg = curl_multi_info_read(g_ws_multi, &msgs_in_queue))) {
            if (msg->msg != CURLMSG_DONE) continue;

            CURL* easy = msg->easy_handle;
            WSConnection* conn = nullptr;

            for (auto it2 = g_ws_connecting.begin(); it2 != g_ws_connecting.end(); ++it2) {
                if ((*it2)->easy == easy) {
                    conn = *it2;
                    g_ws_connecting.erase(it2);
                    break;
                }
            }
            if (!conn) continue;

            curl_multi_remove_handle(g_ws_multi, easy);

            if (msg->data.result == CURLE_OK) {
                conn->state = 1; // open
                conn->connected = true;
                // Resolve promise with true
                JSValue val = JS_TRUE;
                JSValue ret = JS_Call(ctx, conn->resolving[0], JS_UNDEFINED, 1, &val);
                JS_FreeValue(ctx, ret);
            } else {
                conn->state = 3; // closed (failed)
                conn->errorMsg = curl_easy_strerror(msg->data.result);
                // Reject promise
                JSValue err = JS_NewError(ctx);
                JS_SetPropertyStr(ctx, err, "message",
                    JS_NewString(ctx, conn->errorMsg.c_str()));
                JSValue ret = JS_Call(ctx, conn->resolving[1], JS_UNDEFINED, 1, &err);
                JS_FreeValue(ctx, ret);
                JS_FreeValue(ctx, err);
            }

            JS_FreeValue(ctx, conn->resolving[0]);
            JS_FreeValue(ctx, conn->resolving[1]);
            conn->resolving[0] = JS_UNDEFINED;
            conn->resolving[1] = JS_UNDEFINED;
        }
    }

    // Call JS drain to deliver connection results from promise resolution
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue drainFn = JS_GetPropertyStr(ctx, global, "__brokit_ws_drain_all");
        if (JS_IsFunction(ctx, drainFn)) {
            JSValue ret = JS_Call(ctx, drainFn, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, ret);
        }
        JS_FreeValue(ctx, drainFn);
        JS_FreeValue(ctx, global);
    }

    // Poll open connections for incoming frames
    for (auto& [id, conn] : g_ws_conns) {
        if (conn->state != 1 && conn->state != 2) continue;
        if (!conn->easy) continue;

        // Try to receive frames (non-blocking since CONNECT_ONLY)
        char buf[4096];
        for (int polls = 0; polls < 10; polls++) { // process multiple frames per tick
            size_t nread = 0;
            const struct curl_ws_frame* meta = nullptr;
            CURLcode rc = curl_ws_recv(conn->easy, buf, sizeof(buf), &nread, &meta);

            if (rc == CURLE_AGAIN) break; // no data available
            if (rc != CURLE_OK) {
                // Connection error or closed
                if (conn->state != 3) {
                    conn->state = 3;
                    if (conn->closeCode == 0) conn->closeCode = 1006; // abnormal
                }
                break;
            }

            if (!meta) break;

            if (meta->flags & CURLWS_CLOSE) {
                // Parse close frame
                if (nread >= 2) {
                    conn->closeCode = (static_cast<uint8_t>(buf[0]) << 8) |
                                       static_cast<uint8_t>(buf[1]);
                    if (nread > 2) {
                        conn->closeReason.assign(buf + 2, nread - 2);
                    }
                } else {
                    conn->closeCode = 1005; // no status
                }
                conn->state = 3;
                break;
            }

            if (meta->flags & CURLWS_PING) {
                // Auto-pong
                size_t sent = 0;
                curl_ws_send(conn->easy, buf, nread, &sent, 0, CURLWS_PONG);
                continue;
            }

            if ((meta->flags & CURLWS_TEXT) || (meta->flags & CURLWS_BINARY)) {
                bool binary = (meta->flags & CURLWS_BINARY) != 0;

                // Start or continue accumulating
                conn->partialData.insert(conn->partialData.end(), buf, buf + nread);
                conn->partialBinary = binary;

                // Check if this is a complete frame (bytesleft == 0 and not CONT)
                if (meta->bytesleft == 0) {
                    WSConnection::Message msg;
                    msg.data = std::move(conn->partialData);
                    msg.binary = conn->partialBinary;
                    conn->inbox.push_back(std::move(msg));
                    conn->partialData.clear();
                }
            } else if (meta->flags & CURLWS_CONT) {
                // Continuation frame
                conn->partialData.insert(conn->partialData.end(), buf, buf + nread);
                if (meta->bytesleft == 0) {
                    WSConnection::Message msg;
                    msg.data = std::move(conn->partialData);
                    msg.binary = conn->partialBinary;
                    conn->inbox.push_back(std::move(msg));
                    conn->partialData.clear();
                }
            }
        }
    }

    // Drain events to JS instances after frame polling
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue drainFn = JS_GetPropertyStr(ctx, global, "__brokit_ws_drain_all");
        if (JS_IsFunction(ctx, drainFn)) {
            JSValue ret = JS_Call(ctx, drainFn, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, ret);
        }
        JS_FreeValue(ctx, drainFn);
        JS_FreeValue(ctx, global);
    }

    // Reap orphaned closed connections. When a connection reaches state 3 its
    // close event is delivered to a bound WebSocket instance within this same
    // tick (drain → recv → erase, above). Any connection still in state 3 on a
    // later tick has no instance draining it — it was opened through the raw
    // __brokit_ws_* bindings with no WebSocket wrapper — and would otherwise keep
    // __brokit_ws_has_pending() true forever, pinning the event-loop pump until
    // it hits its iteration budget. Give the close event one tick to be claimed,
    // then drop it.
    for (auto it = g_ws_conns.begin(); it != g_ws_conns.end();) {
        WSConnection* conn = it->second;
        if (conn->state == 3 && conn->inbox.empty() && ++conn->closedSweeps > 1) {
            if (conn->easy) curl_easy_cleanup(conn->easy);
            delete conn;
            it = g_ws_conns.erase(it);
        } else {
            ++it;
        }
    }

    return JS_NewInt32(ctx, 0);
}

// ---------------------------------------------------------------------------
// __brokit_ws_has_pending() → bool
// ---------------------------------------------------------------------------
static JSValue js_ws_has_pending(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (!g_ws_connecting.empty()) return JS_NewBool(ctx, true);
    for (auto& [id, conn] : g_ws_conns) {
        if (conn->state == 1 || conn->state == 2) return JS_NewBool(ctx, true);
        if (!conn->inbox.empty()) return JS_NewBool(ctx, true);
        if (conn->state == 3) return JS_NewBool(ctx, true); // close event pending
    }
    return JS_NewBool(ctx, false);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
void installWebSocket(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    JS_SetPropertyStr(ctx, global, "__brokit_ws_connect",
        JS_NewCFunction(ctx, js_ws_connect, "__brokit_ws_connect", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_ws_send",
        JS_NewCFunction(ctx, js_ws_send, "__brokit_ws_send", 3));
    JS_SetPropertyStr(ctx, global, "__brokit_ws_close",
        JS_NewCFunction(ctx, js_ws_close, "__brokit_ws_close", 3));
    JS_SetPropertyStr(ctx, global, "__brokit_ws_recv",
        JS_NewCFunction(ctx, js_ws_recv, "__brokit_ws_recv", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_ws_state",
        JS_NewCFunction(ctx, js_ws_state, "__brokit_ws_state", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_ws_tick",
        JS_NewCFunction(ctx, js_ws_tick, "__brokit_ws_tick", 0));
    JS_SetPropertyStr(ctx, global, "__brokit_ws_has_pending",
        JS_NewCFunction(ctx, js_ws_has_pending, "__brokit_ws_has_pending", 0));

    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
