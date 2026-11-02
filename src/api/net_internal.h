#pragma once

#include "api/api.h"
#include "api/access_policy.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>
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
    std::string errorName;       // Error: "PermissionRevoked" when the grant was taken back
    std::string errorCode;       // Error: the refusal's code
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

    // Held open under the access gate (access_policy.h): the URL it was
    // allowed for, which a listener's accepted connections are held under too.
    std::string accessSubject;
    uint64_t held = 0;
};

void ensureSocketsInit();
bool setNonBlocking(socket_t fd);
std::unordered_map<int, NetHandle*>& netHandles();

} // namespace brokit::api
