// Raw TCP/UDP sockets — the native layer under the `net` and `dgram`
// Node-compat modules (js/net.js, js/dgram.js) and the WebSocketServer
// (js/websocket_server.js).
//
// Architecture mirrors websocket.cpp exactly: nonblocking OS sockets held in a
// global handle table, pumped by a host-driven `__brokit_net_tick()`, with
// per-handle event queues drained from JS via `__brokit_net_poll(id)` inside a
// `globalThis.__brokit_net_drain_all` hook the tick invokes. No threads — the
// host (bro's frame loop / headless advanceTime / worker loop / brokit's test
// harness) calls tick at its own cadence, so all socket IO happens on the JS
// thread. Backpressure is intentionally naive for v1: write() buffers
// unboundedly in userspace and flushes as the kernel accepts bytes.
//
// Security default: listeners (TCP listen + UDP bind) bind 127.0.0.1 unless an
// explicit host is passed. Exposing a port to the network requires opting in
// with "0.0.0.0" / "::" / a concrete interface address.

#include "api/api.h"
#include "runtime/runtime.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define BROKIT_INVALID_SOCKET INVALID_SOCKET
#define brokit_closesocket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int socket_t;
#define BROKIT_INVALID_SOCKET (-1)
#define brokit_closesocket ::close
#endif

