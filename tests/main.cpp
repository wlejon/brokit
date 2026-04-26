#include "runtime/runtime.h"
#include "api/api.h"

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

extern "C" {
#include "quickjs.h"
}

namespace fs = std::filesystem;

struct TestResult {
    std::string name;
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;
};

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static TestResult runTestFile(const std::string& path) {
    TestResult result;
    result.name = fs::path(path).filename().string();

    brokit::Runtime rt;
    if (!rt.context()) {
        result.failed = 1;
        result.failures.push_back("Failed to create runtime");
        return result;
    }

    // Install all APIs
    brokit::api::installAll(rt.context());

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

    rt.eval(testHarness, "<test-harness>");

    // Run the test file
    std::string code = readFile(path);
    if (code.empty()) {
        result.failed = 1;
        result.failures.push_back("Could not read test file: " + path);
        return result;
    }

    rt.eval(code, path);
    rt.executePendingJobs();

    // Pump async subsystems: tick curl_multi (fetch + websocket) until idle.
    JSContext* ctx = rt.context();
    {
        JSValue global2 = JS_GetGlobalObject(ctx);
        JSValue fetchHasPending = JS_GetPropertyStr(ctx, global2, "__brokit_fetch_has_pending");
        JSValue fetchTick = JS_GetPropertyStr(ctx, global2, "__brokit_fetch_tick");
        JSValue wsHasPending = JS_GetPropertyStr(ctx, global2, "__brokit_ws_has_pending");
        JSValue wsTick = JS_GetPropertyStr(ctx, global2, "__brokit_ws_tick");
        JSValue fwHasPending = JS_GetPropertyStr(ctx, global2, "__brokit_fs_watch_has_pending");
        JSValue fwTick = JS_GetPropertyStr(ctx, global2, "__brokit_fs_watch_tick");

        bool haveFetch = JS_IsFunction(ctx, fetchHasPending) && JS_IsFunction(ctx, fetchTick);
        bool haveWs = JS_IsFunction(ctx, wsHasPending) && JS_IsFunction(ctx, wsTick);
        bool haveFw = JS_IsFunction(ctx, fwHasPending) && JS_IsFunction(ctx, fwTick);

        if (haveFetch || haveWs || haveFw) {
            for (int iters = 0; iters < 3000; iters++) { // max ~30s at 10ms sleep
                bool anyPending = false;

                if (haveFetch) {
                    JSValue p = JS_Call(ctx, fetchHasPending, global2, 0, nullptr);
                    if (JS_ToBool(ctx, p)) anyPending = true;
                    JS_FreeValue(ctx, p);

                    JSValue tr = JS_Call(ctx, fetchTick, global2, 0, nullptr);
                    JS_FreeValue(ctx, tr);
                }

                if (haveWs) {
                    JSValue p = JS_Call(ctx, wsHasPending, global2, 0, nullptr);
                    if (JS_ToBool(ctx, p)) anyPending = true;
                    JS_FreeValue(ctx, p);

                    JSValue tr = JS_Call(ctx, wsTick, global2, 0, nullptr);
                    JS_FreeValue(ctx, tr);
                }

                if (haveFw) {
                    JSValue p = JS_Call(ctx, fwHasPending, global2, 0, nullptr);
                    if (JS_ToBool(ctx, p)) anyPending = true;
                    JS_FreeValue(ctx, p);

                    JSValue tr = JS_Call(ctx, fwTick, global2, 0, nullptr);
                    JS_FreeValue(ctx, tr);
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

        JS_FreeValue(ctx, fwTick);
        JS_FreeValue(ctx, fwHasPending);
        JS_FreeValue(ctx, wsTick);
        JS_FreeValue(ctx, wsHasPending);
        JS_FreeValue(ctx, fetchTick);
        JS_FreeValue(ctx, fetchHasPending);
        JS_FreeValue(ctx, global2);
    }

    // Collect results
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__test_results");
    JSValue res = JS_Call(ctx, fn, global, 0, nullptr);

    if (!JS_IsException(res)) {
        const char* json = JS_ToCString(ctx, res);
        if (json) {
            // Simple JSON parsing for { passed, failed, failures }
            std::string s(json);
            JS_FreeCString(ctx, json);

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

    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);

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
