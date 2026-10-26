#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "embed/embed.h"

namespace bronze::embed {
inline bool isDouble(Value v) { return isNumber(v); }
// `val` must be fresh (nothing allocated since it was produced); it is rooted
// before the globalThis lookup, which may allocate.
inline void setGlobalValue(std::string_view name, Value val) {
    Persistent v{val};
    registerGlobal(name, v.get());
    auto g = globalValue("globalThis");
    if (g.found && isObject(g.value)) setProperty(g.value, name, v.get());
}
inline void setGlobalFunction(std::string_view name, uint32_t arity, NativeFn fn) {
    setGlobalValue(name, makeFunction(std::move(fn), arity, name));
}
inline void registerFunction(std::string_view name, NativeFn fn, uint32_t arity = 0) {
    setGlobalValue(name, makeFunction(std::move(fn), arity, name));
}
inline Value getGlobal(std::string_view name) {
    auto gv = globalValue(name);
    return gv.found ? gv.value : undefined();
}
} // namespace bronze::embed

namespace brokit::api {

namespace ev = bronze::embed;
namespace elements = bronze::embed::elements;
using Value = bronze::Value;

/// Helper wrapping ev::Persistent with valid()/reset() semantics
class PersistentSlot {
public:
    PersistentSlot() = default;
    explicit PersistentSlot(bronze::Value v) : p_(v) {}

