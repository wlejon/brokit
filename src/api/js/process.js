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
})();
