// XMLHttpRequest over fetch.
//
// One request is one call to the wrapped `globalThis.fetch` (the one
// fetch_classes.js installs, which understands Blob, FormData and typed-array
// bodies), driven by an AbortController so abort() and `timeout` cancel the
// transfer rather than just ignoring its result. There is no synchronous
// fetch, so open(..., false) throws InvalidAccessError.
//
// Events are the platform `Event`, created when they fire, with the
// ProgressEvent fields (lengthComputable / loaded / total) set on the load,
// progress, abort, error, timeout and loadend events. Listener storage lives
// here rather than on EventTarget.prototype because this installs before
// event_target.js does; `on<type>` handlers run first, then listeners in
// registration order, and every event carries `target`/`currentTarget`.
//
// Response bodies are read whole (the fetch Response is drained to bytes), so
// readyState 3 (LOADING) is entered and left in the same turn; `progress`
// reports the full length once.

(function() {
    var UNSENT = 0, OPENED = 1, HEADERS_RECEIVED = 2, LOADING = 3, DONE = 4;

    var RESPONSE_TYPES = ['', 'text', 'json', 'arraybuffer', 'blob'];

    function domException(message, name) {
        return new globalThis.DOMException(message, name);
    }

    // -----------------------------------------------------------------------
    // XMLHttpRequestEventTarget: listeners + on<type> handler slots
    // -----------------------------------------------------------------------
    function XMLHttpRequestEventTarget() {
        this._listeners = {};
        this._handlers = {};
    }

    XMLHttpRequestEventTarget.prototype.addEventListener = function(type, listener, options) {
        var callable = typeof listener === 'function' ||
            (listener && typeof listener.handleEvent === 'function');
        if (!callable) return;
        var once = !!(options && typeof options === 'object' && options.once);
        if (!this._listeners[type]) this._listeners[type] = [];
        var list = this._listeners[type];
        for (var i = 0; i < list.length; i++) {
            if (list[i].listener === listener) return;
        }
        list.push({ listener: listener, once: once });
    };

    XMLHttpRequestEventTarget.prototype.removeEventListener = function(type, listener) {
        var list = this._listeners[type];
        if (!list) return;
        for (var i = 0; i < list.length; i++) {
            if (list[i].listener === listener) {
                list.splice(i, 1);
                return;
            }
        }
    };

    XMLHttpRequestEventTarget.prototype.dispatchEvent = function(event) {
        event.target = this;
        event.currentTarget = this;
        var handler = this._handlers[event.type];
        if (typeof handler === 'function') {
            handler.call(this, event);
        }
        var list = this._listeners[event.type];
        if (list) {
            var handlers = list.slice();
            for (var i = 0; i < handlers.length; i++) {
                if (event._stopImmediate) break;
                var entry = handlers[i];
                if (entry.once) this.removeEventListener(event.type, entry.listener);
                if (typeof entry.listener === 'function') {
                    entry.listener.call(this, event);
                } else {
                    entry.listener.handleEvent(event);
                }
            }
        }
        return !event.defaultPrevented;
    };

    function defineHandler(proto, type) {
        Object.defineProperty(proto, 'on' + type, {
            get: function() { return this._handlers[type] || null; },
            set: function(fn) { this._handlers[type] = (typeof fn === 'function') ? fn : null; },
            enumerable: true,
            configurable: true
        });
    }

    var PROGRESS_HANDLERS = ['loadstart', 'progress', 'abort', 'error', 'load', 'timeout', 'loadend'];
    for (var h = 0; h < PROGRESS_HANDLERS.length; h++) {
        defineHandler(XMLHttpRequestEventTarget.prototype, PROGRESS_HANDLERS[h]);
    }

    function fireProgress(target, type, loaded, total) {
        var event = new globalThis.Event(type);
        event.lengthComputable = total > 0;
        event.loaded = loaded;
        event.total = total;
        target.dispatchEvent(event);
    }

    // -----------------------------------------------------------------------
    // XMLHttpRequestUpload
    // -----------------------------------------------------------------------
    function XMLHttpRequestUpload() {
        XMLHttpRequestEventTarget.call(this);
    }
    XMLHttpRequestUpload.prototype = Object.create(XMLHttpRequestEventTarget.prototype);
    XMLHttpRequestUpload.prototype.constructor = XMLHttpRequestUpload;

    // -----------------------------------------------------------------------
    // XMLHttpRequest
    // -----------------------------------------------------------------------
    function XMLHttpRequest() {
        XMLHttpRequestEventTarget.call(this);
        this.upload = new XMLHttpRequestUpload();
        this.timeout = 0;
        this.withCredentials = false;

        this._readyState = UNSENT;
        this._responseType = '';
        this._overrideMime = null;
        this._method = 'GET';
        this._url = '';
        this._requestHeaders = {};
        this._sendFlag = false;
        this._hasBody = false;
        this._controller = null;
        this._timer = null;
        // Bumped by open(), abort() and every terminal step, so a fetch that
        // settles for a request that has since been reopened or aborted is
        // recognised and ignored.
        this._seq = 0;
        this._resetResponse();
    }
    XMLHttpRequest.prototype = Object.create(XMLHttpRequestEventTarget.prototype);
    XMLHttpRequest.prototype.constructor = XMLHttpRequest;
    defineHandler(XMLHttpRequest.prototype, 'readystatechange');

    XMLHttpRequest.UNSENT = XMLHttpRequest.prototype.UNSENT = UNSENT;
    XMLHttpRequest.OPENED = XMLHttpRequest.prototype.OPENED = OPENED;
    XMLHttpRequest.HEADERS_RECEIVED = XMLHttpRequest.prototype.HEADERS_RECEIVED = HEADERS_RECEIVED;
    XMLHttpRequest.LOADING = XMLHttpRequest.prototype.LOADING = LOADING;
    XMLHttpRequest.DONE = XMLHttpRequest.prototype.DONE = DONE;

    XMLHttpRequest.prototype._resetResponse = function() {
        this._status = 0;
        this._statusText = '';
        this._responseURL = '';
        this._responseHeaders = null;
        this._responseBytes = null;
        this._responseText = null;
        this._responseValue = undefined;
    };

    XMLHttpRequest.prototype._setState = function(state) {
        this._readyState = state;
        this.dispatchEvent(new globalThis.Event('readystatechange'));
    };

    XMLHttpRequest.prototype._clearTimer = function() {
        if (this._timer !== null) {
            clearTimeout(this._timer);
            this._timer = null;
        }
    };

    // The request error steps: state DONE with a network error, then the
    // `type` event and loadend on the upload (if a body was being sent) and on
    // the request itself.
    XMLHttpRequest.prototype._requestError = function(type) {
        this._seq++;
        this._clearTimer();
        var controller = this._controller;
        this._controller = null;
        if (controller) controller.abort();

        this._sendFlag = false;
        this._resetResponse();
        this._setState(DONE);
        if (this._hasBody) {
            fireProgress(this.upload, type, 0, 0);
            fireProgress(this.upload, 'loadend', 0, 0);
        }
        fireProgress(this, type, 0, 0);
        fireProgress(this, 'loadend', 0, 0);
    };

    Object.defineProperties(XMLHttpRequest.prototype, {
        readyState: { get: function() { return this._readyState; }, enumerable: true, configurable: true },
        status: { get: function() { return this._status; }, enumerable: true, configurable: true },
        statusText: { get: function() { return this._statusText; }, enumerable: true, configurable: true },
        responseURL: { get: function() { return this._responseURL; }, enumerable: true, configurable: true },

        responseType: {
            get: function() { return this._responseType; },
            set: function(value) {
                if (this._readyState === LOADING || this._readyState === DONE) {
                    throw domException('responseType cannot be changed once the response is loading.', 'InvalidStateError');
                }
                value = String(value);
                if (RESPONSE_TYPES.indexOf(value) === -1) {
                    throw new TypeError("Unsupported responseType '" + value + "'");
                }
                this._responseType = value;
            },
            enumerable: true,
            configurable: true
        },

        responseText: {
            get: function() {
                if (this._responseType !== '' && this._responseType !== 'text') {
                    throw domException("responseText is only available when responseType is '' or 'text'.", 'InvalidStateError');
                }
                if (this._readyState !== LOADING && this._readyState !== DONE) return '';
                return this._text();
            },
            enumerable: true,
            configurable: true
        },

        response: {
            get: function() {
                var type = this._responseType;
                if (type === '' || type === 'text') {
                    if (this._readyState !== LOADING && this._readyState !== DONE) return '';
                    return this._text();
                }
                if (this._readyState !== DONE) return null;
                if (this._responseValue !== undefined) return this._responseValue;
                var bytes = this._responseBytes;
                var value = null;
                if (bytes !== null) {
                    if (type === 'arraybuffer') {
                        value = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
                    } else if (type === 'blob') {
                        value = new Blob([bytes], { type: this._responseMime() });
                    } else if (type === 'json') {
                        try {
                            value = JSON.parse(this._text());
                        } catch (e) {
                            value = null;
                        }
                    }
                }
                this._responseValue = value;
                return value;
            },
            enumerable: true,
            configurable: true
        }
    });

    XMLHttpRequest.prototype._text = function() {
        if (this._responseText === null) {
            this._responseText = this._responseBytes
                ? new TextDecoder().decode(this._responseBytes)
                : '';
        }
        return this._responseText;
    };

    XMLHttpRequest.prototype._responseMime = function() {
        if (this._overrideMime !== null) return this._overrideMime;
        var ct = this._responseHeaders ? this._responseHeaders.get('content-type') : null;
        return ct ? String(ct).toLowerCase() : '';
    };

    XMLHttpRequest.prototype.open = function(method, url, async, user, password) {
        method = String(method);
        if (/^(delete|get|head|options|post|put)$/i.test(method)) method = method.toUpperCase();
        if (arguments.length > 2 && async === false) {
            throw domException('Synchronous XMLHttpRequest is not supported: there is no synchronous fetch.', 'InvalidAccessError');
        }

        // Terminate whatever was in flight; its settlement is stale now.
        this._seq++;
        this._clearTimer();
        var controller = this._controller;
        this._controller = null;
        if (controller) controller.abort();

        this._method = method;
        this._url = String(url);
        this._requestHeaders = {};
        this._sendFlag = false;
        this._hasBody = false;
        this._resetResponse();
        if (this._readyState !== OPENED) this._setState(OPENED);
    };

    XMLHttpRequest.prototype.setRequestHeader = function(name, value) {
        if (this._readyState !== OPENED || this._sendFlag) {
            throw domException('setRequestHeader can only be called when the request is OPENED and not yet sent.', 'InvalidStateError');
        }
        var key = String(name).toLowerCase();
        value = String(value);
        if (Object.prototype.hasOwnProperty.call(this._requestHeaders, key)) {
            this._requestHeaders[key] += ', ' + value;
        } else {
            this._requestHeaders[key] = value;
        }
    };

    XMLHttpRequest.prototype.overrideMimeType = function(mime) {
        if (this._readyState === LOADING || this._readyState === DONE) {
            throw domException('overrideMimeType cannot be called once the response is loading.', 'InvalidStateError');
        }
        this._overrideMime = String(mime).toLowerCase();
    };

    XMLHttpRequest.prototype.send = function(body) {
        if (this._readyState !== OPENED || this._sendFlag) {
            throw domException('send can only be called once on an OPENED request.', 'InvalidStateError');
        }
        if (body === undefined || this._method === 'GET' || this._method === 'HEAD') body = null;

        var headers = {};
        var keys = Object.keys(this._requestHeaders);
        for (var i = 0; i < keys.length; i++) headers[keys[i]] = this._requestHeaders[keys[i]];
        if (body !== null && !headers['content-type']) {
            if (typeof body === 'string') {
                headers['content-type'] = 'text/plain;charset=UTF-8';
            } else if (body instanceof Blob && body.type) {
                headers['content-type'] = body.type;
            }
        }

        var self = this;
        var seq = ++this._seq;
        var controller = new AbortController();
        this._controller = controller;
        this._sendFlag = true;
        this._hasBody = body !== null;

        fireProgress(this, 'loadstart', 0, 0);
        if (this._hasBody) fireProgress(this.upload, 'loadstart', 0, 0);

        if (this.timeout > 0) {
            this._timer = setTimeout(function() {
                self._timer = null;
                if (self._seq !== seq) return;
                self._requestError('timeout');
            }, this.timeout);
        }

        var init = { method: this._method, headers: headers, signal: controller.signal };
        if (body !== null) init.body = body;

        globalThis.fetch(this._url, init).then(function(resp) {
            if (self._seq !== seq) return;
            if (self._hasBody) {
                fireProgress(self.upload, 'progress', 0, 0);
                fireProgress(self.upload, 'load', 0, 0);
                fireProgress(self.upload, 'loadend', 0, 0);
            }
            self._status = resp.status;
            self._statusText = resp.statusText || '';
            self._responseURL = resp.url || self._url;
            self._responseHeaders = resp.headers;
            self._setState(HEADERS_RECEIVED);
            if (self._seq !== seq) return;
            self._setState(LOADING);
            if (self._seq !== seq) return;
            return resp.arrayBuffer().then(function(buffer) {
                if (self._seq !== seq) return;
                self._seq++;
                self._clearTimer();
                self._controller = null;
                self._sendFlag = false;
                self._responseBytes = new Uint8Array(buffer);
                var length = self._responseBytes.length;
                self._setState(DONE);
                fireProgress(self, 'progress', length, length);
                fireProgress(self, 'load', length, length);
                fireProgress(self, 'loadend', length, length);
            });
        }).then(null, function(err) {
            if (self._seq !== seq) return;
            // An AbortError here is our own controller firing from abort()
            // or the timeout, both of which have already run the error steps.
            if (err && err.name === 'AbortError') return;
            self._requestError('error');
        });
    };

    XMLHttpRequest.prototype.abort = function() {
        var inFlight = this._sendFlag &&
            (this._readyState === OPENED || this._readyState === HEADERS_RECEIVED || this._readyState === LOADING);
        this._seq++;
        this._clearTimer();
        var controller = this._controller;
        this._controller = null;
        if (controller) controller.abort();
        if (inFlight) this._requestError('abort');
        if (this._readyState === DONE) this._readyState = UNSENT;
    };

    XMLHttpRequest.prototype.getResponseHeader = function(name) {
        if (!this._responseHeaders) return null;
        var value = this._responseHeaders.get(String(name));
        return (value === undefined || value === null) ? null : String(value);
    };

    XMLHttpRequest.prototype.getAllResponseHeaders = function() {
        if (!this._responseHeaders) return '';
        var lines = [];
        this._responseHeaders.forEach(function(value, key) {
            lines.push(String(key).toLowerCase() + ': ' + value);
        });
        lines.sort();
        return lines.length ? lines.join('\r\n') + '\r\n' : '';
    };

    XMLHttpRequest.prototype[Symbol.toStringTag] = 'XMLHttpRequest';
    XMLHttpRequestUpload.prototype[Symbol.toStringTag] = 'XMLHttpRequestUpload';

    globalThis.XMLHttpRequestEventTarget = XMLHttpRequestEventTarget;
    globalThis.XMLHttpRequestUpload = XMLHttpRequestUpload;
    globalThis.XMLHttpRequest = XMLHttpRequest;
})();
