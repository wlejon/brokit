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
    function AbortSignal() {
        this.aborted = false;
        this.reason = undefined;
        this._listeners = [];
    }

    Object.defineProperty(AbortSignal.prototype, 'onabort', {
        get: function() { return this._onabort || null; },
        set: function(fn) { this._onabort = fn; }
    });

    AbortSignal.prototype.addEventListener = function(type, fn) {
        if (type === 'abort' && typeof fn === 'function') {
            this._listeners.push(fn);
        }
    };

    AbortSignal.prototype.removeEventListener = function(type, fn) {
        if (type === 'abort') {
            this._listeners = this._listeners.filter(function(f) { return f !== fn; });
        }
    };

    AbortSignal.prototype.dispatchEvent = function(event) {
        if (event.type === 'abort') {
            if (typeof this._onabort === 'function') {
                this._onabort.call(this, event);
            }
            for (var i = 0; i < this._listeners.length; i++) {
                this._listeners[i].call(this, event);
            }
        }
        return true;
    };

    AbortSignal.prototype.throwIfAborted = function() {
        if (this.aborted) {
            throw this.reason;
        }
    };

    // Static factory: AbortSignal.abort(reason?)
    AbortSignal.abort = function(reason) {
        var signal = new AbortSignal();
        signal.aborted = true;
        signal.reason = (reason !== undefined) ? reason : new globalThis.DOMException('The operation was aborted.', 'AbortError');
        return signal;
    };

    // Static factory: AbortSignal.timeout(ms)
    AbortSignal.timeout = function(ms) {
        var signal = new AbortSignal();
        setTimeout(function() {
            if (!signal.aborted) {
                signal.aborted = true;
                signal.reason = new globalThis.DOMException('The operation timed out.', 'TimeoutError');
                signal.dispatchEvent({ type: 'abort' });
            }
        }, ms);
        return signal;
    };

    // Static factory: AbortSignal.any(signals)
    AbortSignal.any = function(signals) {
        var signal = new AbortSignal();
        for (var i = 0; i < signals.length; i++) {
            if (signals[i].aborted) {
                signal.aborted = true;
                signal.reason = signals[i].reason;
                return signal;
            }
        }
        function onAbort() {
            if (!signal.aborted) {
                signal.aborted = true;
                signal.reason = this.reason;
                signal.dispatchEvent({ type: 'abort' });
            }
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
        if (!this.signal.aborted) {
            this.signal.aborted = true;
            this.signal.reason = (reason !== undefined) ? reason : new globalThis.DOMException('The operation was aborted.', 'AbortError');
            this.signal.dispatchEvent({ type: 'abort' });
        }
    };

    globalThis.AbortController = AbortController;
    globalThis.AbortSignal = AbortSignal;
})();
