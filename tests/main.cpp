#include "runtime/runtime.h"
#include "api/api.h"
#include "api/object_builder.h"
#include "embed/embed_access.h"
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

struct TestResult {
    std::string name;
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;
};

// The most recent console/runtime log line, readable from a test through
// __test_lastLog() so console formatting can be asserted.
static std::string g_lastLog;

// ---- A test access policy ----------------------------------------------------
//
// __test_setAccessPolicy({ fsDenyUnder: [dirs], fsReadOnlyUnder: [dirs],
// spawn: true, net: true | [substrings] }) installs a policy refusing what it
// names (relative dirs are taken against the working directory);
// __test_clearAccessPolicy() removes it, and the harness removes it after
// every file. __test_accessLog() lists what the hooks were asked, as
// "fs:read:<path>", "fs:write:<path>", "spawn:<line>", "net:<url>",
// "env:read:<name>" or "env:write:<name>", since the policy was set. The
// policy is bronze's access gate (embed/embed_access.h), as a host installs
// it. envHide: [names] refuses those variables ([] with anything in it also
// refuses listing them all); revoked: true makes every refusal a revoked one.
// __test_accessChanged() is a host's grant change: it revalidates what this
// thread holds open and returns how many were closed.

namespace {

struct TestPolicy {
    std::vector<std::string> fsDenyUnder, fsReadOnlyUnder, netDeny, envHide;
    bool spawnDeny = false;
    bool netDenyAll = false;
    bool revoked = false;   // refusals say the grant was taken back
};

std::mutex g_accessLogMu;
std::vector<std::string> g_accessLog;

void logAccess(std::string entry) {
    std::lock_guard lock(g_accessLogMu);
    g_accessLog.push_back(std::move(entry));
}

// A comparable form of an absolute path: generic separators, and on Windows
// one case, as the filesystem has.
std::string comparablePath(const std::string& p) {
    std::u8string u = fs::absolute(fs::path(std::u8string(p.begin(), p.end()))).lexically_normal().generic_u8string();
    std::string s(u.begin(), u.end());
#ifdef _WIN32
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

bool isUnder(const std::string& path, const std::vector<std::string>& dirs) {
    const std::string p = comparablePath(path);
    for (const auto& d : dirs) {
        if (p.compare(0, d.size(), d) == 0 && (p.size() == d.size() || p[d.size()] == '/')) return true;
    }
    return false;
}

std::vector<std::string> stringList(bronze::Value obj, const char* name) {
    namespace ev = bronze::embed;
    std::vector<std::string> out;
    ev::Persistent arr{ev::getProperty(obj, name)};
    if (!ev::isObject(arr.get())) return out;
    const uint32_t len = static_cast<uint32_t>(ev::toDouble(ev::getProperty(arr.get(), "length")));
    for (uint32_t i = 0; i < len; ++i) {
        bronze::Value v = ev::getElement(arr.get(), i);
        if (ev::isString(v)) out.push_back(ev::toUtf8(v));
    }
    return out;
}

bronze::Value js_setAccessPolicy(bronze::Value, std::span<const bronze::Value> a) {
    namespace ev = bronze::embed;
    auto cfg = std::make_shared<TestPolicy>();
    if (!a.empty() && ev::isObject(a[0])) {
        for (const auto& d : stringList(a[0], "fsDenyUnder")) cfg->fsDenyUnder.push_back(comparablePath(d));
        for (const auto& d : stringList(a[0], "fsReadOnlyUnder")) cfg->fsReadOnlyUnder.push_back(comparablePath(d));
        bronze::Value sp = ev::getProperty(a[0], "spawn");
        cfg->spawnDeny = ev::isBool(sp) && ev::toBool(sp);
        bronze::Value net = ev::getProperty(a[0], "net");
        if (ev::isBool(net)) cfg->netDenyAll = ev::toBool(net);
        else cfg->netDeny = stringList(a[0], "net");
        cfg->envHide = stringList(a[0], "envHide");
        bronze::Value rv = ev::getProperty(a[0], "revoked");
        cfg->revoked = ev::isBool(rv) && ev::toBool(rv);
    }
    {
        std::lock_guard lock(g_accessLogMu);
        g_accessLog.clear();
    }

    ev::setAccessGate([cfg](const ev::AccessRequest& r) -> std::string {
        const std::string subject(r.subject);
        const std::string prefix = cfg->revoked ? std::string(ev::kPermissionRevokedPrefix) : std::string();
        switch (r.kind) {
            case ev::AccessKind::Read:
            case ev::AccessKind::Write: {
                const bool write = r.kind == ev::AccessKind::Write;
                logAccess(std::string(write ? "fs:write:" : "fs:read:") + subject);
                if (isUnder(subject, cfg->fsDenyUnder)) return prefix + "No file access to " + subject;
                if (write && isUnder(subject, cfg->fsReadOnlyUnder)) return prefix + "No write access to " + subject;
                return {};
            }
            case ev::AccessKind::Spawn:
                logAccess("spawn:" + subject);
                return cfg->spawnDeny ? prefix + "Not allowed to run " + subject : std::string();
            case ev::AccessKind::Net: {
                logAccess("net:" + subject);
                bool deny = cfg->netDenyAll;
                for (const auto& s : cfg->netDeny) deny = deny || subject.find(s) != std::string::npos;
                return deny ? prefix + "No network access to " + subject : std::string();
            }
            case ev::AccessKind::EnvRead:
            case ev::AccessKind::EnvWrite: {
                const bool write = r.kind == ev::AccessKind::EnvWrite;
                logAccess(std::string(write ? "env:write:" : "env:read:") + subject);
                const bool hidden = subject.empty() ? !cfg->envHide.empty()
                                                    : std::find(cfg->envHide.begin(), cfg->envHide.end(), subject) !=
                                                          cfg->envHide.end();
                return hidden ? prefix + "No environment access to " + (subject.empty() ? "*" : subject) : std::string();
            }
            default:
                return {};
        }
    });
    return ev::undefined();
}

bronze::Value js_clearAccessPolicy(bronze::Value, std::span<const bronze::Value>) {
    bronze::embed::setAccessGate(nullptr);
    return bronze::embed::undefined();
}

// __test_accessChanged() -> how many held resources this thread lost: what a
// host does after replacing the policy while the program runs.
bronze::Value js_accessChanged(bronze::Value, std::span<const bronze::Value>) {
    bronze::embed::accessChanged();
    return bronze::embed::fromDouble(bronze::embed::revalidateHeldAccess());
}

// __test_heldAccessCount() -> resources this thread holds open under the gate.
bronze::Value js_heldAccessCount(bronze::Value, std::span<const bronze::Value>) {
    return bronze::embed::fromDouble(static_cast<double>(bronze::embed::heldAccessCount()));
}

bronze::Value js_accessLog(bronze::Value, std::span<const bronze::Value>) {
    namespace ev = bronze::embed;
    std::vector<std::string> log;
    {
        std::lock_guard lock(g_accessLogMu);
        log = g_accessLog;
    }
    brokit::api::ArrayBuilder arr;
    for (const auto& entry : log) arr.push(ev::fromUtf8(entry));
    return arr.get();
}

} // namespace

static void captureLog(brokit::Runtime::LogLevel level, const std::string& msg) {
    g_lastLog = msg;
    using L = brokit::Runtime::LogLevel;
    FILE* out = (level == L::Error || level == L::Warn) ? stderr : stdout;
    const char* prefix = level == L::Debug ? "[DEBUG] " : level == L::Info ? "[INFO]  "
                       : level == L::Warn ? "[WARN]  " : "[ERROR] ";
    fprintf(out, "%s%s\n", prefix, msg.c_str());
}

static TestResult runTestFile(const std::string& path) {
    TestResult result;
    result.name = fs::path(path).filename().string();

    brokit::Runtime rt;

    // Install all APIs
    brokit::api::installAll();
    brokit::Runtime::setLogCallback(captureLog);
    bronze::embed::setGlobalFunction("__test_lastLog", 0, [](bronze::Value, std::span<const bronze::Value>) {
        return bronze::embed::fromUtf8(g_lastLog);
    });
    bronze::embed::setGlobalFunction("__test_fetch_tls_backend", 0, [](bronze::Value, std::span<const bronze::Value>) {
        curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
        if (!info || !(info->features & CURL_VERSION_SSL) || !info->ssl_version) {
            return bronze::embed::null();
        }
        return bronze::embed::fromUtf8(info->ssl_version);
    });
    bronze::embed::setGlobalFunction("__test_setAccessPolicy", 1, js_setAccessPolicy);
    bronze::embed::setGlobalFunction("__test_clearAccessPolicy", 0, js_clearAccessPolicy);
    bronze::embed::setGlobalFunction("__test_accessChanged", 0, js_accessChanged);
    bronze::embed::setGlobalFunction("__test_heldAccessCount", 0, js_heldAccessCount);
    bronze::embed::setGlobalFunction("__test_accessLog", 0, js_accessLog);

    // Install test helpers: assert, test registration
    const char* testHarness = R"JS(
(function() {
    var _passed = 0;
    var _failed = 0;
    var _failures = [];

    globalThis.__test_assert = function(condition, message) {
        if (condition) {
            _passed++;
        } else {
            _failed++;
            _failures.push(message || 'assertion failed');
        }
    };

    globalThis.__test_assertEqual = function(actual, expected, message) {
        var pass = actual === expected;
        if (!pass && typeof actual === 'object' && typeof expected === 'object' &&
            actual !== null && expected !== null) {
            try { pass = JSON.stringify(actual) === JSON.stringify(expected); }
            catch(e) { /* circular refs etc — leave pass as false */ }
        }
        if (pass) {
            _passed++;
        } else {
            _failed++;
            var msg = (message || 'assertEqual') + ': expected ' +
                      JSON.stringify(expected) + ', got ' + JSON.stringify(actual);
            _failures.push(msg);
        }
    };

    globalThis.__test_results = function() {
        return JSON.stringify({ passed: _passed, failed: _failed, failures: _failures });
    };

    // Convenience aliases
    globalThis.assert = globalThis.__test_assert;
    globalThis.assertEqual = globalThis.__test_assertEqual;
})();
)JS";

