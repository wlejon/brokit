#include "api/api.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"

#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <span>
#include <array>

#include <curl/curl.h>

namespace brokit::api {

// ---------------------------------------------------------------------------
// Per-connection state
// ---------------------------------------------------------------------------
struct WSConnection {
    CURL* easy = nullptr;
    int id = 0;

    // Connection state: 0=connecting, 1=open, 2=closing, 3=closed
    int state = 0;

    // Promise for initial connection
    PersistentSlot promise;

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
    // claiming its close event.
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
static bronze::Value js_ws_connect(bronze::Value, std::span<const bronze::Value> args)
{
    ArgReader reader(args);
    std::string url = reader.getString(0, "");
    if (url.empty()) return ev::throwTypeError("ws_connect: URL required");

    std::string protocols = reader.getString(1, "");

    auto* conn = new WSConnection();
    conn->id = g_ws_nextId++;
    conn->state = 0; // connecting

    conn->easy = curl_easy_init();
    if (!conn->easy) {
        delete conn;
        return ev::throwError("ws_connect: curl_easy_init failed");
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
    }

    // Create promise for connection result
    conn->promise.set(ev::createPromise());

    // Add to multi
    if (!g_ws_multi) {
        g_ws_multi = curl_multi_init();
    }
    curl_multi_add_handle(g_ws_multi, conn->easy);
    g_ws_connecting.push_back(conn);
    g_ws_conns[conn->id] = conn;

    // Return { id, promise }
    ObjectBuilder result;
    result.set("id", static_cast<double>(conn->id));
    result.set("promise", conn->promise.get());
    return result.build();
}

// ---------------------------------------------------------------------------
// __brokit_ws_send(id, data, binary) → bool
// ---------------------------------------------------------------------------
static bronze::Value js_ws_send(bronze::Value, std::span<const bronze::Value> args)
{
    ArgReader reader(args);
    if (args.size() < 2) return ev::fromBool(false);

    int id = reader.getInt(0, 0);
    auto it = g_ws_conns.find(id);
    if (it == g_ws_conns.end() || it->second->state != 1)
        return ev::fromBool(false);

    WSConnection* conn = it->second;
    bool binary = reader.getBool(2, false);
    bronze::Value dataVal = reader.get(1);

    size_t sent = 0;
    CURLcode rc = CURLE_OK;

    if (binary) {
        if (auto info = ev::typedArrayInfo(dataVal)) {
            rc = curl_ws_send(conn->easy, info.data, info.byteLength, &sent, 0, CURLWS_BINARY);
        } else if (auto info = ev::arrayBufferInfo(dataVal)) {
            rc = curl_ws_send(conn->easy, info.data, info.byteLength, &sent, 0, CURLWS_BINARY);
        } else {
            return ev::fromBool(false);
        }
    } else {
        std::string str = ev::toUtf8(dataVal);
        rc = curl_ws_send(conn->easy, str.data(), str.size(), &sent, 0, CURLWS_TEXT);
    }

    return ev::fromBool(rc == CURLE_OK);
}

// ---------------------------------------------------------------------------
// __brokit_ws_close(id, code?, reason?) → bool
// ---------------------------------------------------------------------------
static bronze::Value js_ws_close(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty()) return ev::fromBool(false);

    ArgReader reader(args);
    int id = reader.getInt(0, 0);

    auto it = g_ws_conns.find(id);
    if (it == g_ws_conns.end()) return ev::fromBool(false);

    WSConnection* conn = it->second;
    if (conn->state >= 2) return ev::fromBool(false); // already closing/closed

    // If still connecting, abort immediately
    if (conn->state == 0) {
        if (conn->easy && g_ws_multi) {
            curl_multi_remove_handle(g_ws_multi, conn->easy);
            for (auto it2 = g_ws_connecting.begin(); it2 != g_ws_connecting.end(); ++it2) {
                if (*it2 == conn) { g_ws_connecting.erase(it2); break; }
            }
        }
        if (conn->easy) { curl_easy_cleanup(conn->easy); conn->easy = nullptr; }
        conn->state = 3; // closed
        conn->closeCode = 1006;
        conn->promise.reset();
        g_ws_conns.erase(it);
        delete conn;
        return ev::fromBool(true);
    }

