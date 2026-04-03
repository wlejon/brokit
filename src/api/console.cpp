#include "api/api.h"
#include "runtime/runtime.h"

#include <cstring>
#include <string>

namespace brokit::api {

// Format JS values into a space-separated string (like browser console)
static std::string formatArgs(JSContext* ctx, int argc, JSValueConst* argv)
{
    std::string result;
    for (int i = 0; i < argc; i++) {
        if (i > 0) result += ' ';
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) {
            result += str;
            JS_FreeCString(ctx, str);
        } else {
            result += "[object]";
        }
    }
    return result;
}

static JSValue js_console_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Runtime::log(Runtime::LogLevel::Info, "%s", formatArgs(ctx, argc, argv).c_str());
    return JS_UNDEFINED;
}

static JSValue js_console_warn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Runtime::log(Runtime::LogLevel::Warn, "%s", formatArgs(ctx, argc, argv).c_str());
    return JS_UNDEFINED;
}

static JSValue js_console_error(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Runtime::log(Runtime::LogLevel::Error, "%s", formatArgs(ctx, argc, argv).c_str());
    return JS_UNDEFINED;
}

static JSValue js_console_debug(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Runtime::log(Runtime::LogLevel::Debug, "%s", formatArgs(ctx, argc, argv).c_str());
    return JS_UNDEFINED;
}

static JSValue js_console_assert(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    if (JS_ToBool(ctx, argv[0])) return JS_UNDEFINED;
    std::string msg = "Assertion failed";
    if (argc > 1) {
        msg += ": " + formatArgs(ctx, argc - 1, argv + 1);
    }
    Runtime::log(Runtime::LogLevel::Error, "%s", msg.c_str());
    return JS_UNDEFINED;
}

void installConsole(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, console, "log",   JS_NewCFunction(ctx, js_console_log,   "log",   1));
    JS_SetPropertyStr(ctx, console, "info",  JS_NewCFunction(ctx, js_console_log,   "info",  1));
    JS_SetPropertyStr(ctx, console, "warn",  JS_NewCFunction(ctx, js_console_warn,  "warn",  1));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, js_console_error, "error", 1));
    JS_SetPropertyStr(ctx, console, "debug", JS_NewCFunction(ctx, js_console_debug, "debug", 1));
    JS_SetPropertyStr(ctx, console, "assert",JS_NewCFunction(ctx, js_console_assert,"assert",2));

    // console.time / console.timeEnd as JS polyfill (uses performance.now if available)
    const char* timePolyfill = R"JS(
(function(c) {
    var timers = {};
    c.time = function(label) {
        label = label || 'default';
        timers[label] = Date.now();
    };
    c.timeEnd = function(label) {
        label = label || 'default';
        var start = timers[label];
        if (start === undefined) {
            c.warn('Timer "' + label + '" does not exist');
            return;
        }
        var elapsed = Date.now() - start;
        delete timers[label];
        c.log(label + ': ' + elapsed + 'ms');
    };
    c.timeLog = function(label) {
        label = label || 'default';
        var start = timers[label];
        if (start === undefined) {
            c.warn('Timer "' + label + '" does not exist');
            return;
        }
        c.log(label + ': ' + (Date.now() - start) + 'ms');
    };
})(console);
)JS";

    JS_SetPropertyStr(ctx, global, "console", console);

    JSValue r = JS_Eval(ctx, timePolyfill, strlen(timePolyfill), "<console>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
