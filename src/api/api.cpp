#include "api/api.h"

#include <string>

namespace brokit::api {

// ---------------------------------------------------------------------------
// require() — Node-compatible module resolver
//
// Resolution order:
//   1. globalThis.__brokit_modules[name]  — the module registry. Any module
//      (native or JS-layer) can self-register here, so new Node-compat modules
//      never need to edit this function. A leading "node:" prefix is stripped.
//   2. Backward-compatible fallback to the four original built-in globals
//      (fs / path / os / child_process → their __brokit_* globals).
//   3. Otherwise: throw "Cannot find module".
// ---------------------------------------------------------------------------

static JSValue js_require(JSContext* ctx, JSValueConst /*this_val*/,
                          int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "require() expects a module name string");
    }

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    std::string mod(name);
    JS_FreeCString(ctx, name);

    // Strip an optional "node:" prefix for lookup.
    std::string bare = mod;
    if (bare.rfind("node:", 0) == 0) bare = bare.substr(5);

    JSValue global = JS_GetGlobalObject(ctx);

    // 1) Registry lookup — globalThis.__brokit_modules[bare]
    JSValue registry = JS_GetPropertyStr(ctx, global, "__brokit_modules");
    if (JS_IsObject(registry)) {
        JSValue m = JS_GetPropertyStr(ctx, registry, bare.c_str());
        if (!JS_IsUndefined(m)) {
            JS_FreeValue(ctx, registry);
            JS_FreeValue(ctx, global);
            return m;
        }
        JS_FreeValue(ctx, m);
    }
    JS_FreeValue(ctx, registry);

    // 2) Backward-compatible fallback for the original built-in globals.
    const char* globalKey = nullptr;
    if (bare == "fs") globalKey = "__brokit_fs";
    else if (bare == "path") globalKey = "__brokit_path";
    else if (bare == "os") globalKey = "__brokit_os";
    else if (bare == "child_process") globalKey = "__brokit_child_process";

    if (globalKey) {
        JSValue result = JS_GetPropertyStr(ctx, global, globalKey);
        JS_FreeValue(ctx, global);
        if (JS_IsUndefined(result)) {
            JS_FreeValue(ctx, result);
            return JS_ThrowReferenceError(ctx, "Module '%s' is not installed", mod.c_str());
        }
        return result;
    }

    JS_FreeValue(ctx, global);
    return JS_ThrowReferenceError(ctx, "Cannot find module '%s'", mod.c_str());
}

// Create globalThis.__brokit_modules early so modules can self-register into it
// as they install. Idempotent — never clobbers an existing registry.
static void installModuleRegistry(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue existing = JS_GetPropertyStr(ctx, global, "__brokit_modules");
    if (!JS_IsObject(existing)) {
        JS_SetPropertyStr(ctx, global, "__brokit_modules", JS_NewObject(ctx));
    }
    JS_FreeValue(ctx, existing);
    JS_FreeValue(ctx, global);
}

static void installRequire(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "require",
        JS_NewCFunction(ctx, js_require, "require", 1));
    JS_FreeValue(ctx, global);
}

// ---------------------------------------------------------------------------

void installAll(JSContext* ctx)
{
    installModuleRegistry(ctx);
    installConsole(ctx);
    installTimers(ctx);
    installURL(ctx);
    installCrypto(ctx);
    installSubtleCrypto(ctx);
    installEncoding(ctx);
    installTreeWalker(ctx);
    installAbortController(ctx);
    installStructuredClone(ctx);
    installBlob(ctx);
    installURLObject(ctx);
    installProcess(ctx);
    installOS(ctx);
    installPath(ctx);
    installStorage(ctx);
    installIndexedDB(ctx);
    installIndexedDBJS(ctx);
    installReadableStream(ctx);
    installFetch(ctx);
    installWritableStream(ctx);
    installFS(ctx);
    installFSWatch(ctx);
    installChildProcess(ctx);
    installWebSocket(ctx);
    installWebSocketJS(ctx);
    installEventSource(ctx);
    installFormData(ctx);
    installFetchClasses(ctx);
    installBase64(ctx);
    installNavigator(ctx);
    installEventTarget(ctx);
    installMessageChannel(ctx);
#ifdef BROKIT_HAS_NOISE
    installNoise(ctx);
#endif
#ifdef BROKIT_HAS_IMAGE
    installImage(ctx);
#endif

    // Node-compat modules. installBuffer must run after installEncoding and
    // installBase64 (buffer.js uses TextEncoder + atob/btoa at eval time),
    // which is satisfied by placing these at the end, before installRequire.
    installEvents(ctx);
    installUtil(ctx);
    installBuffer(ctx);

    // require() must come last — after all modules are installed
    installRequire(ctx);
}

} // namespace brokit::api
