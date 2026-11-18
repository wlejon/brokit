(function() {
    if (typeof globalThis.MessageChannel !== 'undefined') return;

    function MessagePort() {
        EventTarget.call(this);
        this._otherPort = null;
        this._onmessage = null;
        this.onmessageerror = null;
        this._started = false;
        this._queue = [];
    }
    MessagePort.prototype = Object.create(EventTarget.prototype);
    MessagePort.prototype.constructor = MessagePort;

    // Setting onmessage starts the port, as in a browser (HTML's "message
    // port's onmessage setter ... start()"): React's scheduler sets
    // port1.onmessage and never calls start().
    Object.defineProperty(MessagePort.prototype, 'onmessage', {
        get: function() { return this._onmessage; },
        set: function(fn) { this._onmessage = typeof fn === 'function' ? fn : null; if (this._onmessage) this.start(); },
        enumerable: true, configurable: true,
    });

    // A posted message is a task, not a microtask: timers and rendering run
    // between messages, as on the web.
    function deliverLater(fn) {
        if (typeof setTimeout === 'function') setTimeout(fn, 0); else queueMicrotask(fn);
    }

    MessagePort.prototype.postMessage = function(data, transferOrOptions) {
        var other = this._otherPort;
        if (!other) return;

        var transferList = [];
        if (transferOrOptions) {
            if (Array.isArray(transferOrOptions)) {
                transferList = transferOrOptions;
            } else if (Array.isArray(transferOrOptions.transfer)) {
                transferList = transferOrOptions.transfer;
            }
        }

        var clonedData;
        if (typeof structuredClone === 'function') {
            clonedData = structuredClone(data, transferList.length > 0 ? { transfer: transferList } : undefined);
        } else {
            clonedData = data;
        }

        var transferredPorts = [];
        for (var pi = 0; pi < transferList.length; pi++) {
            if (typeof MessagePort !== 'undefined' && transferList[pi] instanceof MessagePort) {
                transferredPorts.push(transferList[pi]);
            }
        }

        var msg = { data: clonedData, ports: transferredPorts };
        if (other._started) {
            var port = other;
            deliverLater(function() {
                if (!port._started) { port._queue.push(msg); return; }
                var event = new MessageEvent('message', { data: msg.data, ports: msg.ports });
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
                deliverLater(function() {
                    var event = new MessageEvent('message', { data: msg.data, ports: msg.ports || [] });
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

    // MessageEvent — use class syntax so we work whether globalThis.Event is
    // a class (e.g. bro's dom polyfills) or a function constructor (brokit
    // standalone). Calling `Event.call(this, ...)` on a class constructor
    // throws "class constructors must be invoked with 'new'".
    if (typeof globalThis.MessageEvent === 'undefined') {
        globalThis.MessageEvent = class MessageEvent extends Event {
            constructor(type, init) {
                super(type, init);
                this.data = (init && init.data !== undefined) ? init.data : null;
                this.origin = (init && init.origin) || '';
                this.lastEventId = (init && init.lastEventId) || '';
                this.source = (init && init.source) || null;
                this.ports = (init && init.ports) || [];
            }
        };
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
