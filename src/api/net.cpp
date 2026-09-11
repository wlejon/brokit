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
#include "api/arg_reader.h"
#include "api/object_builder.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include <span>

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

    int closedSweeps = 0;
};

// thread_local: each JS thread owns and pumps only its own sockets
thread_local std::unordered_map<int, NetHandle*> g_handles;
thread_local int g_nextId = 1;

NetHandle* findHandle(int id)
{
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
bool valueToBytes(bronze::Value v, std::vector<uint8_t>& out)
{
    if (ev::isString(v)) {
        std::string s = ev::toUtf8(v);
        out.assign(s.begin(), s.end());
        return true;
    }
    if (auto info = ev::typedArrayInfo(v)) {
        out.assign(info.data, info.data + info.byteLength);
        return true;
    }
    if (auto info = ev::arrayBufferInfo(v)) {
        out.assign(info.data, info.data + info.byteLength);
        return true;
    }
    if (ev::isObject(v)) {
        auto u8 = ev::getProperty(v, "_u8");
        if (auto info = ev::typedArrayInfo(u8)) {
            out.assign(info.data, info.data + info.byteLength);
            return true;
        }
    }
    return false;
}

// Resolve host:port. family: AF_UNSPEC / AF_INET / AF_INET6.
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
// __brokit_net_tcp_listen(port, host) → id (throws on failure)
// ---------------------------------------------------------------------------
static bronze::Value js_net_tcp_listen(bronze::Value, std::span<const bronze::Value> args)
{
    ensureSocketsInit();
    if (args.empty()) return ev::throwTypeError("tcp_listen: port required");

    ArgReader reader(args);
    int port = reader.getInt(0, 0);
    std::string host = reader.getString(1, "127.0.0.1");

    std::string err;
    addrinfo* res = resolve(host, port, AF_UNSPEC, SOCK_STREAM, true, err);
    if (!res) return ev::throwError("listen: " + err);

    socket_t fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == BROKIT_INVALID_SOCKET) {
        std::string msg = sockErrorString(lastSockError());
        freeaddrinfo(res);
        return ev::throwError("listen: socket: " + msg);
    }
#ifndef _WIN32
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
               sizeof(one));
#endif
    if (::bind(fd, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen)) != 0 ||
        ::listen(fd, 64) != 0) {
        std::string msg = sockErrorString(lastSockError());
        freeaddrinfo(res);
        brokit_closesocket(fd);
        return ev::throwError("listen " + host + ":" + std::to_string(port) + ": " + msg);
    }
    freeaddrinfo(res);
    setNonBlocking(fd);

    auto* h = new NetHandle();
    h->kind = NetHandle::Kind::TcpListener;
    h->id = g_nextId++;
    h->fd = fd;
    cacheLocalInfo(h);
    g_handles[h->id] = h;
    return ev::fromDouble(h->id);
}

// ---------------------------------------------------------------------------
// __brokit_net_tcp_connect(host, port) → id (throws on immediate failure)
// ---------------------------------------------------------------------------
static bronze::Value js_net_tcp_connect(bronze::Value, std::span<const bronze::Value> args)
{
    ensureSocketsInit();
    if (args.size() < 2) return ev::throwTypeError("tcp_connect: host and port required");

    ArgReader reader(args);
    std::string host = reader.getString(0, "");
    int port = reader.getInt(1, 0);

    std::string err;
    addrinfo* res = resolve(host, port, AF_UNSPEC, SOCK_STREAM, false, err);
    if (!res) return ev::throwError("connect " + host + ":" + std::to_string(port) + ": " + err);

    socket_t fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == BROKIT_INVALID_SOCKET) {
        std::string msg = sockErrorString(lastSockError());
        freeaddrinfo(res);
        return ev::throwError("connect: socket: " + msg);
    }
    setNonBlocking(fd);

    auto* h = new NetHandle();
    h->kind = NetHandle::Kind::TcpConn;
    h->id = g_nextId++;
    h->fd = fd;

    int rc = ::connect(fd, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen));
    freeaddrinfo(res);
    if (rc == 0) {
        cacheLocalInfo(h);
        cacheRemoteInfo(h);
        NetEvent evNet;
        evNet.type = NetEvent::Type::Connect;
        h->events.push_back(std::move(evNet));
    } else {
        int e = lastSockError();
        if (!errInProgress(e)) {
            failHandle(h, "connect failed: " + sockErrorString(e));
        } else {
            h->connecting = true;
        }
    }
    g_handles[h->id] = h;
    return ev::fromDouble(h->id);
}

