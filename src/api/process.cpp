#include "api/api.h"
#include "runtime/runtime.h"
#include "process.js.h"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace brokit::api {

// process.env.KEY — read an environment variable
static JSValue js_env_get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    const char* key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    const char* val = getenv(key);
    JS_FreeCString(ctx, key);

    if (!val) return JS_UNDEFINED;
    return JS_NewString(ctx, val);
}

// process.env.KEY = value — set an environment variable
static JSValue js_env_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_UNDEFINED;
    const char* key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;
    const char* val = JS_ToCString(ctx, argv[1]);
    if (!val) { JS_FreeCString(ctx, key); return JS_EXCEPTION; }

#ifdef _WIN32
    SetEnvironmentVariableA(key, val);
    // Also update CRT environ
    _putenv_s(key, val);
#else
    setenv(key, val, 1);
#endif

    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

// process.env.KEY delete — unset an environment variable
static JSValue js_env_delete(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    const char* key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

#ifdef _WIN32
    SetEnvironmentVariableA(key, nullptr);
    _putenv_s(key, "");
#else
    unsetenv(key);
#endif

    JS_FreeCString(ctx, key);
    return JS_UNDEFINED;
}

// process.cwd()
static JSValue js_process_cwd(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetCurrentDirectoryA(MAX_PATH, buf);
    if (len == 0) return JS_ThrowInternalError(ctx, "process.cwd: failed");
    return JS_NewStringLen(ctx, buf, len);
#else
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return JS_ThrowInternalError(ctx, "process.cwd: failed");
    return JS_NewString(ctx, buf);
#endif
}

// process.exit(code?)
static JSValue js_process_exit(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    int code = 0;
    if (argc > 0) JS_ToInt32(ctx, &code, argv[0]);
    exit(code);
    return JS_UNDEFINED; // unreachable
}

void installProcess(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue process = JS_NewObject(ctx);

    // Native helpers for the env Proxy
    JS_SetPropertyStr(ctx, global, "__brokit_env_get",
                      JS_NewCFunction(ctx, js_env_get, "__brokit_env_get", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_env_set",
                      JS_NewCFunction(ctx, js_env_set, "__brokit_env_set", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_env_delete",
                      JS_NewCFunction(ctx, js_env_delete, "__brokit_env_delete", 1));

    // process.cwd, process.exit
    JS_SetPropertyStr(ctx, process, "cwd",
                      JS_NewCFunction(ctx, js_process_cwd, "cwd", 0));
    JS_SetPropertyStr(ctx, process, "exit",
                      JS_NewCFunction(ctx, js_process_exit, "exit", 1));

    // process.platform
#ifdef _WIN32
    JS_SetPropertyStr(ctx, process, "platform", JS_NewString(ctx, "win32"));
#elif defined(__linux__)
    JS_SetPropertyStr(ctx, process, "platform", JS_NewString(ctx, "linux"));
#elif defined(__APPLE__)
    JS_SetPropertyStr(ctx, process, "platform", JS_NewString(ctx, "darwin"));
#else
    JS_SetPropertyStr(ctx, process, "platform", JS_NewString(ctx, "unknown"));
#endif

    // process.arch
#if defined(_M_X64) || defined(__x86_64__)
    JS_SetPropertyStr(ctx, process, "arch", JS_NewString(ctx, "x64"));
#elif defined(_M_ARM64) || defined(__aarch64__)
    JS_SetPropertyStr(ctx, process, "arch", JS_NewString(ctx, "arm64"));
#elif defined(_M_IX86) || defined(__i386__)
    JS_SetPropertyStr(ctx, process, "arch", JS_NewString(ctx, "ia32"));
#elif defined(_M_ARM) || defined(__arm__)
    JS_SetPropertyStr(ctx, process, "arch", JS_NewString(ctx, "arm"));
#else
    JS_SetPropertyStr(ctx, process, "arch", JS_NewString(ctx, "x64"));
#endif

    // process.argv — argv[0] is the "node" executable stand-in, argv[1] the "script"
    JSValue argv = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, argv, 0, JS_NewString(ctx, "bro"));
    JS_SetPropertyUint32(ctx, argv, 1, JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, process, "argv", argv);

    // process.version / process.versions
    JS_SetPropertyStr(ctx, process, "version", JS_NewString(ctx, "v20.0.0"));
    JSValue versions = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, versions, "node", JS_NewString(ctx, "20.0.0"));
    JS_SetPropertyStr(ctx, versions, "v8", JS_NewString(ctx, "0.0.0"));
    JS_SetPropertyStr(ctx, versions, "brokit", JS_NewString(ctx, "1.0.0"));
    JS_SetPropertyStr(ctx, process, "versions", versions);

    // process.pid
    JS_SetPropertyStr(ctx, process, "pid", JS_NewInt32(ctx, 1));

    // process.execPath
    JS_SetPropertyStr(ctx, process, "execPath", JS_NewString(ctx, "bro"));

    JS_SetPropertyStr(ctx, global, "process", process);
    JS_FreeValue(ctx, global);

    // Install process.env as a Proxy for dynamic property access
    const char* envProxy = R"JS(
(function() {
    var handler = {
        get: function(target, prop) {
            if (typeof prop !== 'string') return undefined;
            return globalThis.__brokit_env_get(prop);
        },
        set: function(target, prop, value) {
            globalThis.__brokit_env_set(prop, String(value));
            return true;
        },
        deleteProperty: function(target, prop) {
            globalThis.__brokit_env_delete(prop);
            return true;
        },
        has: function(target, prop) {
            return globalThis.__brokit_env_get(prop) !== undefined;
        }
    };
    process.env = new Proxy({}, handler);
})();
)JS";

    JSValue r = JS_Eval(ctx, envProxy, strlen(envProxy), "<process>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);

    // JS-layer augmentation: process.nextTick, process.hrtime, process.stdout/stderr, etc.
    JSValue r2 = JS_Eval(ctx, js_process, strlen(js_process), "<process>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r2)) {
        Runtime::checkException(ctx, r2);
    }
    JS_FreeValue(ctx, r2);
}

} // namespace brokit::api
