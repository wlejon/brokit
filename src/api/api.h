#pragma once

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

} // namespace brokit::api
