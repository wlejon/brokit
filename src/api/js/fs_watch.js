// FSWatcher — JS facade over the native fs.watch implementation.
//
// The C++ tick (__brokit_fs_watch_tick) drains queued events from each
// watcher's lock-free ring and calls __brokit_fs_watch_dispatch(id, type,
// filename). That dispatcher routes back to the FSWatcher instance bound to
// `id`, which emits 'change' / 'error' to its listeners.
//
// API surface mirrors Node's fs.watch():
//   const w = fs.watch(path, options, listener);
//   options: { recursive: bool, persistent: bool, encoding: 'utf-8' }
//   listener: (eventType, filename) — eventType ∈ 'rename' | 'change'
//   w.on('change', listener);  w.on('error', err);  w.on('close', () => {})
//   w.close();

(function () {
    var registry = Object.create(null);

    function FSWatcher(path, opts) {
        this._path      = path;
        this._recursive = !!(opts && opts.recursive);
        this._listeners = { change: [], error: [], close: [] };
        this._closed    = false;
        this._id        = globalThis.__brokit_fs_watch_create(path, this._recursive);
        registry[this._id] = this;
    }

    FSWatcher.prototype.on = function (event, fn) {
        if (typeof fn !== 'function') return this;
        if (this._listeners[event]) this._listeners[event].push(fn);
        return this;
    };

    FSWatcher.prototype.addListener = FSWatcher.prototype.on;

    FSWatcher.prototype.off = function (event, fn) {
        var arr = this._listeners[event];
        if (!arr) return this;
        var i = arr.indexOf(fn);
        if (i >= 0) arr.splice(i, 1);
        return this;
    };

    FSWatcher.prototype.removeListener = FSWatcher.prototype.off;

    FSWatcher.prototype.removeAllListeners = function (event) {
        if (event) {
            if (this._listeners[event]) this._listeners[event] = [];
        } else {
            this._listeners.change = [];
            this._listeners.error  = [];
            this._listeners.close  = [];
        }
        return this;
    };

    FSWatcher.prototype._emit = function (event, a, b) {
        var arr = this._listeners[event];
        if (!arr || arr.length === 0) return;
        // Iterate over a snapshot so handlers can off() themselves safely.
        var snap = arr.slice();
        for (var i = 0; i < snap.length; i++) {
            try { snap[i].call(this, a, b); }
            catch (e) {
                if (event !== 'error' && this._listeners.error.length) {
                    this._emit('error', e);
                } else {
                    // Last-resort: don't swallow silently.
                    if (typeof console !== 'undefined' && console.error) {
                        console.error('FSWatcher listener threw:', e && e.message || e);
                    }
                }
            }
        }
    };

    FSWatcher.prototype.close = function () {
        if (this._closed) return;
        this._closed = true;
        delete registry[this._id];
        globalThis.__brokit_fs_watch_close(this._id);
        this._emit('close');
    };

    // Called from C++ tick. Keep this function on globalThis so the native
    // side can find it by name.
    globalThis.__brokit_fs_watch_dispatch = function (id, type, filename) {
        var w = registry[id];
        if (!w || w._closed) return;
        if (type === 'error') {
            // Surface as an Error event, not a change.
            var err = new Error(filename || 'fs.watch error');
            err.code = 'EWATCHER';
            w._emit('error', err);
            return;
        }
        // type ∈ 'change' | 'rename'. Both fire the 'change' listener with
        // (eventType, filename) — matches Node's contract.
        w._emit('change', type, filename);
    };

    // ── fs.watch wiring ──────────────────────────────────────────────────────
    // Patches the existing __brokit_fs object (created by fs.js, which runs
    // before this file) to expose `fs.watch`.

    function watch(path, options, listener) {
        if (typeof options === 'function') { listener = options; options = null; }
        var w = new FSWatcher(path, options || {});
        if (typeof listener === 'function') w.on('change', listener);
        return w;
    }

    if (globalThis.__brokit_fs) {
        globalThis.__brokit_fs.watch    = watch;
        globalThis.__brokit_fs.FSWatcher = FSWatcher;
    }
})();