// ---------------------------------------------------------------------------
// __brokit_net_write(id, data) → bool
// ---------------------------------------------------------------------------
static bronze::Value js_net_write(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 2) return ev::fromBool(false);
    ArgReader reader(args);
    NetHandle* h = findHandle(reader.getInt(0, 0));
    if (!h || h->kind != NetHandle::Kind::TcpConn || h->closed ||
        h->endRequested || h->wroteShutdown)
        return ev::fromBool(false);

    std::vector<uint8_t> bytes;
    if (!valueToBytes(reader.get(1), bytes))
        return ev::throwTypeError("write: data must be a string, ArrayBuffer or TypedArray");

    h->outBuf.insert(h->outBuf.end(), bytes.begin(), bytes.end());
    if (!h->connecting) flushOutBuf(h);
    return ev::fromBool(true);
}

// ---------------------------------------------------------------------------
// __brokit_net_end(id) — graceful: FIN after pending writes drain
// ---------------------------------------------------------------------------
static bronze::Value js_net_end(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty()) return ev::fromBool(false);
    ArgReader reader(args);
    NetHandle* h = findHandle(reader.getInt(0, 0));
    if (!h || h->kind != NetHandle::Kind::TcpConn || h->closed) return ev::fromBool(false);
    h->endRequested = true;
    if (!h->connecting) {
        flushOutBuf(h);
        if (h->peerEof && h->wroteShutdown && !h->closed) {
            hardClose(h);
            queueClose(h, false);
        }
    }
    return ev::fromBool(true);
}

// ---------------------------------------------------------------------------
// __brokit_net_close(id) — immediate teardown
// ---------------------------------------------------------------------------
static bronze::Value js_net_close(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty()) return ev::fromBool(false);
    ArgReader reader(args);
    NetHandle* h = findHandle(reader.getInt(0, 0));
    if (!h || h->closed) return ev::fromBool(false);
    hardClose(h);
    queueClose(h, false);
    return ev::fromBool(true);
}

// ---------------------------------------------------------------------------
// __brokit_net_udp_open(ipv6?) → id
// ---------------------------------------------------------------------------
static bronze::Value js_net_udp_open(bronze::Value, std::span<const bronze::Value> args)
{
    ensureSocketsInit();
    ArgReader reader(args);
    bool ipv6 = reader.getBool(0, false);

    socket_t fd = ::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == BROKIT_INVALID_SOCKET)
        return ev::throwError("udp_open: " + sockErrorString(lastSockError()));
    setNonBlocking(fd);

    auto* h = new NetHandle();
    h->kind = NetHandle::Kind::Udp;
    h->id = g_nextId++;
    h->fd = fd;
    h->ipv6 = ipv6;
    g_handles[h->id] = h;
    return ev::fromDouble(h->id);
}

// ---------------------------------------------------------------------------
// __brokit_net_udp_bind(id, port, host) → bool (throws on bind failure)
// ---------------------------------------------------------------------------
static bronze::Value js_net_udp_bind(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 2) return ev::throwTypeError("udp_bind: id and port required");
    ArgReader reader(args);
    NetHandle* h = findHandle(reader.getInt(0, 0));
    if (!h || h->kind != NetHandle::Kind::Udp || h->closed) return ev::fromBool(false);

    int port = reader.getInt(1, 0);
    std::string defaultHost = h->ipv6 ? "::1" : "127.0.0.1";
    std::string host = reader.getString(2, defaultHost);

    std::string err;
    addrinfo* res = resolve(host, port, h->ipv6 ? AF_INET6 : AF_INET,
                            SOCK_DGRAM, true, err);
    if (!res) return ev::throwError("udp bind: " + err);

    int rc = ::bind(h->fd, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen));
    freeaddrinfo(res);
    if (rc != 0)
        return ev::throwError("udp bind " + host + ":" + std::to_string(port) + ": " +
                              sockErrorString(lastSockError()));
    cacheLocalInfo(h);
    return ev::fromBool(true);
}

