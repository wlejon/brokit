#include "api/api.h"
#include "runtime/runtime.h"

#include <cstring>

namespace brokit::api {

// Timers are implemented as a JS polyfill that stores callbacks in a map.
// The consumer (e.g., bro's engine) drives time by calling brokit::Timers::tick().
// For standalone/headless use, a real-time tick loop can be used.
//
// This installs: setTimeout, setInterval, clearTimeout, clearInterval, queueMicrotask

void installTimers(JSContext* ctx)
{
    const char* polyfill = R"JS(
(function() {
    var _nextId = 1;
    var _timers = {};

    // Internal: current time in ms. Overridable for virtual time.
    if (typeof globalThis.__brokit_now === 'undefined') {
        globalThis.__brokit_now = function() { return Date.now(); };
    }

    globalThis.setTimeout = function(fn, delay) {
        if (typeof fn !== 'function') return 0;
        var id = _nextId++;
        var args = Array.prototype.slice.call(arguments, 2);
        _timers[id] = {
            fn: fn, args: args,
            fireAt: globalThis.__brokit_now() + (delay || 0),
            interval: 0
        };
        return id;
    };

    globalThis.setInterval = function(fn, delay) {
        if (typeof fn !== 'function') return 0;
        var id = _nextId++;
        var args = Array.prototype.slice.call(arguments, 2);
        var ms = delay || 0;
        _timers[id] = {
            fn: fn, args: args,
            fireAt: globalThis.__brokit_now() + ms,
            interval: ms
        };
        return id;
    };

    globalThis.clearTimeout = function(id) { delete _timers[id]; };
    globalThis.clearInterval = function(id) { delete _timers[id]; };

    // Tick: fire all timers whose fireAt <= now.
    // Returns ms until the next timer fires, or -1 if none.
    globalThis.__brokit_tick_timers = function(now) {
        var fired = [];
        for (var id in _timers) {
            var t = _timers[id];
            if (t.fireAt <= now) fired.push(id);
        }
        for (var i = 0; i < fired.length; i++) {
            var id = fired[i];
            var t = _timers[id];
            if (!t) continue;
            if (t.interval > 0) {
                t.fireAt = now + t.interval;
            } else {
                delete _timers[id];
            }
            try { t.fn.apply(null, t.args); } catch(e) { console.error('Timer error:', e); }
        }
        // Find next fire time
        var next = -1;
        for (var id in _timers) {
            var t = _timers[id];
            var remaining = t.fireAt - now;
            if (next < 0 || remaining < next) next = remaining;
        }
        return next;
    };

    // queueMicrotask — via Promise
    if (typeof globalThis.queueMicrotask === 'undefined') {
        globalThis.queueMicrotask = function(cb) {
            if (typeof cb !== 'function') throw new TypeError('queueMicrotask: argument must be a function');
            Promise.resolve().then(cb).catch(function(e) {
                setTimeout(function() { throw e; }, 0);
            });
        };
    }

    // performance.now — monotonic time in ms
    if (typeof globalThis.performance === 'undefined') {
        var _startTime = globalThis.__brokit_now();
        globalThis.performance = {
            now: function() { return globalThis.__brokit_now() - _startTime; }
        };
    }
})();
)JS";

    JSValue r = JS_Eval(ctx, polyfill, strlen(polyfill), "<timers>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
