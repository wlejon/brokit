(function() {
    if (typeof globalThis.MessageChannel !== 'undefined') return;

    function MessagePort() {
        EventTarget.call(this);
        this._otherPort = null;
        this.onmessage = null;
        this.onmessageerror = null;
        this._started = false;
        this._queue = [];
    }
    MessagePort.prototype = Object.create(EventTarget.prototype);
    MessagePort.prototype.constructor = MessagePort;

    MessagePort.prototype.postMessage = function(data) {
        var other = this._otherPort;
        if (!other) return;
        var msg = { data: typeof structuredClone === 'function' ? structuredClone(data) : data };
        if (other._started) {
            var port = other;
            queueMicrotask(function() {
                var event = new MessageEvent('message', { data: msg.data });
                if (typeof port.onmessage === 'function') port.onmessage(event);
                port.dispatchEvent(event);
            });
        } else {
            other._queue.push(msg);
        }
    };

    MessagePort.prototype.start = function() {
        if (this._started) return;
        this._started = true;
        var port = this;
        // Drain queued messages
        var queued = this._queue;
        this._queue = [];
        for (var i = 0; i < queued.length; i++) {
            (function(msg) {
                queueMicrotask(function() {
                    var event = new MessageEvent('message', { data: msg.data });
                    if (typeof port.onmessage === 'function') port.onmessage(event);
                    port.dispatchEvent(event);
                });
            })(queued[i]);
        }
    };

    MessagePort.prototype.close = function() {
        this._otherPort = null;
        this._started = false;
        this._queue = [];
    };

    // MessageEvent
    if (typeof globalThis.MessageEvent === 'undefined') {
        globalThis.MessageEvent = function MessageEvent(type, init) {
            Event.call(this, type, init);
            this.data = (init && init.data !== undefined) ? init.data : null;
            this.origin = (init && init.origin) || '';
            this.lastEventId = (init && init.lastEventId) || '';
            this.source = (init && init.source) || null;
            this.ports = (init && init.ports) || [];
        };
        MessageEvent.prototype = Object.create(Event.prototype);
        MessageEvent.prototype.constructor = MessageEvent;
    }

    function MessageChannel() {
        this.port1 = new MessagePort();
        this.port2 = new MessagePort();
        this.port1._otherPort = this.port2;
        this.port2._otherPort = this.port1;
    }

    globalThis.MessagePort = MessagePort;
    globalThis.MessageChannel = MessageChannel;
})();