namespace brokit::api {

namespace {

// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------

void ensureSocketsInit()
{
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = true;
    }
#endif
}

int lastSockError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool errWouldBlock(int e)
{
#ifdef _WIN32
    return e == WSAEWOULDBLOCK;
#else
    return e == EWOULDBLOCK || e == EAGAIN;
#endif
}

bool errInProgress(int e)
{
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return e == EINPROGRESS;
#endif
}

std::string sockErrorString(int e)
{
#ifdef _WIN32
    char* buf = nullptr;
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(e), 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string s = (n && buf) ? std::string(buf, n) : ("winsock error " + std::to_string(e));
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
#else
    return std::strerror(e);
#endif
}

bool setNonBlocking(socket_t fd)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// Numeric address + port from a sockaddr.
void describeAddr(const sockaddr* sa, socklen_t salen, std::string& address, int& port)
{
    char host[NI_MAXHOST] = {0};
    char serv[NI_MAXSERV] = {0};
    if (getnameinfo(sa, salen, host, sizeof(host), serv, sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        address = host;
        port = std::atoi(serv);
    } else {
        address.clear();
        port = 0;
    }
}

// ---------------------------------------------------------------------------
// Handle table
// ---------------------------------------------------------------------------

struct NetEvent {
    enum class Type { Accept, Connect, Data, End, Message, Close, Error };
    Type type;
    std::vector<uint8_t> data;   // Data / Message payload
    std::string address;         // Accept peer / Message source
    int port = 0;
    std::string family;          // Message: "IPv4" | "IPv6"
    int connId = 0;              // Accept: id of the new connection handle
    std::string message;         // Error text
    bool hadError = false;       // Close
};

struct NetHandle {
    enum class Kind { TcpListener, TcpConn, Udp };
    Kind kind = Kind::TcpConn;
    int id = 0;
    socket_t fd = BROKIT_INVALID_SOCKET;

    // TCP connection lifecycle
    bool connecting = false;   // nonblocking connect() in flight
    bool peerEof = false;      // FIN received
    bool endRequested = false; // end(): send FIN once outBuf drains
    bool wroteShutdown = false;
    bool closed = false;       // fd closed; handle lives until Close drained
    bool errored = false;

    std::vector<uint8_t> outBuf; // pending writes (backpressure-naive, unbounded)
    std::deque<NetEvent> events;

    // Cached endpoint info
    std::string localAddress;
    int localPort = 0;
    std::string remoteAddress;
    int remotePort = 0;

    bool ipv6 = false; // UDP family

    // Ticks a fully-closed handle has survived without JS draining its Close
    // event. Handles wrapped by the JS layer are drained the tick they close;
    // anything still counting up was opened through the raw bindings and gets
    // reaped so it stops holding __brokit_net_has_pending() true forever.
    // (Same orphan policy as websocket.cpp.)
    int closedSweeps = 0;
};

// thread_local: bro pumps __brokit_net_tick from the main thread AND from each
// Worker's own thread (worker.cpp). Per-thread tables mean each JS thread owns
// and pumps only its own sockets — no cross-thread iteration, no locks.
thread_local std::unordered_map<int, NetHandle*> g_handles;
thread_local int g_nextId = 1;

NetHandle* findHandle(JSContext* ctx, JSValueConst idVal)
{
    int id = 0;
    JS_ToInt32(ctx, &id, idVal);
    auto it = g_handles.find(id);
    return it == g_handles.end() ? nullptr : it->second;
}

void cacheLocalInfo(NetHandle* h)
{
    sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    if (getsockname(h->fd, reinterpret_cast<sockaddr*>(&ss), &slen) == 0)
        describeAddr(reinterpret_cast<sockaddr*>(&ss), slen, h->localAddress, h->localPort);
}

void cacheRemoteInfo(NetHandle* h)
{
    sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    if (getpeername(h->fd, reinterpret_cast<sockaddr*>(&ss), &slen) == 0)
        describeAddr(reinterpret_cast<sockaddr*>(&ss), slen, h->remoteAddress, h->remotePort);
}

void hardClose(NetHandle* h)
{
    if (h->fd != BROKIT_INVALID_SOCKET) {
        brokit_closesocket(h->fd);
        h->fd = BROKIT_INVALID_SOCKET;
    }
    h->closed = true;
}

void queueClose(NetHandle* h, bool hadError)
{
    NetEvent ev;
    ev.type = NetEvent::Type::Close;
    ev.hadError = hadError;
    h->events.push_back(std::move(ev));
}

void failHandle(NetHandle* h, const std::string& msg)
{
    if (h->closed) return;
    h->errored = true;
    NetEvent ev;
    ev.type = NetEvent::Type::Error;
    ev.message = msg;
    h->events.push_back(std::move(ev));
    hardClose(h);
    queueClose(h, true);
}

// data: string | ArrayBuffer | any TypedArray/DataView → bytes.
// Strings are encoded as UTF-8 (matching how brokit's fs and WebSocket treat
// string payloads).
bool valueToBytes(JSContext* ctx, JSValueConst v, std::vector<uint8_t>& out)
{
    if (JS_IsString(v)) {
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, v);
        if (!s) return false;
        out.assign(s, s + len);
        JS_FreeCString(ctx, s);
        return true;
    }
    size_t len = 0;
    if (uint8_t* p = JS_GetUint8Array(ctx, &len, v)) {
        out.assign(p, p + len);
        return true;
    }
    JS_FreeValue(ctx, JS_GetException(ctx)); // clear the type-mismatch throw
    if (uint8_t* p = JS_GetArrayBuffer(ctx, &len, v)) {
        out.assign(p, p + len);
        return true;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    // Other TypedArray / DataView: go through .buffer + byteOffset/byteLength.
    JSValue bufv = JS_GetPropertyStr(ctx, v, "buffer");
    if (!JS_IsObject(bufv)) {
        JS_FreeValue(ctx, bufv);
        return false;
    }
    size_t total = 0;
    uint8_t* base = JS_GetArrayBuffer(ctx, &total, bufv);
    JS_FreeValue(ctx, bufv);
    if (!base) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return false;
    }
    int64_t off = 0, blen = 0;
    JSValue offv = JS_GetPropertyStr(ctx, v, "byteOffset");
    JSValue lenv = JS_GetPropertyStr(ctx, v, "byteLength");
    JS_ToInt64(ctx, &off, offv);
    JS_ToInt64(ctx, &blen, lenv);
    JS_FreeValue(ctx, offv);
    JS_FreeValue(ctx, lenv);
    if (off < 0 || blen < 0 || static_cast<size_t>(off + blen) > total) return false;
    out.assign(base + off, base + off + blen);
    return true;
}

// Resolve host:port. family: AF_UNSPEC / AF_INET / AF_INET6.
// Synchronous getaddrinfo — instant for numeric addresses and localhost; a
// remote DNS name blocks the JS thread for the lookup (documented v1 tradeoff).
addrinfo* resolve(const std::string& host, int port, int family, int socktype,
                  bool passive, std::string& err)
{
    addrinfo hints = {};
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    int rc = getaddrinfo(host.empty() ? nullptr : host.c_str(), portStr.c_str(),
                         &hints, &res);
    if (rc != 0) {
#ifdef _WIN32
        err = sockErrorString(rc);
#else
        err = gai_strerror(rc);
#endif
        return nullptr;
    }
    return res;
}

void flushOutBuf(NetHandle* h)
{
    while (!h->outBuf.empty()) {
        int n = ::send(h->fd, reinterpret_cast<const char*>(h->outBuf.data()),
                       static_cast<int>(h->outBuf.size()), 0);
        if (n > 0) {
            h->outBuf.erase(h->outBuf.begin(), h->outBuf.begin() + n);
            continue;
        }
        int e = lastSockError();
        if (errWouldBlock(e)) return;
        failHandle(h, "send failed: " + sockErrorString(e));
        return;
    }
    if (h->endRequested && !h->wroteShutdown && h->outBuf.empty()) {
#ifdef _WIN32
        shutdown(h->fd, SD_SEND);
#else
        shutdown(h->fd, SHUT_WR);
#endif
        h->wroteShutdown = true;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// __brokit_net_tcp_listen(port, host) → id   (throws on failure)
// ---------------------------------------------------------------------------
static JSValue js_net_tcp_listen(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    ensureSocketsInit();
    if (argc < 1) return JS_ThrowTypeError(ctx, "tcp_listen: port required");

    int port = 0;
    JS_ToInt32(ctx, &port, argv[0]);
    std::string host = "127.0.0.1"; // safe default: loopback unless told otherwise
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char* s = JS_ToCString(ctx, argv[1]);
        if (s) { host = s; JS_FreeCString(ctx, s); }
    }

    std::string err;
    addrinfo* res = resolve(host, port, AF_UNSPEC, SOCK_STREAM, true, err);
    if (!res) return JS_ThrowInternalError(ctx, "listen: %s", err.c_str());

    socket_t fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == BROKIT_INVALID_SOCKET) {
        std::string msg = sockErrorString(lastSockError());
        freeaddrinfo(res);
        return JS_ThrowInternalError(ctx, "listen: socket: %s", msg.c_str());
    }
#ifndef _WIN32
    // POSIX: allow fast rebinding after a listener restarts (TIME_WAIT).
    // Deliberately NOT set on Windows, where SO_REUSEADDR allows hijacking a
    // port another process is actively listening on.
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
               sizeof(one));
#endif
    if (::bind(fd, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen)) != 0 ||
        ::listen(fd, 64) != 0) {
        std::string msg = sockErrorString(lastSockError());
        freeaddrinfo(res);
        brokit_closesocket(fd);
        return JS_ThrowInternalError(ctx, "listen %s:%d: %s", host.c_str(), port,
                                     msg.c_str());
    }
    freeaddrinfo(res);
    setNonBlocking(fd);

    auto* h = new NetHandle();
    h->kind = NetHandle::Kind::TcpListener;
    h->id = g_nextId++;
    h->fd = fd;
    cacheLocalInfo(h);
    g_handles[h->id] = h;
    return JS_NewInt32(ctx, h->id);
}

