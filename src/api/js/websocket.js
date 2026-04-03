(function() {
    'use strict';

    var CONNECTING = 0;
    var OPEN = 1;
    var CLOSING = 2;
    var CLOSED = 3;

    function WebSocket(url, protocols) {
        if (!(this instanceof WebSocket))
            throw new TypeError("Failed to construct 'WebSocket': use 'new'");
        if (!url) throw new SyntaxError("Failed to construct 'WebSocket': URL required");

        var urlStr = String(url);
        // Validate URL scheme
        if (urlStr.indexOf('ws://') !== 0 && urlStr.indexOf('wss://') !== 0) {
            throw new SyntaxError("Failed to construct 'WebSocket': URL must use ws:// or wss://");
        }

        this.url = urlStr;
        this.readyState = CONNECTING;
        this.bufferedAmount = 0;
        this.extensions = '';
        this.protocol = '';
        this.binaryType = 'blob'; // 'blob' or 'arraybuffer'

        this._onopen = null;
        this._onclose = null;
        this._onmessage = null;
        this._onerror = null;
        this._listeners = {};
        this._id = -1;
        this._pollTimer = null;

        // Normalize protocols
        var protoStr = '';
        if (protocols) {
            if (typeof protocols === 'string') {
                protoStr = protocols;
            } else if (Array.isArray(protocols)) {
                protoStr = protocols.join(', ');
            }
        }

        // Initiate native connection
        var self = this;
        var connResult = globalThis.__brokit_ws_connect(urlStr, protoStr);
        this._id = connResult.id;

        connResult.promise.then(function() {
            if (self.readyState === CONNECTING) {
                self.readyState = OPEN;
                var event = { type: 'open' };
                if (self._onopen) { try { self._onopen(event); } catch (e) {} }
                self._dispatch(event);
            }
        }).catch(function(err) {
            self._handleError(err.message || 'Connection failed');
            self.readyState = CLOSED;
            var closeEvent = {
                type: 'close',
                code: 1006,
                reason: err.message || '',
                wasClean: false
            };
            if (self._onclose) { try { self._onclose(closeEvent); } catch (e) {} }
            self._dispatch(closeEvent);
        });

        // Start polling for messages
        this._startPolling();
    }

    WebSocket.CONNECTING = CONNECTING;
    WebSocket.OPEN = OPEN;
    WebSocket.CLOSING = CLOSING;
    WebSocket.CLOSED = CLOSED;

    Object.defineProperties(WebSocket.prototype, {
        onopen: {
            get: function() { return this._onopen; },
            set: function(fn) { this._onopen = typeof fn === 'function' ? fn : null; }
        },
        onclose: {
            get: function() { return this._onclose; },
            set: function(fn) { this._onclose = typeof fn === 'function' ? fn : null; }
        },
        onmessage: {
            get: function() { return this._onmessage; },
            set: function(fn) { this._onmessage = typeof fn === 'function' ? fn : null; }
        },
        onerror: {
            get: function() { return this._onerror; },
            set: function(fn) { this._onerror = typeof fn === 'function' ? fn : null; }
        }
    });

    WebSocket.prototype.addEventListener = function(type, listener) {
        if (typeof listener !== 'function') return;
        if (!this._listeners[type]) this._listeners[type] = [];
        this._listeners[type].push(listener);
    };

    WebSocket.prototype.removeEventListener = function(type, listener) {
        var list = this._listeners[type];
        if (!list) return;
        for (var i = list.length - 1; i >= 0; i--) {
            if (list[i] === listener) { list.splice(i, 1); break; }
        }
    };

    WebSocket.prototype._dispatch = function(event) {
        var list = this._listeners[event.type];
        if (list) {
            for (var i = 0; i < list.length; i++) {
                try { list[i].call(this, event); } catch (e) {}
            }
        }
    };

    WebSocket.prototype.send = function(data) {
        if (this.readyState === CONNECTING) {
            throw new DOMException('WebSocket is not open', 'InvalidStateError');
        }
        if (this.readyState !== OPEN) return;

        var binary = false;
        if (data instanceof ArrayBuffer || data instanceof Uint8Array ||
            (typeof SharedArrayBuffer !== 'undefined' && data instanceof SharedArrayBuffer)) {
            binary = true;
        }

        globalThis.__brokit_ws_send(this._id, data, binary);
    };

    WebSocket.prototype.close = function(code, reason) {
        if (this.readyState === CLOSING || this.readyState === CLOSED) return;

        if (code !== undefined && code !== 1000 && (code < 3000 || code > 4999)) {
            throw new DOMException('Invalid close code', 'InvalidAccessError');
        }

        var wasConnecting = (this.readyState === CONNECTING);
        this.readyState = CLOSING;
        globalThis.__brokit_ws_close(this._id, code, reason);

        // If we were still connecting, native layer cleaned up immediately
        if (wasConnecting) {
            this.readyState = CLOSED;
            this._id = -1;
        }
    };

    WebSocket.prototype._handleError = function(msg) {
        var event = { type: 'error', message: msg };
        if (this._onerror) { try { this._onerror(event); } catch (e) {} }
        this._dispatch(event);
    };

    WebSocket.prototype._drainEvents = function() {
        if (this.readyState === CLOSED && this._id === -1) return;

        var msg;
        while ((msg = globalThis.__brokit_ws_recv(this._id)) !== null) {
            if (msg.type === 'message') {
                var msgData = msg.data;
                if (msg.binary && this.binaryType === 'arraybuffer' &&
                    msgData instanceof Uint8Array) {
                    msgData = msgData.buffer;
                }
                var event = { type: 'message', data: msgData };
                if (this._onmessage) { try { this._onmessage(event); } catch (e) {} }
                this._dispatch(event);
            } else if (msg.type === 'error') {
                this._handleError(msg.data);
            } else if (msg.type === 'close') {
                this.readyState = CLOSED;
                this._id = -1;
                var closeEvent = {
                    type: 'close',
                    code: msg.code || 1000,
                    reason: msg.reason || '',
                    wasClean: (msg.code === 1000 || msg.code === 1005)
                };
                if (this._onclose) { try { this._onclose(closeEvent); } catch (e) {} }
                this._dispatch(closeEvent);
                return;
            }
        }
    };

    // Register this instance for tick-driven event delivery
    WebSocket.prototype._startPolling = function() {
        _instances.push(this);
    };

    // ── Instance registry for tick-driven event delivery ────────────────────
    var _instances = [];

    // Called by the native ws_tick to deliver events to all active instances
    globalThis.__brokit_ws_drain_all = function() {
        for (var i = _instances.length - 1; i >= 0; i--) {
            var inst = _instances[i];
            if (inst.readyState === CLOSED && inst._id === -1) {
                _instances.splice(i, 1);
                continue;
            }
            inst._drainEvents();
        }
    };

    // Expose
    globalThis.WebSocket = WebSocket;
})();
