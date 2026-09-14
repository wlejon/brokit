(function() {
    // --- DOMException (minimal, if not already defined) ---
    if (typeof globalThis.DOMException === 'undefined') {
        var _DOMException = function DOMException(message, name) {
            this.message = message || '';
            this.name = name || 'Error';
        };
        _DOMException.prototype = Object.create(Error.prototype);
        _DOMException.prototype.constructor = _DOMException;
        globalThis.DOMException = _DOMException;
    }

    // --- AbortSignal ---
    //
    // `aborted` and `reason` are read-only accessors over private state, as on
    // the web: the only writer is an abort, so a host that reads `aborted`
    // (fetch.cpp does) reads the truth. Listeners are kept here rather than
    // inherited from EventTarget because this installs before event_target.js
    // does; the event dispatched is still the platform `Event`, looked up when
    // the abort happens, so `event.target === signal` in every listener.
    function AbortSignal() {
        this._aborted = false;
        this._reason = undefined;
        this._listeners = [];
        this._onabort = null;
    }

    Object.defineProperties(AbortSignal.prototype, {
        aborted: { get: function() { return this._aborted; }, enumerable: true, configurable: true },
        reason:  { get: function() { return this._reason; }, enumerable: true, configurable: true },
        onabort: {
            get: function() { return this._onabort; },
            set: function(fn) { this._onabort = (typeof fn === 'function') ? fn : null; },
            enumerable: true, configurable: true
        }
    });

    AbortSignal.prototype.addEventListener = function(type, listener, options) {
        if (type !== 'abort') return;
        var callable = typeof listener === 'function' ||
            (listener && typeof listener.handleEvent === 'function');
        if (!callable) return;
        var once = !!(options && typeof options === 'object' && options.once);
        for (var i = 0; i < this._listeners.length; i++) {
            if (this._listeners[i].listener === listener) return;
        }
        this._listeners.push({ listener: listener, once: once });
    };

    AbortSignal.prototype.removeEventListener = function(type, listener) {
        if (type !== 'abort') return;
        for (var i = 0; i < this._listeners.length; i++) {
            if (this._listeners[i].listener === listener) {
                this._listeners.splice(i, 1);
                return;
            }
        }
    };

    AbortSignal.prototype.dispatchEvent = function(event) {
        event.target = this;
        event.currentTarget = this;
        if (event.type === 'abort') {
            if (typeof this._onabort === 'function') {
                this._onabort.call(this, event);
            }
            var handlers = this._listeners.slice();
            for (var i = 0; i < handlers.length; i++) {
                if (event._stopImmediate) break;
                var entry = handlers[i];
                if (entry.once) this.removeEventListener('abort', entry.listener);
                if (typeof entry.listener === 'function') {
                    entry.listener.call(this, event);
                } else {
                    entry.listener.handleEvent(event);
                }
            }
        }
        return !event.defaultPrevented;
    };

    AbortSignal.prototype.throwIfAborted = function() {
        if (this._aborted) {
            throw this._reason;
        }
    };

    // Abort `signal` once: the first reason sticks, a second abort fires
    // nothing. The event is the platform Event so listeners see a target.
    function abortSignal(signal, reason) {
        if (signal._aborted) return;
        signal._aborted = true;
        signal._reason = (reason !== undefined)
            ? reason
            : new globalThis.DOMException('The operation was aborted.', 'AbortError');
        signal.dispatchEvent(new globalThis.Event('abort'));
    }

    // Static factory: AbortSignal.abort(reason?) — born aborted, no event.
    AbortSignal.abort = function(reason) {
        var signal = new AbortSignal();
        signal._aborted = true;
        signal._reason = (reason !== undefined)
            ? reason
            : new globalThis.DOMException('The operation was aborted.', 'AbortError');
        return signal;
    };

    // Static factory: AbortSignal.timeout(ms)
    AbortSignal.timeout = function(ms) {
        var signal = new AbortSignal();
        setTimeout(function() {
            abortSignal(signal, new globalThis.DOMException('The operation timed out.', 'TimeoutError'));
        }, ms);
        return signal;
    };

    // Static factory: AbortSignal.any(signals)
    AbortSignal.any = function(signals) {
        var signal = new AbortSignal();
        for (var i = 0; i < signals.length; i++) {
            if (signals[i].aborted) {
                signal._aborted = true;
                signal._reason = signals[i].reason;
                return signal;
            }
        }
        function onAbort() {
            abortSignal(signal, this.reason);
        }
        for (var i = 0; i < signals.length; i++) {
            signals[i].addEventListener('abort', onAbort);
        }
        return signal;
    };

    // --- AbortController ---
    function AbortController() {
        this.signal = new AbortSignal();
    }

    AbortController.prototype.abort = function(reason) {
        abortSignal(this.signal, reason);
    };

    globalThis.AbortController = AbortController;
    globalThis.AbortSignal = AbortSignal;
})();
