#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "embed/embed.h"

namespace bronze::embed {
inline bool isDouble(Value v) { return isNumber(v); }
inline void setGlobalValue(std::string_view name, Value val) {
    registerGlobal(name, val);
    auto g = globalValue("globalThis");
    if (g.found && isObject(g.value)) setProperty(g.value, name, val);
}
inline void setGlobalFunction(std::string_view name, uint32_t arity, NativeFn fn) {
    auto f = makeFunction(std::move(fn), arity, name);
    registerGlobal(name, f);
    auto g = globalValue("globalThis");
    if (g.found && isObject(g.value)) setProperty(g.value, name, f);
}
inline void registerFunction(std::string_view name, NativeFn fn, uint32_t arity = 0) {
    auto f = makeFunction(std::move(fn), arity, name);
    registerGlobal(name, f);
    auto g = globalValue("globalThis");
    if (g.found && isObject(g.value)) setProperty(g.value, name, f);
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
void installFS();
void installFSWatch();
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
/// Raw TCP/UDP sockets: native __brokit_net_* bindings (nonblocking, pumped
/// by the host via __brokit_net_tick like fetch/websocket).
void installNet();
/// Node-compat `net` + `dgram` modules over installNet. Requires
/// installEvents (EventEmitter) to have run first.
void installNetJS();
/// RFC 6455 WebSocketServer over the `net` module. Requires installNetJS,
/// installEncoding, installBase64, and installTimers to have run first.
void installWebSocketServerJS();
void installEventSource();
void installFormData();
void installFetchClasses();
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

} // namespace brokit::api
