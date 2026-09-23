#include "api/api.h"
#include "runtime/runtime.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <span>

namespace brokit::api {

// ---------------------------------------------------------------------------
// require() — Node-compatible module resolver
// ---------------------------------------------------------------------------

static thread_local std::vector<std::filesystem::path> g_requireDirStack;

void pushRequireDir(const std::filesystem::path& dir)
{
    g_requireDirStack.push_back(dir);
}

void popRequireDir()
{
    if (!g_requireDirStack.empty()) {
        g_requireDirStack.pop_back();
    }
}

static std::filesystem::path caller_dir()
{
    namespace fs = std::filesystem;
    if (!g_requireDirStack.empty()) {
        return g_requireDirStack.back();
    }
    std::error_code ec;
    return fs::current_path(ec);
}

static bool is_path_spec(const std::string& s)
{
    if (s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0) return true;
    if (s.rfind(".\\", 0) == 0 || s.rfind("..\\", 0) == 0) return true;
    if (!s.empty() && (s[0] == '/' || s[0] == '\\')) return true;
    if (s.size() > 2 && s[1] == ':' && (s[2] == '/' || s[2] == '\\')) return true;
    return false;
}

static bronze::Value load_file_module(const std::string& spec)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path base = caller_dir();
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
        return ev::throwError("Cannot find module '" + spec + "'");
    }
    found = fs::weakly_canonical(found, ec);
    const std::string file = found.string();
    const std::string dir = found.parent_path().string();

    ev::Persistent cache{ev::getGlobal("__brokit_module_cache")};
    if (!ev::isObject(cache.get())) {
        cache.set(ev::createObject());
        ev::setGlobalValue("__brokit_module_cache", cache.get());
    }

    bronze::Value hit = ev::getProperty(cache.get(), file);
    if (!ev::isUndefined(hit)) {
        return hit;
    }

    std::ifstream in(found, std::ios::binary);
    if (!in) {
        return ev::throwError("Cannot read module '" + file + "'");
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string src = buf.str();

    if (found.extension() == ".json") {
        auto res = ev::parseJson(src);
        if (!res.thrown) {
            ev::Persistent parsed{res.value};
            cache.set(ev::setProperty(cache.get(), file, parsed.get()));
            return parsed.get();
        }
        return ev::throwError("SyntaxError: failed to parse JSON module " + file);
    }

    // For .js files, execute in CommonJS envelope
    {
        ev::Persistent exportsObj{ev::createObject()};
        cache.set(ev::setProperty(cache.get(), file, exportsObj.get()));
    }

    RequireDirGuard guard{fs::path(dir)};

    std::string escapedFile;
    for (char c : file) {
        if (c == '\\') escapedFile += "\\\\";
        else if (c == '"') escapedFile += "\\\"";
        else escapedFile += c;
    }
    std::string escapedDir;
    for (char c : dir) {
        if (c == '\\') escapedDir += "\\\\";
        else if (c == '"') escapedDir += "\\\"";
        else escapedDir += c;
    }

    std::string wrapped =
        "(function() {\n"
        "  var __file = \"" + escapedFile + "\";\n"
        "  var __dir = \"" + escapedDir + "\";\n"
        "  var module = { exports: globalThis.__brokit_module_cache[__file] || {} };\n"
        "  var exports = module.exports;\n"
        "  var __filename = __file;\n"
        "  var __dirname = __dir;\n"
        "  (function(exports, require, module, __filename, __dirname) {\n"
        + src + "\n"
        "  })(exports, globalThis.require, module, __filename, __dirname);\n"
        "  globalThis.__brokit_module_cache[__file] = module.exports;\n"
        "  globalThis.__brokit_module_last_exports = module.exports;\n"
        "})();\n";

    Runtime rt;
    bool ok = rt.eval(wrapped, file);
    bronze::Value cacheAfter = ev::getGlobal("__brokit_module_cache");
    if (!ok) {
        if (ev::isObject(cacheAfter)) ev::setProperty(cacheAfter, file, ev::undefined());
        return ev::throwError("Failed to evaluate module '" + file + "'");
    }

    bronze::Value finalExports = ev::isObject(cacheAfter) ? ev::getProperty(cacheAfter, file) : ev::undefined();
    return finalExports;
}

static bronze::Value js_require(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty() || !ev::isString(args[0])) {
        return ev::throwTypeError("require() expects a module name string");
    }

    std::string mod = ev::toUtf8(args[0]);

    if (is_path_spec(mod)) return load_file_module(mod);

    // Strip an optional "node:" prefix for lookup.
    std::string bare = mod;
    if (bare.rfind("node:", 0) == 0) bare = bare.substr(5);

    // 1) Registry lookup — globalThis.__brokit_modules[bare]
    bronze::Value registry = ev::getGlobal("__brokit_modules");
    if (ev::isObject(registry)) {
        bronze::Value m = ev::getProperty(registry, bare);
        if (!ev::isUndefined(m)) {
            return m;
        }
    }

    // 2) Backward-compatible fallback for the original built-in globals.
    const char* globalKey = nullptr;
    if (bare == "fs") globalKey = "__brokit_fs";
    else if (bare == "path") globalKey = "__brokit_path";
    else if (bare == "os") globalKey = "__brokit_os";
    else if (bare == "child_process") globalKey = "__brokit_child_process";
    else if (bare == "crypto") globalKey = "crypto";

    if (globalKey) {
        bronze::Value result = ev::getGlobal(globalKey);
        if (!ev::isUndefined(result)) {
            return result;
        }
        return ev::throwError("Module '" + mod + "' is not installed");
    }

    return ev::throwError("Cannot find module '" + mod + "'");
}

void installModuleRegistry()
{
    bronze::Value existing = ev::getGlobal("__brokit_modules");
    if (!ev::isObject(existing)) {
        ev::setGlobalValue("__brokit_modules", ev::createObject());
    }
}

void installRequire()
{
    ev::registerFunction("require", js_require);
}

// ---------------------------------------------------------------------------

void installAll()
{
    installModuleRegistry();
    installConsole();
    installTimers();
    installURL();
    installCrypto();
    installSubtleCrypto();
    installEncoding();
    installTreeWalker();
    installAbortController();
    installStructuredClone();
    installBlob();
    installURLObject();
    installProcess();
    installOS();
    installPath();
    installStorage();
    installIndexedDB();
    installIndexedDBJS();
    installReadableStream();
    installFetch();
    installWritableStream();
    installFS();
    installFSWatch();
    installChildProcess();
    installWebSocket();
    installWebSocketJS();
    installEventTarget();
    installEventSource();
    installFormData();
    installFetchClasses();
    installXMLHttpRequest();
    installCompression();
    installBase64();
    installNavigator();
    installMessageChannel();
#ifdef BROKIT_HAS_NOISE
    installNoise();
#endif
#ifdef BROKIT_HAS_IMAGE
    installImage();
#endif

    // Node-compat modules. installBuffer must run after installEncoding and
    // installBase64 (buffer.js uses TextEncoder + atob/btoa at eval time),
    // which is satisfied by placing these at the end, before installRequire.
    installEvents();
    installUtil();
    installBuffer();

    // Raw sockets + WebSocket server. net.js/dgram.js extend EventEmitter and
    // prefer Buffer for delivered chunks, so these follow the Node-compat
    // block; websocket_server.js requires the net module in turn.
    installNet();
    installNetJS();
    installWebSocketServerJS();

    // require() must come last — after all modules are installed
    installRequire();
}

} // namespace brokit::api
