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

    // performance.now — monotonic time, plus the User Timing API:
    // mark(name), measure(name, startMark?, endMark?), getEntries*, clearMarks,
    // clearMeasures. Entries are PerformanceEntry-like plain objects.
    if (typeof globalThis.performance === 'undefined') {
        var _startTime = globalThis.__brokit_now();
        var _entries = [];
        var _marks = Object.create(null);

        function _findLast(name) {
            for (var i = _entries.length - 1; i >= 0; i--) {
                var e = _entries[i];
                if (e.name === name) return e;
            }
            return null;
        }

        globalThis.performance = {
            now: function() { return globalThis.__brokit_now() - _startTime; },

            mark: function(name, options) {
                var startTime = (options && typeof options.startTime === 'number')
                    ? options.startTime
                    : this.now();
                var entry = {
                    name: String(name), entryType: 'mark',
                    startTime: startTime, duration: 0,
                    detail: options ? options.detail : null
                };
                _entries.push(entry);
                _marks[entry.name] = entry;
                return entry;
            },

            measure: function(name, startOrOptions, endMark) {
                var startTime = 0, endTime = this.now(), detail = null;
                if (typeof startOrOptions === 'object' && startOrOptions !== null) {
                    // measure(name, { start, end, duration, detail })
                    if (typeof startOrOptions.start === 'string') {
                        var s = _marks[startOrOptions.start];
                        if (!s) throw new Error("Mark '" + startOrOptions.start + "' not found");
                        startTime = s.startTime;
                    } else if (typeof startOrOptions.start === 'number') {
                        startTime = startOrOptions.start;
                    }
                    if (typeof startOrOptions.end === 'string') {
                        var e = _marks[startOrOptions.end];
                        if (!e) throw new Error("Mark '" + startOrOptions.end + "' not found");
                        endTime = e.startTime;
                    } else if (typeof startOrOptions.end === 'number') {
                        endTime = startOrOptions.end;
                    }
                    if (typeof startOrOptions.duration === 'number') {
                        if (typeof startOrOptions.start !== 'undefined') {
                            endTime = startTime + startOrOptions.duration;
                        } else if (typeof startOrOptions.end !== 'undefined') {
                            startTime = endTime - startOrOptions.duration;
                        }
                    }
                    detail = startOrOptions.detail || null;
                } else {
                    if (typeof startOrOptions === 'string') {
                        var sm = _marks[startOrOptions];
                        if (!sm) throw new Error("Mark '" + startOrOptions + "' not found");
                        startTime = sm.startTime;
                    }
                    if (typeof endMark === 'string') {
                        var em = _marks[endMark];
                        if (!em) throw new Error("Mark '" + endMark + "' not found");
                        endTime = em.startTime;
                    }
                }
                var entry = {
                    name: String(name), entryType: 'measure',
                    startTime: startTime, duration: endTime - startTime,
                    detail: detail
                };
                _entries.push(entry);
                return entry;
            },

            getEntries: function() { return _entries.slice(); },
            getEntriesByName: function(name, type) {
                return _entries.filter(function(e) {
                    return e.name === name && (!type || e.entryType === type);
                });
            },
            getEntriesByType: function(type) {
                return _entries.filter(function(e) { return e.entryType === type; });
            },
            clearMarks: function(name) {
                _entries = _entries.filter(function(e) {
                    if (e.entryType !== 'mark') return true;
                    if (name && e.name !== name) return true;
                    return false;
                });
                if (name) delete _marks[name];
                else _marks = Object.create(null);
            },
            clearMeasures: function(name) {
                _entries = _entries.filter(function(e) {
                    if (e.entryType !== 'measure') return true;
                    if (name && e.name !== name) return true;
                    return false;
                });
            }
        };
    }

    // requestIdleCallback / cancelIdleCallback — polyfill over setTimeout.
    // No real "idle" detection; each callback gets a nominal 50ms budget so
    // deadline.timeRemaining() shrinks as the callback runs, and didTimeout
    // flips true only if the caller-provided `timeout` deadline has passed.
    if (typeof globalThis.requestIdleCallback === 'undefined') {
        globalThis.requestIdleCallback = function(cb, options) {
            var timeout = (options && typeof options.timeout === 'number')
                ? options.timeout : 0;
            var scheduled = globalThis.__brokit_now();
            return setTimeout(function() {
                var start = globalThis.__brokit_now();
                var didTimeout = timeout > 0 && (start - scheduled) >= timeout;
                cb({
                    didTimeout: didTimeout,
                    timeRemaining: function() {
                        var remaining = 50 - (globalThis.__brokit_now() - start);
                        return remaining > 0 ? remaining : 0;
                    }
                });
            }, 1);
        };
        globalThis.cancelIdleCallback = function(id) { clearTimeout(id); };
    }
})();
