(function() {
    // -----------------------------------------------------------------------
    // Headers
    // -----------------------------------------------------------------------
    function Headers(init) {
        this._map = {};  // lowercase name -> [value, ...]
        if (init instanceof Headers) {
            var self = this;
            init.forEach(function(v, k) { self.append(k, v); });
        } else if (Array.isArray(init)) {
            for (var i = 0; i < init.length; i++) {
                this.append(init[i][0], init[i][1]);
            }
        } else if (init && typeof init === 'object') {
            var keys = Object.keys(init);
            for (var i = 0; i < keys.length; i++) {
                this.append(keys[i], init[keys[i]]);
            }
        }
    }

    Headers.prototype.append = function(name, value) {
        var key = String(name).toLowerCase();
        var val = String(value);
        if (!this._map[key]) this._map[key] = [];
        this._map[key].push(val);
    };

    Headers.prototype.delete = function(name) {
        delete this._map[String(name).toLowerCase()];
    };

    Headers.prototype.get = function(name) {
        var arr = this._map[String(name).toLowerCase()];
        return arr ? arr.join(', ') : null;
    };

    Headers.prototype.has = function(name) {
        return String(name).toLowerCase() in this._map;
    };

    Headers.prototype.set = function(name, value) {
        this._map[String(name).toLowerCase()] = [String(value)];
    };

    Headers.prototype.forEach = function(callback, thisArg) {
        var keys = Object.keys(this._map);
        for (var i = 0; i < keys.length; i++) {
            callback.call(thisArg, this._map[keys[i]].join(', '), keys[i], this);
        }
    };

    Headers.prototype.entries = function() {
        var keys = Object.keys(this._map);
        var map = this._map;
        var idx = 0;
        return {
            next: function() {
                if (idx >= keys.length) return { done: true, value: undefined };
                var k = keys[idx++];
                return { done: false, value: [k, map[k].join(', ')] };
            },
            [Symbol.iterator]: function() { return this; }
        };
    };

    Headers.prototype.keys = function() {
        var keys = Object.keys(this._map);
        var idx = 0;
        return {
            next: function() {
                if (idx >= keys.length) return { done: true, value: undefined };
                return { done: false, value: keys[idx++] };
            },
            [Symbol.iterator]: function() { return this; }
        };
    };

    Headers.prototype.values = function() {
        var keys = Object.keys(this._map);
        var map = this._map;
        var idx = 0;
        return {
            next: function() {
                if (idx >= keys.length) return { done: true, value: undefined };
                return { done: false, value: map[keys[idx++]].join(', ') };
            },
            [Symbol.iterator]: function() { return this; }
        };
    };

    Headers.prototype[Symbol.iterator] = Headers.prototype.entries;
    Headers.prototype[Symbol.toStringTag] = 'Headers';

    // Internal: convert Headers to plain object for native fetch
    Headers.prototype._toObject = function() {
        var obj = {};
        var keys = Object.keys(this._map);
        for (var i = 0; i < keys.length; i++) {
            obj[keys[i]] = this._map[keys[i]].join(', ');
        }
        return obj;
    };

    globalThis.Headers = Headers;

    // -----------------------------------------------------------------------
    // Response
    // -----------------------------------------------------------------------
    function Response(body, init) {
        init = init || {};
        this.status = init.status !== undefined ? init.status : 200;
        this.statusText = init.statusText !== undefined ? init.statusText : '';
        this.ok = this.status >= 200 && this.status < 300;
        this.headers = init.headers instanceof Headers
            ? init.headers : new Headers(init.headers);
        this.url = init.url || '';
        this.bodyUsed = false;
        this._body = body !== undefined && body !== null ? body : null;

        // Eagerly encode body to bytes for consistency
        if (typeof this._body === 'string') {
            this._bodyBytes = new TextEncoder().encode(this._body);
        } else if (this._body instanceof ArrayBuffer) {
            this._bodyBytes = new Uint8Array(this._body);
        } else if (this._body instanceof Uint8Array) {
            this._bodyBytes = this._body;
        } else if (this._body instanceof Blob) {
            // Deferred — use blob's async methods
            this._bodyBlob = this._body;
            this._bodyBytes = null;
        } else if (this._body === null) {
            this._bodyBytes = new Uint8Array(0);
        }

        this.body = null; // ReadableStream not wired for constructed responses
    }

    Response.prototype.text = function() {
        if (this.bodyUsed) return Promise.reject(new TypeError('Body already consumed'));
        this.bodyUsed = true;
        if (this._bodyBlob) return this._bodyBlob.text();
        return Promise.resolve(new TextDecoder().decode(this._bodyBytes));
    };

    Response.prototype.json = function() {
        return this.text().then(function(t) { return JSON.parse(t); });
    };

    Response.prototype.arrayBuffer = function() {
        if (this.bodyUsed) return Promise.reject(new TypeError('Body already consumed'));
        this.bodyUsed = true;
        if (this._bodyBlob) return this._bodyBlob.arrayBuffer();
        return Promise.resolve(this._bodyBytes.buffer.slice(
            this._bodyBytes.byteOffset,
            this._bodyBytes.byteOffset + this._bodyBytes.byteLength));
    };

    Response.prototype.blob = function() {
        if (this.bodyUsed) return Promise.reject(new TypeError('Body already consumed'));
        this.bodyUsed = true;
        var ct = this.headers.get('content-type') || '';
        if (this._bodyBlob) return Promise.resolve(this._bodyBlob);
        return Promise.resolve(new Blob([this._bodyBytes], { type: ct }));
    };

    Response.prototype.clone = function() {
        if (this.bodyUsed) throw new TypeError('Cannot clone a consumed response');
        var r = new Response(this._bodyBlob || this._bodyBytes, {
            status: this.status,
            statusText: this.statusText,
            headers: new Headers(this.headers)
        });
        r.url = this.url;
        r.ok = this.ok;
        return r;
    };

    Response.prototype[Symbol.toStringTag] = 'Response';

    Response.error = function() {
        var r = new Response(null, { status: 0, statusText: '' });
        r.ok = false;
        return r;
    };

    Response.redirect = function(url, status) {
        status = status || 302;
        return new Response(null, {
            status: status,
            headers: { 'location': url }
        });
    };

    Response.json = function(data, init) {
        init = init || {};
        var headers = new Headers(init.headers);
        if (!headers.has('content-type')) {
            headers.set('content-type', 'application/json');
        }
        return new Response(JSON.stringify(data), {
            status: init.status !== undefined ? init.status : 200,
            statusText: init.statusText || '',
            headers: headers
        });
    };

    globalThis.Response = Response;

    // -----------------------------------------------------------------------
    // Request
    // -----------------------------------------------------------------------
    function Request(input, init) {
        init = init || {};

        if (input instanceof Request) {
            this.url = init.url || input.url;
            this.method = (init.method || input.method || 'GET').toUpperCase();
            this.headers = new Headers(init.headers || input.headers);
            this._body = init.body !== undefined ? init.body : input._body;
        } else {
            this.url = String(input);
            this.method = (init.method || 'GET').toUpperCase();
            this.headers = new Headers(init.headers);
            this._body = init.body !== undefined ? init.body : null;
        }

        this.signal = init.signal || null;
        this.bodyUsed = false;
    }

    Request.prototype.clone = function() {
        if (this.bodyUsed) throw new TypeError('Cannot clone a consumed request');
        return new Request(this, {});
    };

    Request.prototype[Symbol.toStringTag] = 'Request';

    globalThis.Request = Request;

    // -----------------------------------------------------------------------
    // FormData multipart serialization
    // -----------------------------------------------------------------------

    // Generate a random boundary string
    function generateBoundary() {
        var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
        var boundary = '----brokit';
        for (var i = 0; i < 24; i++) {
            boundary += chars.charAt(Math.floor(Math.random() * chars.length));
        }
        return boundary;
    }

    // Serialize FormData to multipart/form-data string
    // Returns { body: string, contentType: string }
    // For File/Blob entries, synchronously extracts bytes via the native
    // C++ blob storage (arrayBuffer resolves immediately).
    function serializeFormData(fd) {
        var boundary = generateBoundary();
        var parts = [];

        fd.forEach(function(value, name) {
            if (value instanceof File) {
                // Binary file — encode bytes as latin1 string for transport
                var header = '--' + boundary + '\r\n' +
                    'Content-Disposition: form-data; name="' + name + '"; filename="' + value.name + '"\r\n' +
                    'Content-Type: ' + (value.type || 'application/octet-stream') + '\r\n\r\n';
                parts.push({ header: header, file: value });
            } else if (value instanceof Blob) {
                var header = '--' + boundary + '\r\n' +
                    'Content-Disposition: form-data; name="' + name + '"; filename="blob"\r\n' +
                    'Content-Type: ' + (value.type || 'application/octet-stream') + '\r\n\r\n';
                parts.push({ header: header, file: value });
            } else {
                parts.push({
                    text: '--' + boundary + '\r\n' +
                        'Content-Disposition: form-data; name="' + name + '"\r\n\r\n' +
                        String(value) + '\r\n'
                });
            }
        });

        // If no blob/file entries, can return synchronously
        var hasFiles = parts.some(function(p) { return p.file; });
        var contentType = 'multipart/form-data; boundary=' + boundary;

        if (!hasFiles) {
            var body = '';
            for (var i = 0; i < parts.length; i++) body += parts[i].text;
            body += '--' + boundary + '--\r\n';
            return Promise.resolve({ body: body, contentType: contentType });
        }

        // Has files — need to await arrayBuffer() calls
        var promises = parts.map(function(part) {
            if (part.text) return Promise.resolve(part.text);
            return part.file.arrayBuffer().then(function(ab) {
                var bytes = new Uint8Array(ab);
                var str = '';
                for (var i = 0; i < bytes.length; i++) {
                    str += String.fromCharCode(bytes[i]);
                }
                return part.header + str + '\r\n';
            });
        });

        return Promise.all(promises).then(function(resolved) {
            return { body: resolved.join('') + '--' + boundary + '--\r\n', contentType: contentType };
        });
    }

    // -----------------------------------------------------------------------
    // Wrap native fetch to support Headers, Request, FormData body
    // -----------------------------------------------------------------------
    var nativeFetch = globalThis.fetch;

    globalThis.fetch = function(input, init) {
        var url, options;

        if (input instanceof Request) {
            url = input.url;
            options = {
                method: input.method,
                headers: input.headers._toObject(),
                body: input._body
            };
            // init overrides Request properties
            if (init) {
                if (init.method) options.method = init.method;
                if (init.headers) {
                    var h = init.headers instanceof Headers
                        ? init.headers : new Headers(init.headers);
                    options.headers = h._toObject();
                }
                if (init.body !== undefined) options.body = init.body;
                if (init.signal) options.signal = init.signal;
            }
        } else {
            url = String(input);
            options = {};
            if (init) {
                options.method = init.method;
                if (init.headers) {
                    var h = init.headers instanceof Headers
                        ? init.headers : new Headers(init.headers);
                    options.headers = h._toObject();
                } else {
                    options.headers = {};
                }
                options.body = init.body;
                if (init.signal) options.signal = init.signal;
            }
        }

        // Handle FormData body
        var body = options.body;
        if (body instanceof FormData) {
            if (!options.headers) options.headers = {};
            return serializeFormData(body).then(function(result) {
                options.body = result.body;
                // Set content-type with boundary (don't override if user set it)
                if (!options.headers['content-type'] && !options.headers['Content-Type']) {
                    options.headers['content-type'] = result.contentType;
                }
                if (!options.method) options.method = 'POST';
                return nativeFetch(url, options);
            }).then(wrapResponse);
        }

        // Handle Blob body
        if (body instanceof Blob) {
            if (!options.headers) options.headers = {};
            if (!options.headers['content-type'] && !options.headers['Content-Type'] && body.type) {
                options.headers['content-type'] = body.type;
            }
            return body.text().then(function(text) {
                options.body = text;
                if (!options.method) options.method = 'POST';
                return nativeFetch(url, options);
            }).then(wrapResponse);
        }

        // Handle ArrayBuffer / TypedArray body
        if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) {
            var bytes = body instanceof ArrayBuffer ? new Uint8Array(body) : new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
            var str = '';
            for (var i = 0; i < bytes.length; i++) str += String.fromCharCode(bytes[i]);
            options.body = str;
            if (!options.method) options.method = 'POST';
        }

        // Handle URLSearchParams body
        if (typeof URLSearchParams !== 'undefined' && body instanceof URLSearchParams) {
            options.body = body.toString();
            if (!options.headers) options.headers = {};
            if (!options.headers['content-type'] && !options.headers['Content-Type']) {
                options.headers['content-type'] = 'application/x-www-form-urlencoded;charset=UTF-8';
            }
            if (!options.method) options.method = 'POST';
        }

        return nativeFetch(url, options).then(wrapResponse);
    };

    // Wrap native response object with proper Headers instance
    function wrapResponse(resp) {
        if (resp instanceof Response) return resp;
        // Native fetch returns plain objects — wrap headers
        if (resp.headers && !(resp.headers instanceof Headers)) {
            var h = new Headers();
            if (typeof resp.headers.forEach === 'function') {
                resp.headers.forEach(function(v, k) { h.append(k, v); });
            }
            resp.headers = h;
        }
        return resp;
    }
})();