// ---------------------------------------------------------------------------
// __brokit_net_tcp_connect(host, port) → id   (throws on immediate failure)
// ---------------------------------------------------------------------------
static JSValue js_net_tcp_connect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    ensureSocketsInit();
    if (argc < 2) return JS_ThrowTypeError(ctx, "tcp_connect: host and port required");

    const char* hostC = JS_ToCString(ctx, argv[0]);
    if (!hostC) return JS_EXCEPTION;
    std::string host(hostC);
    JS_FreeCString(ctx, hostC);
    int port = 0;
    JS_ToInt32(ctx, &port, argv[1]);

    std::string err;
    addrinfo* res = resolve(host, port, AF_UNSPEC, SOCK_STREAM, false, err);
    if (!res) return JS_ThrowInternalError(ctx, "connect %s:%d: %s", host.c_str(),
                                           port, err.c_str());

    socket_t fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == BROKIT_INVALID_SOCKET) {
        std::string msg = sockErrorString(lastSockError());
        freeaddrinfo(res);
        return JS_ThrowInternalError(ctx, "connect: socket: %s", msg.c_str());
    }
    setNonBlocking(fd);

    auto* h = new NetHandle();
    h->kind = NetHandle::Kind::TcpConn;
    h->id = g_nextId++;
    h->fd = fd;

    int rc = ::connect(fd, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen));
    freeaddrinfo(res);
    if (rc == 0) {
        // Connected synchronously (loopback often does).
        cacheLocalInfo(h);
        cacheRemoteInfo(h);
        NetEvent ev;
        ev.type = NetEvent::Type::Connect;
        h->events.push_back(std::move(ev));
    } else {
        int e = lastSockError();
        if (!errInProgress(e)) {
            // Immediate failure — still return a handle; error surfaces as
            // events so the JS Socket's error path is uniform.
            failHandle(h, "connect failed: " + sockErrorString(e));
        } else {
            h->connecting = true;
        }
    }
    g_handles[h->id] = h;
    return JS_NewInt32(ctx, h->id);
}

