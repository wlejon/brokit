(function() {
    if (typeof globalThis.EventTarget !== 'undefined') return;

    // -----------------------------------------------------------------------
    // Event
    // -----------------------------------------------------------------------
    if (typeof globalThis.Event === 'undefined') {
        globalThis.Event = function Event(type, options) {
            options = options || {};
            this.type = type;
            this.bubbles = !!options.bubbles;
            this.cancelable = !!options.cancelable;
            this.composed = !!options.composed;
            this.defaultPrevented = false;
            this.target = null;
            this.currentTarget = null;
            this.eventPhase = 0;
            this.timeStamp = performance.now();
            this.isTrusted = false;
            this._stopPropagation = false;
            this._stopImmediate = false;
        };
        Event.prototype.preventDefault = function() {
            if (this.cancelable) this.defaultPrevented = true;
        };
        Event.prototype.stopPropagation = function() {
            this._stopPropagation = true;
        };
        Event.prototype.stopImmediatePropagation = function() {
            this._stopImmediate = true;
            this._stopPropagation = true;
        };
        Event.prototype.composedPath = function() {
            return this.target ? [this.target] : [];
        };
        Event.NONE = 0;
        Event.CAPTURING_PHASE = 1;
        Event.AT_TARGET = 2;
        Event.BUBBLING_PHASE = 3;
    }

    // -----------------------------------------------------------------------
    // CustomEvent
    // -----------------------------------------------------------------------
    if (typeof globalThis.CustomEvent === 'undefined') {
        globalThis.CustomEvent = function CustomEvent(type, options) {
            Event.call(this, type, options);
            this.detail = (options && options.detail !== undefined) ? options.detail : null;
        };
        CustomEvent.prototype = Object.create(Event.prototype);
        CustomEvent.prototype.constructor = CustomEvent;
    }

    // -----------------------------------------------------------------------
    // EventTarget
    // -----------------------------------------------------------------------
    function EventTarget() {
        this._listeners = {};
    }

    EventTarget.prototype.addEventListener = function(type, listener, options) {
        if (typeof listener !== 'function' && !(listener && typeof listener.handleEvent === 'function')) return;
        var once = false, capture = false;
        if (typeof options === 'boolean') {
            capture = options;
        } else if (options && typeof options === 'object') {
            once = !!options.once;
            capture = !!options.capture;
        }
        if (!this._listeners[type]) this._listeners[type] = [];
        // Deduplicate
        var list = this._listeners[type];
        for (var i = 0; i < list.length; i++) {
            if (list[i].listener === listener && list[i].capture === capture) return;
        }
        list.push({ listener: listener, once: once, capture: capture });
    };

    EventTarget.prototype.removeEventListener = function(type, listener, options) {
        if (!this._listeners[type]) return;
        var capture = false;
        if (typeof options === 'boolean') capture = options;
        else if (options && typeof options === 'object') capture = !!options.capture;
        var list = this._listeners[type];
        for (var i = 0; i < list.length; i++) {
            if (list[i].listener === listener && list[i].capture === capture) {
                list.splice(i, 1);
                break;
            }
        }
    };

    EventTarget.prototype.dispatchEvent = function(event) {
        event.target = this;
        event.currentTarget = this;
        var list = this._listeners[event.type];
        if (!list) return !event.defaultPrevented;
        var handlers = list.slice(); // snapshot
        for (var i = 0; i < handlers.length; i++) {
            if (event._stopImmediate) break;
            var entry = handlers[i];
            if (entry.once) {
                this.removeEventListener(event.type, entry.listener, entry.capture);
            }
            if (typeof entry.listener === 'function') {
                entry.listener.call(this, event);
            } else if (entry.listener && typeof entry.listener.handleEvent === 'function') {
                entry.listener.handleEvent(event);
            }
        }
        return !event.defaultPrevented;
    };

    globalThis.EventTarget = EventTarget;
})();