// ---------------------------------------------------------------------------
// __brokit_net_udp_send(id, data, port, host) → bool
// ---------------------------------------------------------------------------
static bronze::Value js_net_udp_send(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 4) return ev::fromBool(false);
    ArgReader reader(args);
    NetHandle* h = findHandle(reader.getInt(0, 0));
    if (!h || h->kind != NetHandle::Kind::Udp || h->closed) return ev::fromBool(false);

    std::vector<uint8_t> bytes;
    if (!valueToBytes(reader.get(1), bytes))
        return ev::throwTypeError("send: data must be a string, ArrayBuffer or TypedArray");

    int port = reader.getInt(2, 0);
    std::string host = reader.getString(3, "");

    std::string err;
    addrinfo* res = resolve(host, port, h->ipv6 ? AF_INET6 : AF_INET,
                            SOCK_DGRAM, false, err);
    if (!res) {
        failHandle(h, "udp send resolve: " + err);
        return ev::fromBool(false);
    }
    int n = ::sendto(h->fd, reinterpret_cast<const char*>(bytes.data()),
                     static_cast<int>(bytes.size()), 0, res->ai_addr,
                     static_cast<socklen_t>(res->ai_addrlen));
    freeaddrinfo(res);
    if (n < 0) {
        int e = lastSockError();
        if (errWouldBlock(e)) return ev::fromBool(false);
        failHandle(h, "udp send: " + sockErrorString(e));
        return ev::fromBool(false);
    }
    return ev::fromBool(true);
}

// ---------------------------------------------------------------------------
// __brokit_net_set_broadcast(id, on) → bool
// ---------------------------------------------------------------------------
static bronze::Value js_net_set_broadcast(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 2) return ev::fromBool(false);
    ArgReader reader(args);
    NetHandle* h = findHandle(reader.getInt(0, 0));
    if (!h || h->kind != NetHandle::Kind::Udp || h->closed) return ev::fromBool(false);
    int on = reader.getBool(1, false) ? 1 : 0;
    int rc = setsockopt(h->fd, SOL_SOCKET, SO_BROADCAST,
                        reinterpret_cast<const char*>(&on), sizeof(on));
    return ev::fromBool(rc == 0);
}

// ---------------------------------------------------------------------------
// __brokit_net_info(id) → { localAddress, localPort, remoteAddress, remotePort }
// ---------------------------------------------------------------------------
static bronze::Value js_net_info(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty()) return ev::null();
    ArgReader reader(args);
    NetHandle* h = findHandle(reader.getInt(0, 0));
    if (!h) return ev::null();
    if (!h->closed && h->localAddress.empty()) cacheLocalInfo(h);

    ObjectBuilder obj;
    obj.set("localAddress", h->localAddress);
    obj.set("localPort", static_cast<double>(h->localPort));
    obj.set("remoteAddress", h->remoteAddress);
    obj.set("remotePort", static_cast<double>(h->remotePort));
    return obj.build();
}

// ---------------------------------------------------------------------------
// __brokit_net_poll(id) → event object | null
// ---------------------------------------------------------------------------
static bronze::Value js_net_poll(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty()) return ev::null();
    ArgReader reader(args);
    int id = reader.getInt(0, 0);
    auto it = g_handles.find(id);
    if (it == g_handles.end()) return ev::null();
    NetHandle* h = it->second;
    if (h->events.empty()) return ev::null();

    NetEvent evNet = std::move(h->events.front());
    h->events.pop_front();

    ObjectBuilder obj;
    switch (evNet.type) {
    case NetEvent::Type::Accept:
        obj.set("type", "accept");
        obj.set("connId", static_cast<double>(evNet.connId));
        obj.set("address", evNet.address);
        obj.set("port", static_cast<double>(evNet.port));
        break;
    case NetEvent::Type::Connect:
        obj.set("type", "connect");
        break;
    case NetEvent::Type::Data: {
        obj.set("type", "data");
        bronze::Value ab = ev::createArrayBuffer(std::span<const uint8_t>(evNet.data.data(), evNet.data.size()));
        bronze::Value u8 = ev::createTypedArrayView(elements::Uint8, ab, 0, static_cast<uint32_t>(evNet.data.size()));
        obj.set("data", u8);
        break;
    }
    case NetEvent::Type::End:
        obj.set("type", "end");
        break;
    case NetEvent::Type::Message: {
        obj.set("type", "message");
        bronze::Value ab = ev::createArrayBuffer(std::span<const uint8_t>(evNet.data.data(), evNet.data.size()));
        bronze::Value u8 = ev::createTypedArrayView(elements::Uint8, ab, 0, static_cast<uint32_t>(evNet.data.size()));
        obj.set("data", u8);
        obj.set("address", evNet.address);
        obj.set("port", static_cast<double>(evNet.port));
        obj.set("family", evNet.family);
        break;
    }
    case NetEvent::Type::Error:
        obj.set("type", "error");
        obj.set("message", evNet.message);
        break;
    case NetEvent::Type::Close:
        obj.set("type", "close");
        obj.set("hadError", evNet.hadError);
        g_handles.erase(it);
        delete h;
        break;
    }
    return obj.build();
}