    if (!rt.eval(testHarness, "<test-harness>")) {
        result.failed = 1;
        result.failures.push_back("Failed to evaluate test harness");
        return result;
    }

    // Run the test file
    if (!rt.loadFile(path)) {
        result.failed = 1;
        result.failures.push_back("Could not load/execute test file: " + path);
        return result;
    }

    rt.executePendingJobs();

    // Pump async subsystems: tick curl_multi (fetch + websocket) until idle.
    namespace ev = bronze::embed;

    // Each lookup may allocate, so each result is rooted before the next.
    auto rootedFn = [](const char* name) {
        auto g = ev::globalValue(name);
        return ev::Persistent{g.found && ev::isFunction(g.value) ? g.value : ev::undefined()};
    };
    ev::Persistent pFetchHasPending = rootedFn("__brokit_fetch_has_pending");
    ev::Persistent pFetchTick = rootedFn("__brokit_fetch_tick");
    ev::Persistent pWsHasPending = rootedFn("__brokit_ws_has_pending");
    ev::Persistent pWsTick = rootedFn("__brokit_ws_tick");
    ev::Persistent pFwHasPending = rootedFn("__brokit_fs_watch_has_pending");
    ev::Persistent pFwTick = rootedFn("__brokit_fs_watch_tick");
    ev::Persistent pNetHasPending = rootedFn("__brokit_net_has_pending");
    ev::Persistent pNetTick = rootedFn("__brokit_net_tick");
    ev::Persistent pCpHasPending = rootedFn("__brokit_cp_has_pending");
    ev::Persistent pFsHasPending = rootedFn("__brokit_fs_async_has_pending");
    ev::Persistent pFsTick = rootedFn("__brokit_fs_async_tick");
    ev::Persistent pTimersTick = rootedFn("__brokit_tick_timers");

