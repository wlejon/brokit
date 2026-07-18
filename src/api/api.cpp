#include "api/api.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace brokit::api {

// ---------------------------------------------------------------------------
// require() — Node-compatible module resolver
//
// Resolution order:
//   1. globalThis.__brokit_modules[name]  — the module registry. Any module
//      (native or JS-layer) can self-register here, so new Node-compat modules
//      never need to edit this function. A leading "node:" prefix is stripped.
//   2. A PATH specifier ("./x", "../x/y", "/abs/x", "C:/abs/x") — a JS or JSON
//      file on disk, loaded and cached (see load_file_module below).
//   3. Backward-compatible fallback to the four original built-in globals
//      (fs / path / os / child_process → their __brokit_* globals).
//   4. Otherwise: throw "Cannot find module".
// ---------------------------------------------------------------------------

// The directory a relative require() resolves against: the directory of the file
// that CALLED require, exactly as Node does it. QuickJS knows the filename of the
// calling frame's script, and every path we evaluate a module under is its own
// absolute path — so this works for a module requiring its sibling and for the
// entry script alike, with no cooperation from the host. Falls back to the process
// working directory when the caller has no meaningful filename (an -e expression,
// a REPL line).
static std::filesystem::path caller_dir(JSContext* ctx)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    JSAtom a = JS_GetScriptOrModuleName(ctx, 1);
    if (a != JS_ATOM_NULL) {
        const char* s = JS_AtomToCString(ctx, a);
        JS_FreeAtom(ctx, a);
        if (s) {
            fs::path p(s);
            JS_FreeCString(ctx, s);
            fs::path dir = p.parent_path();
            if (!dir.empty() && fs::is_directory(dir, ec)) return dir;
        }
    }
    return fs::current_path(ec);
}

// Does this specifier name a file rather than a module? Bare names ("fs",
// "lodash") stay with the registry; anything that looks like a path goes to disk.
static bool is_path_spec(const std::string& s)
{
    if (s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0) return true;
    if (s.rfind(".\\", 0) == 0 || s.rfind("..\\", 0) == 0) return true;
    if (!s.empty() && (s[0] == '/' || s[0] == '\\')) return true;
    // Windows drive-absolute: "D:/x", "C:\x".
    if (s.size() > 2 && s[1] == ':' && (s[2] == '/' || s[2] == '\\')) return true;
    return false;
}

// Load "./foo", "./foo.js", "./foo.json" or "./foo/index.js" off disk.
//
// The module is wrapped in the Node function envelope — (exports, require,
// module, __filename, __dirname) — so module code sees the identifiers it
// expects, and both `exports.x = ...` and `module.exports = ...` work. The
// wrapper prefix carries no newline, so reported line numbers still match the
// file.
//
// Cached by resolved absolute path in globalThis.__brokit_module_cache: a module
// evaluates once, and two requires of the same file share one instance. The
// cache entry is written BEFORE evaluation so that an import cycle sees a
// partially-filled exports object instead of recursing forever.
static JSValue load_file_module(JSContext* ctx, const std::string& spec)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path base = caller_dir(ctx);
    fs::path p(spec);
    if (!p.is_absolute()) p = base / p;
    p = p.lexically_normal();

    const std::string s = p.string();
    fs::path found;
    for (const std::string& cand : {s, s + ".js", s + ".json",
                                    (p / "index.js").string()}) {
        if (fs::is_regular_file(cand, ec)) { found = cand; break; }
    }
    if (found.empty()) {
        return JS_ThrowReferenceError(ctx, "Cannot find module '%s'", spec.c_str());
    }
    found = fs::weakly_canonical(found, ec);
    const std::string file = found.string();
    const std::string dir = found.parent_path().string();

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue cache = JS_GetPropertyStr(ctx, global, "__brokit_module_cache");
    if (!JS_IsObject(cache)) {
        JS_FreeValue(ctx, cache);
        cache = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__brokit_module_cache",
                          JS_DupValue(ctx, cache));
    }

    JSValue hit = JS_GetPropertyStr(ctx, cache, file.c_str());
    if (!JS_IsUndefined(hit)) {
        JS_FreeValue(ctx, cache);
        JS_FreeValue(ctx, global);
        return hit;
    }
    JS_FreeValue(ctx, hit);

    std::ifstream in(found, std::ios::binary);
    if (!in) {
        JS_FreeValue(ctx, cache);
        JS_FreeValue(ctx, global);
        return JS_ThrowReferenceError(ctx, "Cannot read module '%s'", file.c_str());
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string src = buf.str();

    if (found.extension() == ".json") {
        JSValue json = JS_ParseJSON(ctx, src.c_str(), src.size(), file.c_str());
        if (!JS_IsException(json))
            JS_SetPropertyStr(ctx, cache, file.c_str(), JS_DupValue(ctx, json));
        JS_FreeValue(ctx, cache);
        JS_FreeValue(ctx, global);
        return json;
    }

    JSValue module = JS_NewObject(ctx);
    JSValue exports = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, module, "exports", JS_DupValue(ctx, exports));
    JS_SetPropertyStr(ctx, cache, file.c_str(), JS_DupValue(ctx, exports));

    const std::string wrapped =
        "(function(exports, require, module, __filename, __dirname){" + src + "\n})";
    JSValue fn = JS_Eval(ctx, wrapped.c_str(), wrapped.size(), file.c_str(),
                         JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        JS_DeleteProperty(ctx, cache, JS_NewAtom(ctx, file.c_str()), 0);
        JS_FreeValue(ctx, exports);
        JS_FreeValue(ctx, module);
        JS_FreeValue(ctx, cache);
        JS_FreeValue(ctx, global);
        return fn;
    }

    JSValue req = JS_GetPropertyStr(ctx, global, "require");
    JSValue argv2[5] = { JS_DupValue(ctx, exports), req, JS_DupValue(ctx, module),
                         JS_NewString(ctx, file.c_str()),
                         JS_NewString(ctx, dir.c_str()) };

    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 5, argv2);

    for (JSValue v : argv2) JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, fn);

    if (JS_IsException(ret)) {
        JS_DeleteProperty(ctx, cache, JS_NewAtom(ctx, file.c_str()), 0);
        JS_FreeValue(ctx, exports);
        JS_FreeValue(ctx, module);
        JS_FreeValue(ctx, cache);
        JS_FreeValue(ctx, global);
        return ret;
    }
    JS_FreeValue(ctx, ret);

    // The module may have REPLACED module.exports wholesale; that value, not the
    // object we seeded, is what the cache and the caller must see.
    JSValue final_exports = JS_GetPropertyStr(ctx, module, "exports");
    JS_SetPropertyStr(ctx, cache, file.c_str(), JS_DupValue(ctx, final_exports));

    JS_FreeValue(ctx, exports);
    JS_FreeValue(ctx, module);
    JS_FreeValue(ctx, cache);
    JS_FreeValue(ctx, global);
    return final_exports;
}

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

    if (is_path_spec(mod)) return load_file_module(ctx, mod);

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
    installCompression(ctx);
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

    // Raw sockets. net.js extends EventEmitter and prefers Buffer for
    // delivered chunks, so it follows the Node-compat block.
    installNet(ctx);
    installNetJS(ctx);

    // require() must come last — after all modules are installed
    installRequire(ctx);
}

} // namespace brokit::api