    conn->state = 2; // closing

    int code = reader.getInt(1, 1000);
    std::string reason = reader.getString(2, "");

    // Close frame: 2-byte network-order code + reason
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(code & 0xFF));
    payload.insert(payload.end(), reason.begin(), reason.end());

    size_t sent = 0;
    curl_ws_send(conn->easy, payload.data(), payload.size(), &sent, 0, CURLWS_CLOSE);

    conn->closeCode = code;
    conn->closeReason = reason;

    return ev::fromBool(true);
}

// ---------------------------------------------------------------------------
// __brokit_ws_recv(id) → { type, data, binary, code, reason } | null
// ---------------------------------------------------------------------------
static bronze::Value js_ws_recv(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty()) return ev::null();

    ArgReader reader(args);
    int id = reader.getInt(0, 0);

    auto it = g_ws_conns.find(id);
    if (it == g_ws_conns.end()) return ev::null();

    WSConnection* conn = it->second;

    if (!conn->inbox.empty()) {
        auto msg = std::move(conn->inbox.front());
        conn->inbox.erase(conn->inbox.begin());

        ObjectBuilder result;
        result.set("type", "message");
        result.set("binary", msg.binary);

        if (msg.binary) {
            bronze::Value ab = ev::createArrayBuffer(std::span<const uint8_t>(msg.data.data(), msg.data.size()));
            bronze::Value u8 = ev::createTypedArrayView(elements::Uint8, ab, 0, static_cast<uint32_t>(msg.data.size()));
            result.set("data", u8);
        } else {
            result.set("data", std::string(reinterpret_cast<const char*>(msg.data.data()), msg.data.size()));
        }
        return result.build();
    }

    // Check if connection has an error
    if (!conn->errorMsg.empty()) {
        ObjectBuilder result;
        result.set("type", "error");
        result.set("data", conn->errorMsg);
        conn->errorMsg.clear();
        return result.build();
    }

    // Check if closed
    if (conn->state == 3) {
        ObjectBuilder result;
        result.set("type", "close");
        result.set("code", static_cast<double>(conn->closeCode));
        result.set("reason", conn->closeReason);
        g_ws_conns.erase(it);
        if (conn->easy) {
            if (g_ws_multi) curl_multi_remove_handle(g_ws_multi, conn->easy);
            curl_easy_cleanup(conn->easy);
        }
        delete conn;
        return result.build();
    }

    return ev::null();
}

// ---------------------------------------------------------------------------
// __brokit_ws_state(id) → int (0=connecting, 1=open, 2=closing, 3=closed, -1=gone)
// ---------------------------------------------------------------------------
static bronze::Value js_ws_state(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty()) return ev::fromDouble(-1);
    ArgReader reader(args);
    int id = reader.getInt(0, 0);
    auto it = g_ws_conns.find(id);
    if (it == g_ws_conns.end()) return ev::fromDouble(-1);
    return ev::fromDouble(it->second->state);
}