    auto isFn = [](const ev::Persistent& p) { return ev::isFunction(p.get()); };
    bool haveFetch = isFn(pFetchHasPending) && isFn(pFetchTick);
    bool haveWs = isFn(pWsHasPending) && isFn(pWsTick);
    bool haveFw = isFn(pFwHasPending) && isFn(pFwTick);
    bool haveNet = isFn(pNetHasPending) && isFn(pNetTick);
    bool haveCp = isFn(pCpHasPending);
    bool haveFs = isFn(pFsHasPending) && isFn(pFsTick);
    bool haveTimers = isFn(pTimersTick);

    if (haveFetch || haveWs || haveFw || haveNet || haveCp || haveFs) {
        for (int iters = 0; iters < 3000; iters++) { // max ~30s at 10ms sleep
            bool anyPending = false;

            if (haveFetch) {
                auto p = ev::call(pFetchHasPending.get(), ev::undefined(), {});
                if (!p.thrown && ev::toBool(p.value)) anyPending = true;
                ev::call(pFetchTick.get(), ev::undefined(), {});
            }

            if (haveWs) {
                auto p = ev::call(pWsHasPending.get(), ev::undefined(), {});
                if (!p.thrown && ev::toBool(p.value)) anyPending = true;
                ev::call(pWsTick.get(), ev::undefined(), {});
            }

            if (haveFw) {
                auto p = ev::call(pFwHasPending.get(), ev::undefined(), {});
                if (!p.thrown && ev::toBool(p.value)) anyPending = true;
                ev::call(pFwTick.get(), ev::undefined(), {});
            }

            if (haveNet) {
                auto p = ev::call(pNetHasPending.get(), ev::undefined(), {});
                if (!p.thrown && ev::toBool(p.value)) anyPending = true;
                ev::call(pNetTick.get(), ev::undefined(), {});
            }

            if (haveCp) {
                auto p = ev::call(pCpHasPending.get(), ev::undefined(), {});
                if (!p.thrown && ev::toBool(p.value)) anyPending = true;
            }

            if (haveFs) {
                auto p = ev::call(pFsHasPending.get(), ev::undefined(), {});
                if (!p.thrown && ev::toBool(p.value)) anyPending = true;
                ev::call(pFsTick.get(), ev::undefined(), {});
            }

            if (haveTimers) {
                double nowMs = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                ev::Value nowVal = ev::fromDouble(nowMs);
                ev::call(pTimersTick.get(), ev::undefined(), std::span<const ev::Value>(&nowVal, 1));
            }

            rt.executePendingJobs();
            if (!anyPending) break;

#ifdef _WIN32
            Sleep(10);
#else
            usleep(10000);
#endif
        }
    }

