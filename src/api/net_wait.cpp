// Cross-thread wake pipe/socket + select() wait for event-driven hosts.

#include "api/net_internal.h"

namespace brokit::api {

namespace {

std::atomic<bool> g_wakeSignaled{false};
std::mutex g_wakeInitMu;
socket_t g_wakeReadFd = BROKIT_INVALID_SOCKET;
socket_t g_wakeWriteFd = BROKIT_INVALID_SOCKET;

void ensureWakePipe()
{
    std::lock_guard<std::mutex> lk(g_wakeInitMu);
    if (g_wakeReadFd != BROKIT_INVALID_SOCKET) return;
    ensureSocketsInit();
#ifdef _WIN32
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != BROKIT_INVALID_SOCKET) {
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            int len = sizeof(addr);
            if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0 &&
                ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                setNonBlocking(s);
                g_wakeReadFd = s;
                g_wakeWriteFd = s;
                return;
            }
        }
        brokit_closesocket(s);
    }
#else
    int p[2] = {-1, -1};
    if (::pipe(p) == 0) {
        fcntl(p[0], F_SETFD, FD_CLOEXEC);
        fcntl(p[1], F_SETFD, FD_CLOEXEC);
        setNonBlocking(p[0]);
        setNonBlocking(p[1]);
        g_wakeReadFd = p[0];
        g_wakeWriteFd = p[1];
    }
#endif
}

void drainWakePipe()
{
    g_wakeSignaled.store(false, std::memory_order_release);
    if (g_wakeReadFd == BROKIT_INVALID_SOCKET) return;
    char buf[64];
    for (;;) {
#ifdef _WIN32
        int n = ::recv(g_wakeReadFd, buf, sizeof(buf), 0);
#else
        ssize_t n = ::read(g_wakeReadFd, buf, sizeof(buf));
#endif
        if (n <= 0) break;
    }
}

} // namespace

void wakeNetWait()
{
    if (g_wakeSignaled.exchange(true, std::memory_order_acq_rel)) return;
    socket_t wfd = g_wakeWriteFd;
    if (wfd == BROKIT_INVALID_SOCKET) return;
    const char b = 1;
#ifdef _WIN32
    ::send(wfd, &b, 1, 0);
#else
    auto ignored = ::write(wfd, &b, 1);
    (void)ignored;
#endif
}

bool waitForNetEvents(double timeoutMs)
{
    ensureWakePipe();
    if (g_wakeSignaled.load(std::memory_order_acquire)) {
        drainWakePipe();
        return true;
    }
    auto& handles = netHandles();
    for (auto& [id, h] : handles) {
        if (!h->events.empty()) return true;
    }
    if (timeoutMs <= 0.0) return false;

    fd_set rfds, wfds, efds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);

    int maxFd = -1;
    bool hasRead = false;
    bool hasWrite = false;
    bool hasExcept = false;

    auto addFd = [&](socket_t fd, fd_set& set, bool& flag) {
        if (fd == BROKIT_INVALID_SOCKET) return;
#ifndef _WIN32
        if (fd < 0 || fd >= FD_SETSIZE) return;
        if (static_cast<int>(fd) > maxFd) maxFd = static_cast<int>(fd);
#endif
        FD_SET(fd, &set);
        flag = true;
    };

    if (g_wakeReadFd != BROKIT_INVALID_SOCKET) {
        addFd(g_wakeReadFd, rfds, hasRead);
    }

    for (auto& [id, h] : handles) {
        if (h->closed || h->fd == BROKIT_INVALID_SOCKET) continue;
        switch (h->kind) {
        case NetHandle::Kind::TcpListener:
        case NetHandle::Kind::Udp:
            addFd(h->fd, rfds, hasRead);
            break;
        case NetHandle::Kind::TcpConn:
            if (h->connecting) {
                addFd(h->fd, wfds, hasWrite);
                addFd(h->fd, efds, hasExcept);
            } else {
                if (!h->peerEof) addFd(h->fd, rfds, hasRead);
                if (!h->outBuf.empty()) addFd(h->fd, wfds, hasWrite);
            }
            break;
        }
    }

    if (!hasRead && !hasWrite && !hasExcept) return false;

    if (timeoutMs > 60000.0) timeoutMs = 60000.0;
    long secs = static_cast<long>(timeoutMs / 1000.0);
    long usecs = static_cast<long>((timeoutMs - static_cast<double>(secs) * 1000.0) * 1000.0);
    if (usecs < 0) usecs = 0;
    if (usecs >= 1000000) {
        secs += usecs / 1000000;
        usecs %= 1000000;
    }
    timeval tv = {secs, static_cast<decltype(tv.tv_usec)>(usecs)};

    int rc = ::select(maxFd + 1,
                      hasRead ? &rfds : nullptr,
                      hasWrite ? &wfds : nullptr,
                      hasExcept ? &efds : nullptr,
                      &tv);

    if ((g_wakeReadFd != BROKIT_INVALID_SOCKET && rc > 0 && FD_ISSET(g_wakeReadFd, &rfds)) ||
        g_wakeSignaled.load(std::memory_order_acquire)) {
        drainWakePipe();
    }
    return rc > 0;
}

} // namespace brokit::api