// ---------------------------------------------------------------------------
// __brokit_net_tick() — pump every socket, then let JS drain
// ---------------------------------------------------------------------------
static bronze::Value js_net_tick(bronze::Value, std::span<const bronze::Value>)
{
    if (g_handles.empty()) return ev::fromDouble(0);

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

                NetEvent evNet;
                evNet.type = NetEvent::Type::Accept;
                evNet.connId = c->id;
                evNet.address = c->remoteAddress;
                evNet.port = c->remotePort;
                h->events.push_back(std::move(evNet));
            }
            break;
        }
        case NetHandle::Kind::TcpConn: {
            if (h->connecting) {
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
                        NetEvent evNet;
                        evNet.type = NetEvent::Type::Connect;
                        h->events.push_back(std::move(evNet));
                        flushOutBuf(h);
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
                    NetEvent evNet;
                    evNet.type = NetEvent::Type::Data;
                    evNet.data.assign(buf, buf + n);
                    h->events.push_back(std::move(evNet));
                    continue;
                }
                if (n == 0) {
                    if (!h->peerEof) {
                        h->peerEof = true;
                        NetEvent evNet;
                        evNet.type = NetEvent::Type::End;
                        h->events.push_back(std::move(evNet));
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
                    if (e == WSAECONNRESET) continue;
#endif
                    if (!errWouldBlock(e))
                        failHandle(h, "recvfrom failed: " + sockErrorString(e));
                    break;
                }
                NetEvent evNet;
                evNet.type = NetEvent::Type::Message;
                evNet.data.assign(buf, buf + n);
                describeAddr(reinterpret_cast<sockaddr*>(&ss), slen, evNet.address, evNet.port);
                evNet.family = (ss.ss_family == AF_INET6) ? "IPv6" : "IPv4";
                h->events.push_back(std::move(evNet));
            }
            break;
        }
        }
    }

    // Deliver queued events to the JS wrappers
    {
        bronze::Value drainFn = ev::getGlobal("__brokit_net_drain_all");
        if (ev::isFunction(drainFn)) {
            ev::call(drainFn, ev::undefined(), {});
        }
    }

    // Reap orphans
    for (auto it = g_handles.begin(); it != g_handles.end();) {
        NetHandle* h = it->second;
        if (h->closed && ++h->closedSweeps > 1) {
            delete h;
            it = g_handles.erase(it);
        } else {
            ++it;
        }
    }

    return ev::fromDouble(0);
}

// ---------------------------------------------------------------------------
// __brokit_net_has_pending() → bool
// ---------------------------------------------------------------------------
static bronze::Value js_net_has_pending(bronze::Value, std::span<const bronze::Value>)
{
    for (auto& [id, h] : g_handles) {
        if (!h->closed) return ev::fromBool(true);
        if (!h->events.empty()) return ev::fromBool(true);
    }
    return ev::fromBool(false);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
void installNet()
{
    ev::registerFunction("__brokit_net_tcp_listen", js_net_tcp_listen);
    ev::registerFunction("__brokit_net_tcp_connect", js_net_tcp_connect);
    ev::registerFunction("__brokit_net_write", js_net_write);
    ev::registerFunction("__brokit_net_end", js_net_end);
    ev::registerFunction("__brokit_net_close", js_net_close);
    ev::registerFunction("__brokit_net_udp_open", js_net_udp_open);
    ev::registerFunction("__brokit_net_udp_bind", js_net_udp_bind);
    ev::registerFunction("__brokit_net_udp_send", js_net_udp_send);
    ev::registerFunction("__brokit_net_set_broadcast", js_net_set_broadcast);
    ev::registerFunction("__brokit_net_info", js_net_info);
    ev::registerFunction("__brokit_net_poll", js_net_poll);
    ev::registerFunction("__brokit_net_tick", js_net_tick);
    ev::registerFunction("__brokit_net_has_pending", js_net_has_pending);
}

} // namespace brokit::api