// ---------------------------------------------------------------------------
// __brokit_net_write(id, data) → bool
// ---------------------------------------------------------------------------
static JSValue js_net_write(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_FALSE;
    NetHandle* h = findHandle(ctx, argv[0]);
    if (!h || h->kind != NetHandle::Kind::TcpConn || h->closed ||
        h->endRequested || h->wroteShutdown)
        return JS_FALSE;

    std::vector<uint8_t> bytes;
    if (!valueToBytes(ctx, argv[1], bytes))
        return JS_ThrowTypeError(ctx, "write: data must be a string, ArrayBuffer or TypedArray");

    h->outBuf.insert(h->outBuf.end(), bytes.begin(), bytes.end());
    if (!h->connecting) flushOutBuf(h);
    return JS_TRUE;
}

// ---------------------------------------------------------------------------
// __brokit_net_end(id) — graceful: FIN after pending writes drain
// ---------------------------------------------------------------------------
static JSValue js_net_end(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_FALSE;
    NetHandle* h = findHandle(ctx, argv[0]);
    if (!h || h->kind != NetHandle::Kind::TcpConn || h->closed) return JS_FALSE;
    h->endRequested = true;
    if (!h->connecting) {
        flushOutBuf(h);
        if (h->peerEof && h->wroteShutdown && !h->closed) {
            hardClose(h);
            queueClose(h, false);
        }
    }
    return JS_TRUE;
}

// ---------------------------------------------------------------------------
// __brokit_net_close(id) — immediate teardown (destroy / server.close / udp close)
// ---------------------------------------------------------------------------
static JSValue js_net_close(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_FALSE;
    NetHandle* h = findHandle(ctx, argv[0]);
    if (!h || h->closed) return JS_FALSE;
    hardClose(h);
    queueClose(h, false);
    return JS_TRUE;
}

// ---------------------------------------------------------------------------
// __brokit_net_udp_open(ipv6?) → id
// ---------------------------------------------------------------------------
static JSValue js_net_udp_open(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    ensureSocketsInit();
    bool ipv6 = argc >= 1 && JS_ToBool(ctx, argv[0]);

    socket_t fd = ::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == BROKIT_INVALID_SOCKET)
        return JS_ThrowInternalError(ctx, "udp_open: %s",
                                     sockErrorString(lastSockError()).c_str());
    setNonBlocking(fd);

    auto* h = new NetHandle();
    h->kind = NetHandle::Kind::Udp;
    h->id = g_nextId++;
    h->fd = fd;
    h->ipv6 = ipv6;
    g_handles[h->id] = h;
    return JS_NewInt32(ctx, h->id);
}

