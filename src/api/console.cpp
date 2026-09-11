#include "api/api.h"
#include "api/object_builder.h"
#include "api/arg_reader.h"
#include "runtime/runtime.h"

#include <chrono>
#include <string>
#include <unordered_map>

namespace brokit::api {

namespace {

std::string formatArgs(std::span<const Value> args) {
    std::string result;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) result += ' ';
        Value v = args[i];
        if (ev::isUndefined(v)) {
            result += "undefined";
        } else if (ev::isNull(v)) {
            result += "null";
        } else if (ev::isString(v) || ev::isNumber(v) || ev::isBool(v)) {
            result += ev::toUtf8(v);
        } else {
            result += "[object]";
        }
    }
    return result;
}

std::unordered_map<std::string, std::chrono::steady_clock::time_point>& consoleTimers() {
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> timers;
    return timers;
}

} // namespace

void installConsole() {
    ObjectBuilder console;

    console.def("log", 0, [](Value, std::span<const Value> a) {
        Runtime::log(Runtime::LogLevel::Info, "%s", formatArgs(a).c_str());
        return ev::undefined();
    });

    console.def("info", 0, [](Value, std::span<const Value> a) {
        Runtime::log(Runtime::LogLevel::Info, "%s", formatArgs(a).c_str());
        return ev::undefined();
    });

    console.def("warn", 0, [](Value, std::span<const Value> a) {
        Runtime::log(Runtime::LogLevel::Warn, "%s", formatArgs(a).c_str());
        return ev::undefined();
    });

    console.def("error", 0, [](Value, std::span<const Value> a) {
        Runtime::log(Runtime::LogLevel::Error, "%s", formatArgs(a).c_str());
        return ev::undefined();
    });

    console.def("debug", 0, [](Value, std::span<const Value> a) {
        Runtime::log(Runtime::LogLevel::Debug, "%s", formatArgs(a).c_str());
        return ev::undefined();
    });

    console.def("assert", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::undefined();
        if (boolAt(a, 0)) return ev::undefined();
        std::string msg = "Assertion failed";
        if (a.size() > 1) {
            msg += ": " + formatArgs(a.subspan(1));
        }
        Runtime::log(Runtime::LogLevel::Error, "%s", msg.c_str());
        return ev::undefined();
    });

    console.def("time", 0, [](Value, std::span<const Value> a) {
        std::string label = hasArg(a, 0) ? strAt(a, 0) : "default";
        consoleTimers()[label] = std::chrono::steady_clock::now();
        return ev::undefined();
    });

    console.def("timeLog", 0, [](Value, std::span<const Value> a) {
        std::string label = hasArg(a, 0) ? strAt(a, 0) : "default";
        auto& t = consoleTimers();
        auto it = t.find(label);
        if (it == t.end()) {
            Runtime::log(Runtime::LogLevel::Warn, "Timer '%s' does not exist", label.c_str());
            return ev::undefined();
        }
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - it->second).count();
        std::string extra;
        if (a.size() > 1) {
            extra = " " + formatArgs(a.subspan(1));
        }
        Runtime::log(Runtime::LogLevel::Info, "%s: %.3fms%s", label.c_str(), ms, extra.c_str());
        return ev::undefined();
    });

    console.def("timeEnd", 0, [](Value, std::span<const Value> a) {
        std::string label = hasArg(a, 0) ? strAt(a, 0) : "default";
        auto& t = consoleTimers();
        auto it = t.find(label);
        if (it == t.end()) {
            Runtime::log(Runtime::LogLevel::Warn, "Timer '%s' does not exist", label.c_str());
            return ev::undefined();
        }
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - it->second).count();
        t.erase(it);
        Runtime::log(Runtime::LogLevel::Info, "%s: %.3fms", label.c_str(), ms);
        return ev::undefined();
    });

    ev::setGlobalValue("console", console.get());
}

} // namespace brokit::api