    bool valid() const { return !bronze::embed::isUndefined(p_.get()); }
    void reset() { p_.set(bronze::embed::undefined()); }
    void set(bronze::Value v) { p_.set(v); }
    bronze::Value get() const { return p_.get(); }

private:
    bronze::embed::Persistent p_;
};

/// `new <ctorName>(message)` for a global error constructor (Error,
/// TypeError, ...), built GC-safely: the message is rooted before the
/// constructor is looked up and called. Falls back to the message string.
inline bronze::Value newError(std::string_view ctorName, std::string_view message) {
    namespace ev = bronze::embed;
    ev::Persistent msg{ev::fromUtf8(message)};
    auto g = ev::globalValue(ctorName);
    if (!g.found || !ev::isFunction(g.value)) return msg.get();
    ev::Persistent ctor{g.value};
    const bronze::Value args[1] = {msg.get()};
    ev::CallResult r = ev::construct(ctor.get(), std::span<const bronze::Value>(args, 1));
    return r.thrown ? msg.get() : r.value;
}

/// Throw `new DOMException(message, name)` into the running program (falls
/// back to an Error whose message carries the name). Returns what the
/// NativeFn should return.
inline bronze::Value throwDOMException(const std::string& message, const std::string& name) {
    namespace ev = bronze::embed;
    auto de = ev::globalValue("DOMException");
    if (de.found && ev::isFunction(de.value)) {
        ev::Persistent ctor{de.value};
        ev::Persistent msg{ev::fromUtf8(message)};
        ev::Persistent nm{ev::fromUtf8(name)};
        const bronze::Value args[2] = {msg.get(), nm.get()};
        ev::CallResult res = ev::construct(ctor.get(), std::span<const bronze::Value>(args, 2));
        if (!res.thrown) return ev::throwValue(res.value);
    }
    return ev::throwError(name + ": " + message);
}

void pushRequireDir(const std::filesystem::path& dir);
void popRequireDir();

struct RequireDirGuard {
    explicit RequireDirGuard(const std::filesystem::path& dir) {
        pushRequireDir(dir);
    }
    ~RequireDirGuard() {
        popRequireDir();
    }
    RequireDirGuard(const RequireDirGuard&) = delete;
    RequireDirGuard& operator=(const RequireDirGuard&) = delete;
};

/// Install all brokit APIs into the current thread's Bronze runtime.
void installAll();

// Individual API installers — consumers can pick and choose.
/// The `globalThis.__brokit_modules` registry that `require()` consults.
/// Idempotent; run it before any installer that registers a module.
void installModuleRegistry();
/// `require()` itself. Run it last, after every module it should find.
void installRequire();
void installConsole();
void installTimers();
void installURL();
void installCrypto();
void installSubtleCrypto();
void installEncoding();
void installTreeWalker();
void installAbortController();
void installStructuredClone();
void installBlob();
void installURLObject();
void installProcess();
void installOS();
void installPath();
void installReadableStream();
void installFetch();
/// Tear down per-context fetch state. Cancels in-flight requests and frees curl handles.
void uninstallFetch();
/// Settle the file:, data: and blob: fetches on this thread whose responses
/// are already built, as the task that follows the current one. Runs no JS
/// (settling only queues reaction jobs); returns how many it settled. A host
/// calls it after a microtask checkpoint so local fetches resolve in the same
/// turn rather than waiting for the next __brokit_fetch_tick pump.
int settleLocalFetches();
void installFS();
void installFSWatch();
void uninstallFSWatch();
void installChildProcess();
void installStorage();
void setStoragePath(const std::string& path);
void cleanupStorage();
void installIndexedDB();
void installIndexedDBJS();
void setIndexedDBPath(const std::string& path);
void cleanupIndexedDB();
void installWritableStream();
void installWebSocket();
void installWebSocketJS();
void uninstallWebSocket();
/// Raw TCP/UDP sockets: native __brokit_net_* bindings (nonblocking, pumped
/// by the host via __brokit_net_tick like fetch/websocket).
void installNet();
void uninstallNet();
/// Block the calling thread up to `timeoutMs` milliseconds until any socket on
/// this thread is ready for I/O, already has pending events, or wakeNetWait()
/// is called. Returns true if woken by socket readiness or wakeNetWait().
bool waitForNetEvents(double timeoutMs);
/// Wake a thread blocked in waitForNetEvents() from any thread or signal handler.
void wakeNetWait();
/// Node-compat `net` + `dgram` modules over installNet. Requires
/// installEvents (EventEmitter) to have run first.
void installNetJS();
/// RFC 6455 WebSocketServer over the `net` module. Requires installNetJS,
/// installEncoding, installBase64, and installTimers to have run first.
void installWebSocketServerJS();
/// Native HTTP/1.1 parser bindings over llhttp (__brokit_http_parser_*).
void installHttp();
/// Node-compat `http` module over installHttp + the `net` module.
void installHttpJS();
/// Native SQLite bindings over the bundled sqlite3 (__brokit_sqlite_*).
void installSqlite();
void cleanupSqlite();
/// Node-compat `sqlite` (`node:sqlite`) module over installSqlite.
void installSqliteJS();
void installEventSource();
void installFormData();
void installFetchClasses();
/// `XMLHttpRequest` in JS over the wrapped `fetch`. Requires installFetch,
/// installFetchClasses and installAbortController to have run first; the
/// events it fires are `Event` instances, so installEventTarget (or a host
/// `Event`) must be present before the first request is sent.
void installXMLHttpRequest();
void installBase64();
void installNavigator();
void installEventTarget();
void installMessageChannel();
void installBuffer();
void installCompression();
void installEvents();
void installUtil();
#ifdef BROKIT_HAS_NOISE
void installNoise();
#endif
#ifdef BROKIT_HAS_IMAGE
void installImage();
#endif

/// Add a base path for local file fetch resolution.
/// Paths are searched in overlay order (last added = checked first).
/// Call after installFetch() or installAll().
void addFetchBasePath(const std::string& path);

/// Add a base path for fs module relative path resolution.
/// Relative paths are resolved against base paths (last added = checked first).
/// Call after installFS() or installAll().
void addFsBasePath(const std::string& path);

/// Mount a virtual prefix for both fs and fetch resolution. Paths beginning
/// with "<prefix>/..." (or exactly "<prefix>") are rewritten to
/// "<absPath>/<remainder>" before any base-path lookup.
void addFsPrefixMount(const std::string& prefix, const std::string& absPath);
void addFetchPrefixMount(const std::string& prefix, const std::string& absPath);

/// Resolve a path the same way fs.* does: /<prefix>/... mounts and absolute
/// paths are returned as-is; a relative path is checked against registered fs
/// base paths (most-recently-added first) and rewritten to the first existing
/// candidate, or returned unchanged if none exist (so it then resolves
/// against the process's OS working directory).
std::string resolveAssetPath(const std::string& path);

/// Bytes behind a Blob or File value, without copying.
/// Returns false if `val` is not a Blob or File.
bool blobBytes(bronze::Value val, const uint8_t** data,
               size_t* len, std::string* type = nullptr);

/// Set the `webkitRelativePath` of a File value — what a host fills in when it
/// builds the File for an entry of a dropped directory. Returns false if
/// `file` is not a File.
bool setFileWebkitRelativePath(bronze::Value file, std::string_view path);

/// The Blob behind a `blob:` URL minted by `URL.createObjectURL`, from the
/// same registry `fetch('blob:...')` serves. Returns false for a URL that was
/// never minted or has been revoked. Call after installURLObject().
bool blobByObjectURL(const std::string& url, bronze::Value* outBlob);

/// Host task poster hook for asynchronous deliveries (e.g. FileReader).
using HostTaskPoster = std::function<void(std::function<void()>)>;
void setHostTaskPoster(HostTaskPoster poster);

// ---- Access ------------------------------------------------------------------
//
// What the program may touch outside itself is the host's decision, not
// brokit's: the host knows which app is running and what it was granted.
// brokit asks bronze's access gate (embed/embed_access.h, which the host
// installs once for every library) at the point the power is exercised
// (opening the file, spawning the process, connecting the socket, reading an
// environment variable), never only at the JS surface, because a reference
// kept from before a check would get past a check made there. With no gate,
// everything is allowed.
//
//   Read / Write  every fs entry point, sqlite's open, a `file:` fetch. The
//                 path is absolute with brokit's mounts and base paths
//                 applied; the host canonicalizes it (symlinks, `..`).
//                 Removing, renaming away, truncating or changing the
//                 metadata of a file is Write.
//   Spawn         child_process in every form: the program and its
//                 arguments as one line.
//   Net           the request URL for fetch, XMLHttpRequest, EventSource and
//                 WebSocket (http, https, ws, wss), and `tcp://host:port`,
//                 `udp://host:port`, `tcp-listen://host:port`,
//                 `udp-bind://host:port` for raw sockets and servers (an IPv6
//                 host bracketed). A UDP send asks for each datagram; an http
//                 server or WebSocketServer listens through `tcp-listen`. A
//                 redirect is asked for too. localhost is network; `file:`,
//                 `data:` and `blob:` never are.
//   EnvRead       process.env: each variable read by name, "" to list them.
//                 A refused variable reads as unset (it is not thrown).
//   EnvWrite      process.env: each variable set or deleted. Refused, the
//                 change stays in this process's own view of the
//                 environment (what process.env shows the program, and what
//                 its child processes are given), never the real one.
//
// A refusal is an Error whose `message` is exactly the gate's sentence (named
// PermissionRevoked for a grant taken back while the program ran) and whose
// `code` is EACCES for fs (with `path` the absolute path asked about), EACCES
// for spawn, and ERR_ACCESS_DENIED for net. A synchronous call throws it;
// fs.promises, fetch and the callback forms reject or call back with it.
// fs.existsSync answers false instead.
//
// What stays open under an answer is held in bronze's access ledger: a socket,
// server, WebSocket, EventSource, child process or fs watcher. When the host's
// grants change and the gate refuses it, it is closed and the program told why:
// a socket's, server's or WebSocket's 'error' (then 'close'), a child killed
// with the reason as its 'error', a watcher's 'error'.

enum class FsAccess { Read, Write };

/// The checks, for a host's own natives that read files or open connections on
/// the program's behalf: "" when allowed, else the refusal.
std::string checkFsAccess(const std::string& path, FsAccess access);
std::string checkSpawnAccess(const std::string& command);
std::string checkNetAccess(const std::string& url);

} // namespace brokit::api
