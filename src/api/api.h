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
void installFS(JSContext* ctx);
void installChildProcess(JSContext* ctx);

/// Add a base path for local file fetch resolution.
/// Paths are searched in overlay order (last added = checked first).
/// Call after installFetch() or installAll().
void addFetchBasePath(JSContext* ctx, const std::string& path);

} // namespace brokit::api
