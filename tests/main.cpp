#include "runtime/runtime.h"
#include "api/api.h"

#include <chrono>
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

static TestResult runTestFile(const std::string& path) {
    TestResult result;
    result.name = fs::path(path).filename().string();

    brokit::Runtime rt;

    // Install all APIs
    brokit::api::installAll();

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

    auto fetchHasPending = ev::globalValue("__brokit_fetch_has_pending");
    auto fetchTick = ev::globalValue("__brokit_fetch_tick");
    auto wsHasPending = ev::globalValue("__brokit_ws_has_pending");
    auto wsTick = ev::globalValue("__brokit_ws_tick");
    auto fwHasPending = ev::globalValue("__brokit_fs_watch_has_pending");
    auto fwTick = ev::globalValue("__brokit_fs_watch_tick");
    auto netHasPending = ev::globalValue("__brokit_net_has_pending");
    auto netTick = ev::globalValue("__brokit_net_tick");
    auto cpHasPending = ev::globalValue("__brokit_cp_has_pending");
    auto timersTick = ev::globalValue("__brokit_tick_timers");

    bool haveFetch = fetchHasPending.found && ev::isFunction(fetchHasPending.value) &&
                     fetchTick.found && ev::isFunction(fetchTick.value);
    bool haveWs = wsHasPending.found && ev::isFunction(wsHasPending.value) &&
                  wsTick.found && ev::isFunction(wsTick.value);
    bool haveFw = fwHasPending.found && ev::isFunction(fwHasPending.value) &&
                  fwTick.found && ev::isFunction(fwTick.value);
    bool haveNet = netHasPending.found && ev::isFunction(netHasPending.value) &&
                   netTick.found && ev::isFunction(netTick.value);
    bool haveCp = cpHasPending.found && ev::isFunction(cpHasPending.value);
    bool haveTimers = timersTick.found && ev::isFunction(timersTick.value);

    ev::Persistent pFetchHasPending{haveFetch ? fetchHasPending.value : ev::undefined()};
    ev::Persistent pFetchTick{haveFetch ? fetchTick.value : ev::undefined()};
    ev::Persistent pWsHasPending{haveWs ? wsHasPending.value : ev::undefined()};
    ev::Persistent pWsTick{haveWs ? wsTick.value : ev::undefined()};
    ev::Persistent pFwHasPending{haveFw ? fwHasPending.value : ev::undefined()};
    ev::Persistent pFwTick{haveFw ? fwTick.value : ev::undefined()};
    ev::Persistent pNetHasPending{haveNet ? netHasPending.value : ev::undefined()};
    ev::Persistent pNetTick{haveNet ? netTick.value : ev::undefined()};
    ev::Persistent pCpHasPending{haveCp ? cpHasPending.value : ev::undefined()};
    ev::Persistent pTimersTick{haveTimers ? timersTick.value : ev::undefined()};

    if (haveFetch || haveWs || haveFw || haveNet || haveCp) {
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