// ---------------------------------------------------------------------------
// Tick: pump connecting handles and poll for incoming frames
// ---------------------------------------------------------------------------
static bronze::Value js_ws_tick(bronze::Value, std::span<const bronze::Value>)
{
    if (!g_ws_multi && g_ws_conns.empty()) return ev::fromDouble(0);

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

            if (msg->data.result != CURLE_OK)
                curl_multi_remove_handle(g_ws_multi, easy);

            if (msg->data.result == CURLE_OK) {
                conn->state = 1; // open
                conn->connected = true;
                if (conn->promise.valid()) {
                    ev::resolvePromise(conn->promise.get(), ev::fromBool(true));
                    conn->promise.reset();
                }
            } else {
                conn->state = 3; // closed (failed)
                conn->errorMsg = curl_easy_strerror(msg->data.result);
                if (conn->promise.valid()) {
                    auto errRes = ev::construct(ev::getGlobal("Error"),
                        std::array<bronze::Value, 1>{ev::fromUtf8(conn->errorMsg)});
                    ev::rejectPromise(conn->promise.get(), errRes.value);
                    conn->promise.reset();
                }
            }
        }
    }

    // Call JS drain to deliver connection results from promise resolution
    {
        bronze::Value drainFn = ev::getGlobal("__brokit_ws_drain_all");
        if (ev::isFunction(drainFn)) {
            ev::call(drainFn, ev::undefined(), {});
        }
    }

    // Poll open connections for incoming frames
    for (auto& [id, conn] : g_ws_conns) {
        if (conn->state != 1 && conn->state != 2) continue;
        if (!conn->easy) continue;

        char buf[4096];
        for (int polls = 0; polls < 10; polls++) {
            size_t nread = 0;
            const struct curl_ws_frame* meta = nullptr;
            CURLcode rc = curl_ws_recv(conn->easy, buf, sizeof(buf), &nread, &meta);

            if (rc == CURLE_AGAIN) break;
            if (rc != CURLE_OK) {
                if (conn->state != 3) {
                    conn->state = 3;
                    if (conn->closeCode == 0) conn->closeCode = 1006;
                    if (conn->errorMsg.empty())
                        conn->errorMsg = std::string("curl_ws_recv: ") +
                                         curl_easy_strerror(rc);
                }
                break;
            }

            if (!meta) break;

            if (meta->flags & CURLWS_CLOSE) {
                if (nread >= 2) {
                    conn->closeCode = (static_cast<uint8_t>(buf[0]) << 8) |
                                       static_cast<uint8_t>(buf[1]);
                    if (nread > 2) {
                        conn->closeReason.assign(buf + 2, nread - 2);
                    }
                } else {
                    conn->closeCode = 1005;
                }
                conn->state = 3;
                break;
            }

            if (meta->flags & CURLWS_PING) {
                size_t sent = 0;
                curl_ws_send(conn->easy, buf, nread, &sent, 0, CURLWS_PONG);
                continue;
            }

            if ((meta->flags & CURLWS_TEXT) || (meta->flags & CURLWS_BINARY)) {
                bool binary = (meta->flags & CURLWS_BINARY) != 0;

                conn->partialData.insert(conn->partialData.end(), buf, buf + nread);
                conn->partialBinary = binary;

                if (meta->bytesleft == 0) {
                    WSConnection::Message msg;
                    msg.data = std::move(conn->partialData);
                    msg.binary = conn->partialBinary;
                    conn->inbox.push_back(std::move(msg));
                    conn->partialData.clear();
                }
            } else if (meta->flags & CURLWS_CONT) {
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
        bronze::Value drainFn = ev::getGlobal("__brokit_ws_drain_all");
        if (ev::isFunction(drainFn)) {
            ev::call(drainFn, ev::undefined(), {});
        }
    }

    // Reap orphaned closed connections
    for (auto it = g_ws_conns.begin(); it != g_ws_conns.end();) {
        WSConnection* conn = it->second;
        if (conn->state == 3 && conn->inbox.empty() && ++conn->closedSweeps > 1) {
            if (conn->easy) {
                if (g_ws_multi) curl_multi_remove_handle(g_ws_multi, conn->easy);
                curl_easy_cleanup(conn->easy);
            }
            delete conn;
            it = g_ws_conns.erase(it);
        } else {
            ++it;
        }
    }

    return ev::fromDouble(0);
}

// ---------------------------------------------------------------------------
// __brokit_ws_has_pending() → bool
// ---------------------------------------------------------------------------
static bronze::Value js_ws_has_pending(bronze::Value, std::span<const bronze::Value>)
{
    if (!g_ws_connecting.empty()) return ev::fromBool(true);
    for (auto& [id, conn] : g_ws_conns) {
        if (conn->state == 1 || conn->state == 2) return ev::fromBool(true);
        if (!conn->inbox.empty()) return ev::fromBool(true);
        if (conn->state == 3) return ev::fromBool(true);
    }
    return ev::fromBool(false);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
void installWebSocket()
{
    ev::registerFunction("__brokit_ws_connect", js_ws_connect);
    ev::registerFunction("__brokit_ws_send", js_ws_send);
    ev::registerFunction("__brokit_ws_close", js_ws_close);
    ev::registerFunction("__brokit_ws_recv", js_ws_recv);
    ev::registerFunction("__brokit_ws_state", js_ws_state);
    ev::registerFunction("__brokit_ws_tick", js_ws_tick);
    ev::registerFunction("__brokit_ws_has_pending", js_ws_has_pending);
}

} // namespace brokit::api