// ---------------------------------------------------------------------------
// __brokit_net_udp_bind(id, port, host) → bool (throws on bind failure)
// ---------------------------------------------------------------------------
static JSValue js_net_udp_bind(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "udp_bind: id and port required");
    NetHandle* h = findHandle(ctx, argv[0]);
    if (!h || h->kind != NetHandle::Kind::Udp || h->closed) return JS_FALSE;

    int port = 0;
    JS_ToInt32(ctx, &port, argv[1]);
    std::string host = h->ipv6 ? "::1" : "127.0.0.1"; // safe default: loopback
    if (argc >= 3 && JS_IsString(argv[2])) {
        const char* s = JS_ToCString(ctx, argv[2]);
        if (s) { host = s; JS_FreeCString(ctx, s); }
    }

    std::string err;
    addrinfo* res = resolve(host, port, h->ipv6 ? AF_INET6 : AF_INET,
                            SOCK_DGRAM, true, err);
    if (!res) return JS_ThrowInternalError(ctx, "udp bind: %s", err.c_str());

    int rc = ::bind(h->fd, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen));
    freeaddrinfo(res);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "udp bind %s:%d: %s", host.c_str(), port,
                                     sockErrorString(lastSockError()).c_str());
    cacheLocalInfo(h);
    return JS_TRUE;
}

// ---------------------------------------------------------------------------
// __brokit_net_udp_send(id, data, port, host) → bool
// Datagram semantics: sent immediately (no userspace queue); a full kernel
// buffer drops the datagram, which is faithful UDP behavior.
// ---------------------------------------------------------------------------
static JSValue js_net_udp_send(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 4) return JS_FALSE;
    NetHandle* h = findHandle(ctx, argv[0]);
    if (!h || h->kind != NetHandle::Kind::Udp || h->closed) return JS_FALSE;

    std::vector<uint8_t> bytes;
    if (!valueToBytes(ctx, argv[1], bytes))
        return JS_ThrowTypeError(ctx, "send: data must be a string, ArrayBuffer or TypedArray");

    int port = 0;
    JS_ToInt32(ctx, &port, argv[2]);
    const char* hostC = JS_ToCString(ctx, argv[3]);
    if (!hostC) return JS_EXCEPTION;
    std::string host(hostC);
    JS_FreeCString(ctx, hostC);

    std::string err;
    addrinfo* res = resolve(host, port, h->ipv6 ? AF_INET6 : AF_INET,
                            SOCK_DGRAM, false, err);
    if (!res) {
        failHandle(h, "udp send resolve: " + err);
        return JS_FALSE;
    }
    int n = ::sendto(h->fd, reinterpret_cast<const char*>(bytes.data()),
                     static_cast<int>(bytes.size()), 0, res->ai_addr,
                     static_cast<socklen_t>(res->ai_addrlen));
    freeaddrinfo(res);
    if (n < 0) {
        int e = lastSockError();
        if (errWouldBlock(e)) return JS_FALSE; // kernel buffer full → dropped
        failHandle(h, "udp send: " + sockErrorString(e));
        return JS_FALSE;
    }
    return JS_TRUE;
}

// ---------------------------------------------------------------------------
// __brokit_net_set_broadcast(id, on) → bool
// ---------------------------------------------------------------------------
static JSValue js_net_set_broadcast(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_FALSE;
    NetHandle* h = findHandle(ctx, argv[0]);
    if (!h || h->kind != NetHandle::Kind::Udp || h->closed) return JS_FALSE;
    int on = JS_ToBool(ctx, argv[1]) ? 1 : 0;
    int rc = setsockopt(h->fd, SOL_SOCKET, SO_BROADCAST,
                        reinterpret_cast<const char*>(&on), sizeof(on));
    return JS_NewBool(ctx, rc == 0);
}

