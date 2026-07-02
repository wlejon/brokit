#pragma once

#include <string>

extern "C" {
#include "quickjs.h"
}

namespace brokit::api {

/// Install all brokit APIs on the given context.
/// Call this once after creating the Runtime.
void installAll(JSContext* ctx);

// Individual API installers — consumers can pick and choose.
void installConsole(JSContext* ctx);
void installTimers(JSContext* ctx);
void installURL(JSContext* ctx);
void installCrypto(JSContext* ctx);
void installSubtleCrypto(JSContext* ctx);
void installEncoding(JSContext* ctx);
void installTreeWalker(JSContext* ctx);
void installAbortController(JSContext* ctx);
void installStructuredClone(JSContext* ctx);
void installBlob(JSContext* ctx);
void installURLObject(JSContext* ctx);
void installProcess(JSContext* ctx);
void installOS(JSContext* ctx);
void installPath(JSContext* ctx);
void installReadableStream(JSContext* ctx);
void installFetch(JSContext* ctx);
/// Tear down per-context fetch state. Cancels in-flight requests, frees curl
/// handles, and releases owned JSValues. Call before JS_FreeContext if the
/// runtime is being destroyed while fetches may still be pending.
void uninstallFetch(JSContext* ctx);
void installFS(JSContext* ctx);
void installFSWatch(JSContext* ctx);
void installChildProcess(JSContext* ctx);
void installStorage(JSContext* ctx);
void setStoragePath(JSContext* ctx, const std::string& path);
void cleanupStorage(JSContext* ctx);
void installIndexedDB(JSContext* ctx);
void installIndexedDBJS(JSContext* ctx);
void setIndexedDBPath(JSContext* ctx, const std::string& path);
void cleanupIndexedDB(JSContext* ctx);
void installWritableStream(JSContext* ctx);
void installWebSocket(JSContext* ctx);
void installWebSocketJS(JSContext* ctx);
void installEventSource(JSContext* ctx);
void installFormData(JSContext* ctx);
void installFetchClasses(JSContext* ctx);
void installBase64(JSContext* ctx);
void installNavigator(JSContext* ctx);
void installEventTarget(JSContext* ctx);
void installMessageChannel(JSContext* ctx);
#ifdef BROKIT_HAS_NOISE
void installNoise(JSContext* ctx);
#endif
#ifdef BROKIT_HAS_IMAGE
void installImage(JSContext* ctx);
#endif

/// Add a base path for local file fetch resolution.
/// Paths are searched in overlay order (last added = checked first).
/// Call after installFetch() or installAll().
void addFetchBasePath(JSContext* ctx, const std::string& path);

/// Add a base path for fs module relative path resolution.
/// Relative paths are resolved against base paths (last added = checked first).
/// Call after installFS() or installAll().
void addFsBasePath(JSContext* ctx, const std::string& path);

/// Mount a virtual prefix for both fs and fetch resolution. Paths beginning
/// with "<prefix>/..." (or exactly "<prefix>") are rewritten to
/// "<absPath>/<remainder>" before any base-path lookup.
///
/// Mount prefixes always start with `/` and have no trailing slash. The
/// rewritten absolute path takes precedence over both base paths and the
/// filesystem-absolute interpretation, so `/lib/foo.js` resolves through
/// the mount even when `/lib/foo.js` happens to exist on disk.
///
/// Call after installFS() / installFetch() / installAll().
void addFsPrefixMount(JSContext* ctx, const std::string& prefix, const std::string& absPath);
void addFetchPrefixMount(JSContext* ctx, const std::string& prefix, const std::string& absPath);

/// Resolve a path the same way fs.* does: /<prefix>/... mounts and absolute
/// paths are returned as-is; a relative path is checked against registered fs
/// base paths (most-recently-added first) and rewritten to the first existing
/// candidate, or returned unchanged if none exist (so it then resolves
/// against the process's OS working directory).
///
/// For native bindings that take a path/dir argument straight from a JS
/// string and hand it to C++ file I/O without going through fs.* (e.g. model
/// loaders like bro.tts.loadKokoro) — resolve through this at the JS call
/// boundary so their behavior matches fs.existsSync() regardless of which
/// directory the process was actually launched from.
std::string resolveAssetPath(JSContext* ctx, const std::string& path);

} // namespace brokit::api
