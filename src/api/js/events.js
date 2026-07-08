// Node-compat `events` module — EventEmitter.
// Classic script (no import/export) — attaches via globalThis, matching the
// other brokit JS-layer modules (see abort.js for the install pattern).

(function() {
    'use strict';

    function EventEmitter() {
        this._events = Object.create(null);
        this._eventsCount = 0;
        this._maxListeners = undefined;
    }

    EventEmitter.defaultMaxListeners = 10;

    EventEmitter.prototype.setMaxListeners = function(n) {
        this._maxListeners = n;
        return this;
    };

    EventEmitter.prototype.getMaxListeners = function() {
        return this._maxListeners === undefined
            ? EventEmitter.defaultMaxListeners
            : this._maxListeners;
    };

    function _addListener(emitter, type, listener, prepend) {
        if (typeof listener !== 'function') {
            throw new TypeError('The "listener" argument must be of type Function');
        }
        if (!emitter._events) {
            emitter._events = Object.create(null);
            emitter._eventsCount = 0;
        }
        if (emitter._events.newListener) {
            emitter.emit('newListener', type, listener.listener ? listener.listener : listener);
        }
        var existing = emitter._events[type];
        if (existing === undefined) {
            emitter._events[type] = [listener];
            emitter._eventsCount++;
        } else {
            if (prepend) existing.unshift(listener);
            else existing.push(listener);
        }
        return emitter;
    }

    EventEmitter.prototype.addListener = function(type, listener) {
        return _addListener(this, type, listener, false);
    };
    EventEmitter.prototype.on = EventEmitter.prototype.addListener;

    EventEmitter.prototype.prependListener = function(type, listener) {
        return _addListener(this, type, listener, true);
    };

    function _onceWrap(emitter, type, listener) {
        var fired = false;
        function wrapped() {
            if (fired) return;
            fired = true;
            emitter.removeListener(type, wrapped);
            listener.apply(emitter, arguments);
        }
        wrapped.listener = listener;
        return wrapped;
    }

    EventEmitter.prototype.once = function(type, listener) {
        if (typeof listener !== 'function') {
            throw new TypeError('The "listener" argument must be of type Function');
        }
        _addListener(this, type, _onceWrap(this, type, listener), false);
        return this;
    };

    EventEmitter.prototype.prependOnceListener = function(type, listener) {
        if (typeof listener !== 'function') {
            throw new TypeError('The "listener" argument must be of type Function');
        }
        _addListener(this, type, _onceWrap(this, type, listener), true);
        return this;
    };

    EventEmitter.prototype.removeListener = function(type, listener) {
        if (typeof listener !== 'function') {
            throw new TypeError('The "listener" argument must be of type Function');
        }
        if (!this._events) return this;
        var list = this._events[type];
        if (!list) return this;

        var position = -1;
        for (var i = list.length - 1; i >= 0; i--) {
            if (list[i] === listener || list[i].listener === listener) {
                position = i;
                break;
            }
        }
        if (position < 0) return this;

        var removed = list[position];
        if (list.length === 1) {
            delete this._events[type];
            this._eventsCount--;
        } else {
            list.splice(position, 1);
        }

        if (this._events.removeListener) {
            this.emit('removeListener', type, removed.listener ? removed.listener : removed);
        }
        return this;
    };
    EventEmitter.prototype.off = EventEmitter.prototype.removeListener;

    EventEmitter.prototype.removeAllListeners = function(type) {
        if (!this._events) return this;

        if (arguments.length === 0) {
            this._events = Object.create(null);
            this._eventsCount = 0;
            return this;
        }

        var list = this._events[type];
        if (list) {
            delete this._events[type];
            this._eventsCount--;

            if (this._events.removeListener) {
                // Emit removeListener for each, in reverse-add order, using a
                // snapshot since the array is already detached.
                for (var i = list.length - 1; i >= 0; i--) {
                    this.emit('removeListener', type, list[i].listener ? list[i].listener : list[i]);
                }
            }
        }
        return this;
    };

    EventEmitter.prototype.emit = function(type) {
        var args = Array.prototype.slice.call(arguments, 1);

        var hasListeners = !!(this._events && this._events[type] && this._events[type].length);

        if (!hasListeners) {
            if (type === 'error') {
                var er = args.length > 0 ? args[0] : undefined;
                if (er instanceof Error) throw er;
                var wrapped = new Error(er !== undefined ? String(er) : 'Unhandled error.');
                wrapped.context = er;
                throw wrapped;
            }
            return false;
        }

        // Snapshot so listeners added/removed during emit don't disturb this
        // iteration (Node semantics).
        var handlers = this._events[type].slice();
        for (var i = 0; i < handlers.length; i++) {
            handlers[i].apply(this, args);
        }
        return true;
    };

    EventEmitter.prototype.listeners = function(type) {
        if (!this._events || !this._events[type]) return [];
        return this._events[type].map(function(l) { return l.listener ? l.listener : l; });
    };

    EventEmitter.prototype.rawListeners = function(type) {
        if (!this._events || !this._events[type]) return [];
        return this._events[type].slice();
    };

    EventEmitter.prototype.listenerCount = function(type) {
        if (!this._events || !this._events[type]) return 0;
        return this._events[type].length;
    };

    EventEmitter.prototype.eventNames = function() {
        if (!this._events) return [];
        var keys = Object.keys(this._events);
        if (typeof Object.getOwnPropertySymbols === 'function') {
            keys = keys.concat(Object.getOwnPropertySymbols(this._events));
        }
        return keys;
    };

    globalThis.EventEmitter = EventEmitter;

    globalThis.__brokit_modules = globalThis.__brokit_modules || {};
    var eventsModule = EventEmitter;
    eventsModule.EventEmitter = EventEmitter;
    eventsModule.default = EventEmitter;
    eventsModule.once = function(emitter, name){ return new Promise(function(res, rej){ function ok(){ emitter.off(name, ok); emitter.off('error', err); res(Array.prototype.slice.call(arguments)); } function err(e){ emitter.off(name, ok); rej(e); } emitter.on(name, ok); emitter.once('error', err); }); };
    globalThis.__brokit_modules['events'] = eventsModule;
})();
