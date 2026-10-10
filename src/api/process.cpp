#include "api/api.h"
#include "api/object_builder.h"
#include "api/arg_reader.h"
#include "api/host_proxy.h"
#include "api/signals.h"
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
#include <cerrno>
#include <csignal>
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

// An Error as Node's process.kill throws it: { code, errno, syscall: 'kill' }.
[[noreturn]] Value throwKillError(const char* code, int errnoValue, const char* what) {
    std::string message = std::string(what) + " " + code;
    ev::Persistent err;
    auto ctor = ev::globalValue("Error");
    if (ctor.found && ev::isFunction(ctor.value)) {
        ev::Persistent c(ctor.value);
        ev::Persistent text(ev::fromUtf8(message));
        const Value arg = text.get();
        auto r = ev::construct(c.get(), std::span<const Value>(&arg, 1));
        if (!r.thrown) err.set(r.value);
    }
    if (!ev::isObject(err.get())) {
        err.set(ev::createObject());
        ObjectBuilder(err.get()).set("message", ev::fromUtf8(message));
    }
    ObjectBuilder e(err.get());
    e.set("code", ev::fromUtf8(code));
    e.set("errno", ev::fromDouble(double(-errnoValue)));
    e.set("syscall", ev::fromUtf8("kill"));
    ev::throwValue(e.get());
}

// process.kill(pid, signal = 'SIGTERM') -> true, as Node: the signal by name
// or number, 0 asking only whether the process exists (ESRCH when it does
// not, EPERM when it is not ours to signal). On Windows SIGINT, SIGQUIT,
// SIGTERM and SIGKILL end the process (TerminateProcess, exit code 1, as
// libuv); other signals are ENOSYS.
Value js_process_kill(Value, std::span<const Value> a) {
    if (!hasArg(a, 0) || !ev::isNumber(a[0]))
        return ev::throwTypeError("The \"pid\" argument must be of type number");
    const double pidD = ev::toDouble(a[0]);
    if (!(pidD == pidD) || pidD != double(int64_t(pidD)))
        return ev::throwTypeError("The \"pid\" argument must be an integer");
    int sig = signalNumber("SIGTERM");
    if (hasArg(a, 1) && !ev::isUndefined(a[1]) && !ev::isNull(a[1])) {
        if (ev::isNumber(a[1])) {
            sig = int(ev::toDouble(a[1]));
        } else if (ev::isString(a[1])) {
            std::string name = ev::toUtf8(a[1]);
            sig = signalNumber(name.c_str());
            if (sig < 0) return ev::throwTypeError("Unknown signal: " + name);
        } else {
            return ev::throwTypeError("The \"signal\" argument must be of type string or number");
        }
    }
#ifdef _WIN32
    constexpr int ESRCH_ = 3, EPERM_ = 1, ENOSYS_ = 40, EINVAL_ = 22;
    const DWORD pid = DWORD(int64_t(pidD));
    const bool ends = sig == 2 || sig == 3 || sig == 9 || sig == 15;
    if (sig != 0 && !ends) throwKillError("ENOSYS", ENOSYS_, "kill");
    if (pidD < 0) throwKillError("EINVAL", EINVAL_, "kill");
    const DWORD access = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | (ends ? PROCESS_TERMINATE : 0);
    HANDLE h = OpenProcess(access, FALSE, pid);
    if (!h) {
        DWORD e = GetLastError();
        if (e == ERROR_INVALID_PARAMETER) throwKillError("ESRCH", ESRCH_, "kill");
        throwKillError("EPERM", EPERM_, "kill");
    }
    // A process that has exited but whose handle someone still holds is
    // still openable: it no longer exists for kill.
    const bool exited = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    if (exited) {
        CloseHandle(h);
        throwKillError("ESRCH", ESRCH_, "kill");
    }
    if (ends && !TerminateProcess(h, 1)) {
        const bool goneMeanwhile = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
        CloseHandle(h);
        throwKillError(goneMeanwhile ? "ESRCH" : "EPERM", goneMeanwhile ? ESRCH_ : EPERM_, "kill");
    }
    CloseHandle(h);
    return ev::fromBool(true);
#else
    if (::kill(pid_t(int64_t(pidD)), sig) != 0) {
        const int e = errno;
        const char* code = e == ESRCH ? "ESRCH" : e == EPERM ? "EPERM" : e == EINVAL ? "EINVAL" : "EIO";
        throwKillError(code, e, "kill");
    }
    return ev::fromBool(true);
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
    process.def("kill", 2, js_process_kill);

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