    // A test with asynchronous parts checks what they left here, once every
    // pump above has gone idle: an assertion that sits only in a callback
    // that never ran would otherwise never count as a failure.
    {
        auto doneFn = ev::globalValue("__test_onDone");
        if (doneFn.found && ev::isFunction(doneFn.value)) {
            ev::Persistent fn{doneFn.value};
            // The files share one realm: the hook is this file's alone.
            ev::setGlobalValue("__test_onDone", ev::undefined());
            auto r = ev::call(fn.get(), ev::undefined(), {});
            rt.executePendingJobs();
            if (r.thrown) {
                // Counted by the results below, which are the JS side's.
                auto fail = rootedFn("__test_assert");
                const ev::Value args[2] = {ev::fromBool(false), ev::fromUtf8("__test_onDone threw")};
                if (ev::isFunction(fail.get())) ev::call(fail.get(), ev::undefined(), std::span<const ev::Value>(args, 2));
            }
        }
    }

    // Collect results
    auto resultsFn = ev::globalValue("__test_results");
    if (resultsFn.found && ev::isFunction(resultsFn.value)) {
        auto res = ev::call(resultsFn.value, ev::undefined(), {});
        if (!res.thrown && ev::isString(res.value)) {
            std::string s = ev::toUtf8(res.value);
            // Extract passed count
            auto passedPos = s.find("\"passed\":");
            if (passedPos != std::string::npos) {
                result.passed = std::atoi(s.c_str() + passedPos + 9);
            }
            auto failedPos = s.find("\"failed\":");
            if (failedPos != std::string::npos) {
                result.failed = std::atoi(s.c_str() + failedPos + 9);
            }

            // Extract failures array (simplified)
            auto failuresStart = s.find("\"failures\":[");
            if (failuresStart != std::string::npos && result.failed > 0) {
                auto arrStart = s.find('[', failuresStart) + 1;
                auto arrEnd = s.rfind(']');
                if (arrEnd > arrStart) {
                    std::string arr = s.substr(arrStart, arrEnd - arrStart);
                    // Split by ","  (simplified — handles quoted strings with escaped quotes)
                    size_t pos = 0;
                    while (pos < arr.size()) {
                        if (arr[pos] == '"') {
                            pos++;
                            size_t end = pos;
                            while (end < arr.size() && arr[end] != '"') {
                                if (arr[end] == '\\') end++;
                                end++;
                            }
                            result.failures.push_back(arr.substr(pos, end - pos));
                            pos = end + 1;
                            if (pos < arr.size() && arr[pos] == ',') pos++;
                        } else {
                            pos++;
                        }
                    }
                }
            }
        }
    }