// ---------------------------------------------------------------------------
// __brokit_net_info(id) → { localAddress, localPort, remoteAddress, remotePort }
// ---------------------------------------------------------------------------
static JSValue js_net_info(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NULL;
    NetHandle* h = findHandle(ctx, argv[0]);
    if (!h) return JS_NULL;
    if (!h->closed && h->localAddress.empty()) cacheLocalInfo(h);

    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "localAddress", JS_NewString(ctx, h->localAddress.c_str()));
    JS_SetPropertyStr(ctx, o, "localPort", JS_NewInt32(ctx, h->localPort));
    JS_SetPropertyStr(ctx, o, "remoteAddress", JS_NewString(ctx, h->remoteAddress.c_str()));
    JS_SetPropertyStr(ctx, o, "remotePort", JS_NewInt32(ctx, h->remotePort));
    return o;
}

// ---------------------------------------------------------------------------
// __brokit_net_poll(id) → event object | null
// Delivering a Close event erases the handle (mirrors __brokit_ws_recv).
// ---------------------------------------------------------------------------
static JSValue js_net_poll(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NULL;
    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    auto it = g_handles.find(id);
    if (it == g_handles.end()) return JS_NULL;
    NetHandle* h = it->second;
    if (h->events.empty()) return JS_NULL;

    NetEvent ev = std::move(h->events.front());
    h->events.pop_front();

    JSValue o = JS_NewObject(ctx);
    switch (ev.type) {
    case NetEvent::Type::Accept:
        JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, "accept"));
        JS_SetPropertyStr(ctx, o, "connId", JS_NewInt32(ctx, ev.connId));
        JS_SetPropertyStr(ctx, o, "address", JS_NewString(ctx, ev.address.c_str()));
        JS_SetPropertyStr(ctx, o, "port", JS_NewInt32(ctx, ev.port));
        break;
    case NetEvent::Type::Connect:
        JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, "connect"));
        break;
    case NetEvent::Type::Data:
        JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, "data"));
        JS_SetPropertyStr(ctx, o, "data",
            JS_NewUint8ArrayCopy(ctx, ev.data.data(), ev.data.size()));
        break;
    case NetEvent::Type::End:
        JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, "end"));
        break;
    case NetEvent::Type::Message:
        JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, "message"));
        JS_SetPropertyStr(ctx, o, "data",
            JS_NewUint8ArrayCopy(ctx, ev.data.data(), ev.data.size()));
        JS_SetPropertyStr(ctx, o, "address", JS_NewString(ctx, ev.address.c_str()));
        JS_SetPropertyStr(ctx, o, "port", JS_NewInt32(ctx, ev.port));
        JS_SetPropertyStr(ctx, o, "family", JS_NewString(ctx, ev.family.c_str()));
        break;
    case NetEvent::Type::Error:
        JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, "error"));
        JS_SetPropertyStr(ctx, o, "message", JS_NewString(ctx, ev.message.c_str()));
        break;
    case NetEvent::Type::Close:
        JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, "close"));
        JS_SetPropertyStr(ctx, o, "hadError", JS_NewBool(ctx, ev.hadError));
        // Close is always the final event; retire the handle.
        g_handles.erase(it);
        delete h;
        break;
    }
    return o;
}

