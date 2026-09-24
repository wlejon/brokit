#include "api/api.h"
#include "api/object_builder.h"
#include "api/arg_reader.h"
#include "api/host_proxy.h"
#include "runtime/runtime.h"
#include "embed/embed.h"

extern "C" void bronze_process_main();

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
extern "C" char** environ;
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif
#include <string>

namespace brokit::api {

namespace {

// The running executable's absolute path (Node's process.execPath), so an
// app can start another instance of its own runtime.
std::string executablePath() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return (len > 0 && len < MAX_PATH) ? std::string(buf, len) : std::string("bro");
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    return _NSGetExecutablePath(buf, &size) == 0 ? std::string(buf) : std::string("bro");
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return len > 0 ? std::string(buf, static_cast<size_t>(len)) : std::string("bro");
#endif
}

Value js_process_cwd(Value, std::span<const Value>) {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetCurrentDirectoryA(MAX_PATH, buf);
    if (len == 0) return ev::throwError("process.cwd: failed");
    return ev::fromUtf8(buf);
#else
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return ev::throwError("process.cwd: failed");
    return ev::fromUtf8(buf);
#endif
}

Value js_process_exit(Value, std::span<const Value> a) {
    int code = 0;
    if (!a.empty()) code = i32At(a, 0);
    exit(code);
    return ev::undefined();
}

Value makeEnvProxy() {
    HostProxyTraps traps;
    traps.get = [](const std::string& key, Value& out) -> bool {
        const char* val = getenv(key.c_str());
        if (!val) return false;
        out = ev::fromUtf8(val);
        return true;
    };
    traps.set = [](const std::string& key, Value v) {
        std::string val = ev::toUtf8(v);
#ifdef _WIN32
        SetEnvironmentVariableA(key.c_str(), val.c_str());
        _putenv_s(key.c_str(), val.c_str());
#else
        setenv(key.c_str(), val.c_str(), 1);
#endif
    };
    traps.remove = [](const std::string& key) {
#ifdef _WIN32
        SetEnvironmentVariableA(key.c_str(), nullptr);
        _putenv_s(key.c_str(), "");
#else
        unsetenv(key.c_str());
#endif
    };
    traps.has = [](const std::string& key) -> bool {
        return getenv(key.c_str()) != nullptr;
    };
    traps.ownKeys = []() -> std::vector<std::string> {
        std::vector<std::string> keys;
#ifdef _WIN32
        char* block = GetEnvironmentStringsA();
        if (block) {
            for (char* p = block; *p; p += strlen(p) + 1) {
                if (p[0] == '=') continue;
                const char* eq = strchr(p, '=');
                if (!eq) continue;
                keys.emplace_back(p, static_cast<size_t>(eq - p));
            }
            FreeEnvironmentStringsA(block);
        }
#else
        for (char** e = environ; e && *e; e++) {
            const char* eq = strchr(*e, '=');
            if (!eq) continue;
            keys.emplace_back(*e, static_cast<size_t>(eq - *e));
        }
#endif
        return keys;
    };
    return makeHostProxy(std::move(traps));
}

} // namespace

void installProcess() {
    ObjectBuilder process;

    process.set("env", makeEnvProxy());
    process.def("cwd", 0, js_process_cwd);
    process.def("exit", 1, js_process_exit);

#ifdef _WIN32
    process.set("platform", ev::fromUtf8("win32"));
#elif defined(__linux__)
    process.set("platform", ev::fromUtf8("linux"));
#elif defined(__APPLE__)
    process.set("platform", ev::fromUtf8("darwin"));
#else
    process.set("platform", ev::fromUtf8("unknown"));
#endif

#if defined(_M_X64) || defined(__x86_64__)
    process.set("arch", ev::fromUtf8("x64"));
#elif defined(_M_ARM64) || defined(__aarch64__)
    process.set("arch", ev::fromUtf8("arm64"));
#elif defined(_M_IX86) || defined(__i386__)
    process.set("arch", ev::fromUtf8("ia32"));
#elif defined(_M_ARM) || defined(__arm__)
    process.set("arch", ev::fromUtf8("arm"));
#else
    process.set("arch", ev::fromUtf8("x64"));
#endif

    process.set("argv", hostArrayOf(2, [](size_t i) {
        return i == 0 ? ev::fromUtf8("bro") : ev::fromUtf8("");
    }));

    process.set("version", ev::fromUtf8("v20.0.0"));
    {
        ObjectBuilder versions;
        versions.set("node", ev::fromUtf8("20.0.0"));
        versions.set("v8", ev::fromUtf8("0.0.0"));
        versions.set("brokit", ev::fromUtf8("1.0.0"));
        process.set("versions", versions.get());
    }

#ifdef _WIN32
    process.set("pid", ev::fromDouble(static_cast<double>(GetCurrentProcessId())));
#else
    process.set("pid", ev::fromDouble(static_cast<double>(getpid())));
#endif
    process.set("execPath", ev::fromUtf8(executablePath()));

    process.def("nextTick", 1, [](Value, std::span<const Value> a) {
        if (a.empty() || !ev::isFunction(a[0])) return ev::undefined();
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        ev::Persistent thenFn{ev::getProperty(p.get(), "then")};
        if (ev::isFunction(thenFn.get())) {
            // a[0] is read from the rooted args span, never a stale copy.
            ev::call(thenFn.get(), p.get(), a.subspan(0, 1));
        }
        return ev::undefined();
    });

    process.def("hrtime", 0, [](Value, std::span<const Value> a) {
        static auto start = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto totalNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
        if (a.size() > 0 && ev::isObject(a[0])) {
            double prevSec = ev::toDouble(ev::getElement(a[0], 0));
            double prevNs = ev::toDouble(ev::getElement(a[0], 1));
            int64_t prevTotal = static_cast<int64_t>(prevSec) * 1000000000LL + static_cast<int64_t>(prevNs);
            int64_t diff = totalNs - prevTotal;
            return hostArrayOf(2, [diff](size_t i) {
                if (i == 0) return ev::fromDouble(static_cast<double>(diff / 1000000000LL));
                return ev::fromDouble(static_cast<double>(diff % 1000000000LL));
            });
        }
        return hostArrayOf(2, [totalNs](size_t i) {
            if (i == 0) return ev::fromDouble(static_cast<double>(totalNs / 1000000000LL));
            return ev::fromDouble(static_cast<double>(totalNs % 1000000000LL));
        });
    });

    {
        ObjectBuilder stdoutObj;
        stdoutObj.def("write", 1, [](Value, std::span<const Value> a) {
            if (!a.empty()) {
                std::string s = ev::toUtf8(a[0]);
                std::fwrite(s.data(), 1, s.size(), stdout);
                std::fflush(stdout);
            }
            return ev::fromBool(true);
        });
        process.set("stdout", stdoutObj.get());
    }

    {
        ObjectBuilder stderrObj;
        stderrObj.def("write", 1, [](Value, std::span<const Value> a) {
            if (!a.empty()) {
                std::string s = ev::toUtf8(a[0]);
                std::fwrite(s.data(), 1, s.size(), stderr);
                std::fflush(stderr);
            }
            return ev::fromBool(true);
        });
        process.set("stderr", stderrObj.get());
    }



    ev::registerGlobal("process", process.get());
    auto g = ev::globalValue("globalThis");
    if (g.found && ev::isObject(g.value)) {
        ev::setProperty(g.value, "process", process.get());
    }
    bronze::embed::runEntry(bronze_process_main);
}

} // namespace brokit::api