    return result;
}

int main(int argc, char* argv[]) {
    // Find test JS files
    std::string testDir;
    if (argc > 1) {
        testDir = argv[1];
    } else {
        // Look relative to executable
        fs::path exePath = fs::path(argv[0]).parent_path();
        // Try a few common locations
        for (auto& candidate : {
            exePath / "../../tests/js",
            exePath / "../../../tests/js",
            exePath / "../tests/js",
            fs::path("tests/js")
        }) {
            if (fs::exists(candidate)) {
                testDir = candidate.string();
                break;
            }
        }
    }

    if (testDir.empty() || !fs::exists(testDir)) {
        std::cerr << "Test directory not found. Usage: brokit_test [test_dir]\n";
        return 1;
    }

    std::vector<std::string> testFiles;
    if (fs::is_regular_file(testDir) && fs::path(testDir).extension() == ".js") {
        testFiles.push_back(testDir);
    } else {
        for (auto& entry : fs::directory_iterator(testDir)) {
            if (entry.path().extension() == ".js") {
                testFiles.push_back(entry.path().string());
            }
        }
    }
    std::sort(testFiles.begin(), testFiles.end());

    if (testFiles.empty()) {
        std::cerr << "No .js test files found in " << testDir << "\n";
        return 1;
    }

    int totalPassed = 0, totalFailed = 0;

    std::cout << "\n=== brokit test suite ===\n\n";

    for (auto& file : testFiles) {
        auto result = runTestFile(file);
        // A file that set a policy and failed before clearing it must not
        // refuse the next file's work.
        bronze::embed::setAccessGate(nullptr);
        totalPassed += result.passed;
        totalFailed += result.failed;

        const char* status = result.failed ? "FAIL" : "PASS";
        std::cout << "  " << status << "  " << result.name
                  << " (" << result.passed << " passed";
        if (result.failed) std::cout << ", " << result.failed << " failed";
        std::cout << ")\n";

        for (auto& f : result.failures) {
            std::cout << "         - " << f << "\n";
        }
    }

    std::cout << "\n  Total: " << totalPassed << " passed, " << totalFailed << " failed\n\n";

    return totalFailed > 0 ? 1 : 0;
}