// ---------------------------------------------------------------------------
// __brokit_net_tick() — pump every socket, then let JS drain
// ---------------------------------------------------------------------------
static JSValue js_net_tick(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (g_handles.empty()) return JS_NewInt32(ctx, 0);

    // Snapshot ids — accepts insert into g_handles during iteration.
    std::vector<int> ids;
    ids.reserve(g_handles.size());
    for (auto& [id, h] : g_handles) ids.push_back(id);

    for (int id : ids) {
        auto it = g_handles.find(id);
        if (it == g_handles.end()) continue;
        NetHandle* h = it->second;
        if (h->closed) continue;

        switch (h->kind) {
        case NetHandle::Kind::TcpListener: {
            for (int i = 0; i < 16; i++) {
                sockaddr_storage ss;
                socklen_t slen = sizeof(ss);
                socket_t cfd = ::accept(h->fd, reinterpret_cast<sockaddr*>(&ss), &slen);
                if (cfd == BROKIT_INVALID_SOCKET) break;
                setNonBlocking(cfd);

                auto* c = new NetHandle();
                c->kind = NetHandle::Kind::TcpConn;
                c->id = g_nextId++;
                c->fd = cfd;
                describeAddr(reinterpret_cast<sockaddr*>(&ss), slen,
                             c->remoteAddress, c->remotePort);
                cacheLocalInfo(c);
                g_handles[c->id] = c;

                NetEvent ev;
                ev.type = NetEvent::Type::Accept;
                ev.connId = c->id;
                ev.address = c->remoteAddress;
                ev.port = c->remotePort;
                h->events.push_back(std::move(ev));
            }
            break;
        }
        case NetHandle::Kind::TcpConn: {
            if (h->connecting) {
                // Nonblocking connect progress: writable ⇒ done, then SO_ERROR
                // says whether it succeeded.
                fd_set wfds, efds;
                FD_ZERO(&wfds);
                FD_ZERO(&efds);
                FD_SET(h->fd, &wfds);
                FD_SET(h->fd, &efds);
                timeval tv = {0, 0};
                int nfds = static_cast<int>(h->fd) + 1;
                int sel = ::select(nfds, nullptr, &wfds, &efds, &tv);
                if (sel > 0) {
                    int soerr = 0;
                    socklen_t elen = sizeof(soerr);
                    getsockopt(h->fd, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&soerr), &elen);
                    if (soerr == 0 && !FD_ISSET(h->fd, &efds)) {
                        h->connecting = false;
                        cacheLocalInfo(h);
                        cacheRemoteInfo(h);
                        NetEvent ev;
                        ev.type = NetEvent::Type::Connect;
                        h->events.push_back(std::move(ev));
                        flushOutBuf(h); // writes queued while connecting
                    } else {
                        failHandle(h, "connect failed: " +
                                          sockErrorString(soerr ? soerr :
#ifdef _WIN32
                                                          WSAECONNREFUSED
#else
                                                          ECONNREFUSED
#endif
                                                          ));
                    }
                }
                break;
            }

            // Read: drain what's available, bounded per tick.
            char buf[65536];
            for (int i = 0; i < 8 && !h->closed; i++) {
                int n = ::recv(h->fd, buf, sizeof(buf), 0);
                if (n > 0) {
                    NetEvent ev;
                    ev.type = NetEvent::Type::Data;
                    ev.data.assign(buf, buf + n);
                    h->events.push_back(std::move(ev));
                    continue;
                }
                if (n == 0) {
                    if (!h->peerEof) {
                        h->peerEof = true;
                        NetEvent ev;
                        ev.type = NetEvent::Type::End;
                        h->events.push_back(std::move(ev));
                        // Node allowHalfOpen:false semantics — answer FIN with
                        // our own once pending writes drain.
                        h->endRequested = true;
                    }
                    break;
                }
                int e = lastSockError();
                if (errWouldBlock(e)) break;
#ifdef _WIN32
                bool reset = (e == WSAECONNRESET || e == WSAECONNABORTED);
#else
                bool reset = (e == ECONNRESET);
#endif
                if (reset)
                    failHandle(h, "connection reset by peer");
                else
                    failHandle(h, "recv failed: " + sockErrorString(e));
                break;
            }
            if (h->closed) break;

            flushOutBuf(h);
            if (h->closed) break;

            if (h->peerEof && h->wroteShutdown) {
                hardClose(h);
                queueClose(h, false);
            }
            break;
        }
        case NetHandle::Kind::Udp: {
            char buf[65536];
            for (int i = 0; i < 16 && !h->closed; i++) {
                sockaddr_storage ss;
                socklen_t slen = sizeof(ss);
                int n = ::recvfrom(h->fd, buf, sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&ss), &slen);
                if (n < 0) {
                    int e = lastSockError();
#ifdef _WIN32
                    // A previous send to a dead port surfaces here as
                    // WSAECONNRESET (ICMP port unreachable). Not fatal for UDP.
                    if (e == WSAECONNRESET) continue;
#endif
                    if (!errWouldBlock(e))
                        failHandle(h, "recvfrom failed: " + sockErrorString(e));
                    break;
                }
                NetEvent ev;
                ev.type = NetEvent::Type::Message;
                ev.data.assign(buf, buf + n);
                describeAddr(reinterpret_cast<sockaddr*>(&ss), slen, ev.address, ev.port);
                ev.family = (ss.ss_family == AF_INET6) ? "IPv6" : "IPv4";
                h->events.push_back(std::move(ev));
            }
            break;
        }
        }
    }

    // Deliver queued events to the JS wrappers (net.js registers this hook).
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue drainFn = JS_GetPropertyStr(ctx, global, "__brokit_net_drain_all");
        if (JS_IsFunction(ctx, drainFn)) {
            JSValue ret = JS_Call(ctx, drainFn, JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(ret)) Runtime::checkException(ctx, ret);
            JS_FreeValue(ctx, ret);
        }
        JS_FreeValue(ctx, drainFn);
        JS_FreeValue(ctx, global);
    }

    // Reap orphans: fully-closed handles whose events nobody drained (raw
    // binding users). One tick of grace, same policy as websocket.cpp.
    for (auto it = g_handles.begin(); it != g_handles.end();) {
        NetHandle* h = it->second;
        if (h->closed && ++h->closedSweeps > 1) {
            delete h;
            it = g_handles.erase(it);
        } else {
            ++it;
        }
    }

    return JS_NewInt32(ctx, 0);
}

