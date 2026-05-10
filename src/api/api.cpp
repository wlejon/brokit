#include "api/api.h"

#include <string>

namespace brokit::api {

// ---------------------------------------------------------------------------
// require() — Node-compatible module resolver
// Maps standard module names to their __brokit_* globals.
// ---------------------------------------------------------------------------

static JSValue js_require(JSContext* ctx, JSValueConst /*this_val*/,
                          int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "require() expects a module name string");
    }

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    // Map module name to its __brokit_* global
    std::string globalKey;
    std::string mod(name);
    JS_FreeCString(ctx, name);

    if (mod == "fs" || mod == "node:fs") {
        globalKey = "__brokit_fs";
    } else if (mod == "path" || mod == "node:path") {
        globalKey = "__brokit_path";
    } else if (mod == "os" || mod == "node:os") {
        globalKey = "__brokit_os";
    } else if (mod == "child_process" || mod == "node:child_process") {
        globalKey = "__brokit_child_process";
    } else {
        return JS_ThrowReferenceError(ctx, "Cannot find module '%s'", mod.c_str());
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue result = JS_GetPropertyStr(ctx, global, globalKey.c_str());
    JS_FreeValue(ctx, global);

    if (JS_IsUndefined(result)) {
        JS_FreeValue(ctx, result);
        return JS_ThrowReferenceError(ctx, "Module '%s' is not installed", mod.c_str());
    }
    return result;
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

    // require() must come last — after all modules are installed
    installRequire(ctx);
}

} // namespace brokit::api
