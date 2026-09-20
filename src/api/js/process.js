(function() {
    var process = globalThis.process;
    if (!process) return;

    // process.nextTick(fn, ...args) — schedule via the microtask queue
    process.nextTick = function(fn) {
        if (typeof fn !== 'function') return;
        var args = Array.prototype.slice.call(arguments, 1);
        queueMicrotask(function() { fn.apply(null, args); });
    };

    // process.hrtime([prev]) — [seconds, nanoseconds], relative to performance.now()
    function hrtime(prev) {
        var ms = performance.now();
        var sec = Math.floor(ms / 1000);
        var nsec = Math.round((ms - sec * 1000) * 1e6);
        if (prev && prev.length === 2) {
            sec -= prev[0];
            nsec -= prev[1];
            if (nsec < 0) {
                sec -= 1;
                nsec += 1e9;
            }
        }
        return [sec, nsec];
    }
    hrtime.bigint = function() {
        return BigInt(Math.round(performance.now() * 1e6));
    };
    process.hrtime = hrtime;

    // process.stdout / process.stderr
    process.stdout = {
        write: function(s) {
            console.log(String(s).replace(/\n$/, ''));
            return true;
        },
        isTTY: false,
        fd: 1
    };
    process.stderr = {
        write: function(s) {
            console.error(String(s).replace(/\n$/, ''));
            return true;
        },
        isTTY: false,
        fd: 2
    };

    // process.emitWarning — no-op
    process.emitWarning = function() {};

    // Event emitter semantics for process
    var _events = Object.create(null);
    var _maxListeners = 10;
    process._events = _events;

    process.addListener = function(event, listener) {
        if (typeof listener !== 'function') {
            throw new TypeError('The "listener" argument must be of type Function');
        }
        var key = String(event);
        if (!_events[key]) {
            _events[key] = [];
        }
        _events[key].push(listener);
        return process;
    };
    process.on = process.addListener;

    process.prependListener = function(event, listener) {
        if (typeof listener !== 'function') {
            throw new TypeError('The "listener" argument must be of type Function');
        }
        var key = String(event);
        if (!_events[key]) {
            _events[key] = [];
        }
        _events[key].unshift(listener);
        return process;
    };

    process.once = function(event, listener) {
        if (typeof listener !== 'function') {
            throw new TypeError('The "listener" argument must be of type Function');
        }
        var g = function() {
            process.removeListener(event, g);
            listener.apply(process, arguments);
        };
        g.listener = listener;
        return process.addListener(event, g);
    };

    process.prependOnceListener = function(event, listener) {
        if (typeof listener !== 'function') {
            throw new TypeError('The "listener" argument must be of type Function');
        }
        var g = function() {
            process.removeListener(event, g);
            listener.apply(process, arguments);
        };
        g.listener = listener;
        return process.prependListener(event, g);
    };

    process.removeListener = function(event, listener) {
        if (typeof listener !== 'function') {
            throw new TypeError('The "listener" argument must be of type Function');
        }
        var key = String(event);
        var list = _events[key];
        if (!list) return process;
        for (var i = list.length - 1; i >= 0; i--) {
            if (list[i] === listener || list[i].listener === listener) {
                list.splice(i, 1);
                break;
            }
        }
        if (list.length === 0) {
            delete _events[key];
        }
        return process;
    };
    process.off = process.removeListener;

    process.removeAllListeners = function(event) {
        if (event === undefined) {
            _events = Object.create(null);
            process._events = _events;
        } else {
            delete _events[String(event)];
        }
        return process;
    };

    process.emit = function(event) {
        var key = String(event);
        var list = _events[key];
        var args = Array.prototype.slice.call(arguments, 1);
        if (!list || list.length === 0) {
            if (key === 'error') {
                var err = args[0];
                if (err instanceof Error) throw err;
                throw new Error(err !== undefined ? String(err) : 'Unhandled error.');
            }
            return false;
        }
        var copy = list.slice();
        for (var i = 0; i < copy.length; i++) {
            copy[i].apply(process, args);
        }
        return true;
    };

    process.listeners = function(event) {
        var list = _events[String(event)];
        if (!list) return [];
        return list.map(function(fn) { return fn.listener || fn; });
    };

    process.rawListeners = function(event) {
        var list = _events[String(event)];
        if (!list) return [];
        return list.slice();
    };

    process.listenerCount = function(event) {
        var list = _events[String(event)];
        return list ? list.length : 0;
    };

    process.eventNames = function() {
        return Object.keys(_events);
    };

    process.setMaxListeners = function(n) {
        _maxListeners = n;
        return process;
    };

    process.getMaxListeners = function() {
        return _maxListeners;
    };
})();