// ---------------------------------------------------------------------------
// __brokit_net_has_pending() → bool
// True while any socket is live (a listener, bound UDP socket, or open TCP
// connection can produce events with no other wakeup signal — same reasoning
// as __brokit_ws_has_pending, and the same consequence: close your sockets or
// the host keeps polling).
// ---------------------------------------------------------------------------
static JSValue js_net_has_pending(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    for (auto& [id, h] : g_handles) {
        if (!h->closed) return JS_TRUE;
        if (!h->events.empty()) return JS_TRUE;
    }
    return JS_FALSE;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
void installNet(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    JS_SetPropertyStr(ctx, global, "__brokit_net_tcp_listen",
        JS_NewCFunction(ctx, js_net_tcp_listen, "__brokit_net_tcp_listen", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_net_tcp_connect",
        JS_NewCFunction(ctx, js_net_tcp_connect, "__brokit_net_tcp_connect", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_net_write",
        JS_NewCFunction(ctx, js_net_write, "__brokit_net_write", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_net_end",
        JS_NewCFunction(ctx, js_net_end, "__brokit_net_end", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_net_close",
        JS_NewCFunction(ctx, js_net_close, "__brokit_net_close", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_net_udp_open",
        JS_NewCFunction(ctx, js_net_udp_open, "__brokit_net_udp_open", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_net_udp_bind",
        JS_NewCFunction(ctx, js_net_udp_bind, "__brokit_net_udp_bind", 3));
    JS_SetPropertyStr(ctx, global, "__brokit_net_udp_send",
        JS_NewCFunction(ctx, js_net_udp_send, "__brokit_net_udp_send", 4));
    JS_SetPropertyStr(ctx, global, "__brokit_net_set_broadcast",
        JS_NewCFunction(ctx, js_net_set_broadcast, "__brokit_net_set_broadcast", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_net_info",
        JS_NewCFunction(ctx, js_net_info, "__brokit_net_info", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_net_poll",
        JS_NewCFunction(ctx, js_net_poll, "__brokit_net_poll", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_net_tick",
        JS_NewCFunction(ctx, js_net_tick, "__brokit_net_tick", 0));
    JS_SetPropertyStr(ctx, global, "__brokit_net_has_pending",
        JS_NewCFunction(ctx, js_net_has_pending, "__brokit_net_has_pending", 0));

    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
