(function() {
    'use strict';

    var CONNECTING = 0;
    var OPEN = 1;
    var CLOSED = 2;

    function EventSource(url, options) {
        if (!(this instanceof EventSource))
            throw new TypeError("Failed to construct 'EventSource': use 'new'");
        if (!url) throw new SyntaxError("Failed to construct 'EventSource': URL required");

        this.url = String(url);
        this.withCredentials = (options && options.withCredentials) ? true : false;
        this.readyState = CONNECTING;

        this._lastEventId = '';
        this._retryMs = 3000;
        this._listeners = {};
        this._onopen = null;
        this._onmessage = null;
        this._onerror = null;
        this._abortController = null;
        this._closed = false;

        this._connect();
    }

    EventSource.CONNECTING = CONNECTING;
    EventSource.OPEN = OPEN;
    EventSource.CLOSED = CLOSED;

    Object.defineProperties(EventSource.prototype, {
        onopen: {
            get: function() { return this._onopen; },
            set: function(fn) { this._onopen = typeof fn === 'function' ? fn : null; }
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

    EventSource.prototype.addEventListener = function(type, listener) {
        if (typeof listener !== 'function') return;
        if (!this._listeners[type]) this._listeners[type] = [];
        this._listeners[type].push(listener);
    };

    EventSource.prototype.removeEventListener = function(type, listener) {
        var list = this._listeners[type];
        if (!list) return;
        for (var i = list.length - 1; i >= 0; i--) {
            if (list[i] === listener) { list.splice(i, 1); break; }
        }
    };

    EventSource.prototype.dispatchEvent = function(event) {
        var list = this._listeners[event.type];
        if (list) {
            for (var i = 0; i < list.length; i++) {
                try { list[i].call(this, event); } catch (e) { /* swallow */ }
            }
        }
    };

    EventSource.prototype.close = function() {
        this._closed = true;
        this.readyState = CLOSED;
        if (this._abortController) {
            this._abortController.abort();
            this._abortController = null;
        }
    };

    EventSource.prototype._connect = function() {
        if (this._closed) return;

        var self = this;
        this.readyState = CONNECTING;

        this._abortController = new AbortController();
        var headers = { 'Accept': 'text/event-stream', 'Cache-Control': 'no-cache' };
        if (this._lastEventId) {
            headers['Last-Event-ID'] = this._lastEventId;
        }

        fetch(this.url, {
            headers: headers,
            signal: this._abortController.signal
        }).then(function(response) {
            if (self._closed) return;

            if (!response.ok) {
                self._failAndReconnect();
                return;
            }

            self.readyState = OPEN;
            var openEvent = { type: 'open' };
            if (self._onopen) { try { self._onopen(openEvent); } catch (e) {} }
            self.dispatchEvent(openEvent);

            self._readStream(response.body);
        }).catch(function(err) {
            if (self._closed) return;
            self._failAndReconnect();
        });
    };

    EventSource.prototype._failAndReconnect = function() {
        if (this._closed) return;
        this.readyState = CONNECTING;

        var errorEvent = { type: 'error' };
        if (this._onerror) { try { this._onerror(errorEvent); } catch (e) {} }
        this.dispatchEvent(errorEvent);

        if (this._closed) return;

        var self = this;
        setTimeout(function() {
            if (!self._closed) self._connect();
        }, self._retryMs);
    };

    EventSource.prototype._readStream = function(body) {
        if (!body || this._closed) return;

        var self = this;
        var reader = body.pipeThrough(new TextDecoderStream()).getReader();
        var buf = '';

        function pump() {
            reader.read().then(function(result) {
                if (self._closed) {
                    reader.releaseLock();
                    return;
                }
                if (result.done) {
                    // Process any remaining buffer
                    if (buf.length > 0) self._processChunk(buf + '\n\n');
                    // Stream ended — reconnect per spec
                    self._failAndReconnect();
                    return;
                }
                buf += result.value;
                // Process complete events (separated by blank lines)
                var parts = buf.split('\n\n');
                // Last element is incomplete — keep in buffer
                buf = parts.pop();
                for (var i = 0; i < parts.length; i++) {
                    if (parts[i].length > 0) self._processEvent(parts[i]);
                }
                pump();
            }).catch(function(err) {
                if (self._closed) return;
                self._failAndReconnect();
            });
        }

        pump();
    };

    EventSource.prototype._processEvent = function(block) {
        var eventType = '';
        var data = [];
        var lines = block.split('\n');

        for (var i = 0; i < lines.length; i++) {
            var line = lines[i];

            // Comment lines (starting with :) are ignored
            if (line.charAt(0) === ':') continue;

            // Empty line should not appear here (we split on \n\n)
            if (line === '') continue;

            var colonIdx = line.indexOf(':');
            var field, value;
            if (colonIdx === -1) {
                field = line;
                value = '';
            } else {
                field = line.substring(0, colonIdx);
                value = line.substring(colonIdx + 1);
                // Strip single leading space after colon
                if (value.charAt(0) === ' ') value = value.substring(1);
            }

            switch (field) {
                case 'event':
                    eventType = value;
                    break;
                case 'data':
                    data.push(value);
                    break;
                case 'id':
                    // Per spec, ignore id fields containing null
                    if (value.indexOf('\0') === -1) {
                        this._lastEventId = value;
                    }
                    break;
                case 'retry':
                    var retry = parseInt(value, 10);
                    if (!isNaN(retry) && retry >= 0) {
                        this._retryMs = retry;
                    }
                    break;
                // Unknown fields are ignored per spec
            }
        }

        // If no data lines, don't dispatch
        if (data.length === 0) return;

        var type = eventType || 'message';
        var event = {
            type: type,
            data: data.join('\n'),
            lastEventId: this._lastEventId,
            origin: this.url
        };

        if (type === 'message' && this._onmessage) {
            try { this._onmessage(event); } catch (e) {}
        }
        this.dispatchEvent(event);
    };

    // Expose
    globalThis.EventSource = EventSource;
})();
