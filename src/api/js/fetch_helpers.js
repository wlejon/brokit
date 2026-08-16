// fetch_helpers.js — JS polyfill factories used by fetch.cpp.
//
// These are pulled out of the C++ source so they can be edited as JS, not as
// raw string literals. The C++ side evaluates this file once per context
// (during installFetch) and looks up `__brokit_fetch_internals.<fn>` when it
// needs to decorate a Response.

(function () {
    var internals = {};

    // Build a Headers-like object from a plain {name: value} dictionary.
    internals.headers = function (entries) {
        var obj = {
            get: function (name) { return entries[name.toLowerCase()] || null; },
            has: function (name) { return entries[name.toLowerCase()] !== undefined; },
            forEach: function (cb) {
                var keys = Object.keys(entries);
                for (var i = 0; i < keys.length; i++) cb(entries[keys[i]], keys[i], this);
            },
            entries: function () {
                var keys = Object.keys(entries); var i = 0;
                return { next: function () {
                    if (i >= keys.length) return { done: true };
                    var k = keys[i++]; return { done: false, value: [k, entries[k]] };
                }, [Symbol.iterator]: function () { return this; } };
            },
            keys: function () {
                var keys = Object.keys(entries); var i = 0;
                return { next: function () {
                    if (i >= keys.length) return { done: true };
                    return { done: false, value: keys[i++] };
                }, [Symbol.iterator]: function () { return this; } };
            },
            values: function () {
                var keys = Object.keys(entries); var i = 0;
                return { next: function () {
                    if (i >= keys.length) return { done: true };
                    return { done: false, value: entries[keys[i++]] };
                }, [Symbol.iterator]: function () { return this; } };
            }
        };
        obj[Symbol.iterator] = obj.entries;
        return obj;
    };

    // A ReadableStream that hands out one already-complete ArrayBuffer and
    // closes. `get` is called at pull time so the body can be attached to the
    // response after the stream is built.
    function replayStream(get) {
        if (typeof ReadableStream !== 'function') return null;
        var emitted = false;
        return new ReadableStream({
            pull: function (controller) {
                if (emitted) { controller.close(); return; }
                emitted = true;
                var buf = get();
                controller.enqueue(new Uint8Array(buf ? buf.slice(0) : 0));
            }
        });
    }

    // Decorate a Response object that represents a missing local file (404).
    internals.applyNotFoundBody = function (resp) {
        resp.bodyUsed = false;
        resp.body = null;
        resp.text = function () { return Promise.resolve(''); };
        resp.json = function () { return Promise.reject(new SyntaxError('Not Found')); };
        resp.arrayBuffer = function () { return Promise.resolve(new ArrayBuffer(0)); };
        resp.blob = function () { return Promise.resolve(new Blob([])); };
        resp.clone = function () { return Object.assign(Object.create(null), resp); };
    };

    // Decorate a Response object backed by a fully-loaded local file body
    // (already attached as `resp.__body`, an ArrayBuffer).
    internals.applyFileBody = function (resp) {
        resp.bodyUsed = false;
        // `body` is a ReadableStream over the bytes, not null. A response that
        // HAS a body and reports `body === null` reads as "bodyless" to any
        // caller that feature-detects streaming — three.js's FileLoader does
        // exactly that (`response.body.getReader === undefined`) and throws on
        // the null instead, so every loader built on it failed on local files.
        resp.body = replayStream(function () { return resp.__body; });
        resp.text = function () {
            resp.bodyUsed = true;
            return Promise.resolve(new TextDecoder().decode(new Uint8Array(this.__body)));
        };
        resp.json = function () {
            return this.text().then(function (t) { return JSON.parse(t); });
        };
        resp.arrayBuffer = function () {
            resp.bodyUsed = true;
            return Promise.resolve(this.__body.slice(0));
        };
        resp.blob = function () {
            var ct = resp.headers.get('content-type') || '';
            resp.bodyUsed = true;
            return Promise.resolve(new Blob([new Uint8Array(this.__body)], { type: ct }));
        };
        resp.clone = function () {
            var r = Object.assign(Object.create(null), this);
            r.__body = this.__body.slice(0);
            return r;
        };
    };

    // Decorate a Response object whose body is still streaming from native.
    // Pulls chunks from `__brokit_fetch_stream_read(streamId)` and uses
    // `__brokit_fetch_stream_wait` to suspend until more data arrives.
    internals.applyStreamingBody = function (resp) {
        var streamId = resp.__streamId;

        resp.body = new ReadableStream({
            pull: function (controller) {
                return new Promise(function (resolve) {
                    function tryRead() {
                        var result = globalThis.__brokit_fetch_stream_read(streamId);
                        if (result === null) {
                            globalThis.__brokit_fetch_stream_wait(streamId, function () {
                                tryRead();
                            });
                            return;
                        }
                        if (result.done) {
                            controller.close();
                            resolve();
                            return;
                        }
                        controller.enqueue(result.value);
                        resolve();
                    }
                    tryRead();
                });
            },
            cancel: function () {
                // Best effort — data already in flight from native.
            }
        });
        resp.bodyUsed = false;

        function consumeBody() {
            if (resp.bodyUsed) return Promise.reject(new TypeError('Body already consumed'));
            resp.bodyUsed = true;
            var reader = resp.body.getReader();
            var chunks = [];
            function pump() {
                return reader.read().then(function (result) {
                    if (result.done) {
                        var totalLen = 0;
                        for (var i = 0; i < chunks.length; i++) totalLen += chunks[i].byteLength;
                        var merged = new Uint8Array(totalLen);
                        var offset = 0;
                        for (var i = 0; i < chunks.length; i++) {
                            merged.set(new Uint8Array(chunks[i].buffer || chunks[i]), offset);
                            offset += chunks[i].byteLength;
                        }
                        return merged;
                    }
                    chunks.push(result.value);
                    return pump();
                });
            }
            return pump();
        }

        resp.text = function () {
            return consumeBody().then(function (bytes) {
                return new TextDecoder().decode(bytes);
            });
        };
        resp.json = function () {
            return resp.text().then(function (t) { return JSON.parse(t); });
        };
        resp.arrayBuffer = function () {
            return consumeBody().then(function (bytes) { return bytes.buffer; });
        };
        resp.blob = function () {
            var ct = resp.headers.get('content-type') || '';
            return consumeBody().then(function (bytes) {
                return new Blob([bytes], { type: ct });
            });
        };
        resp.clone = function () {
            throw new TypeError('Cannot clone a streaming response');
        };
    };

    // Decorate a Response whose body has already fully arrived (`resp.__body`
    // is the complete ArrayBuffer). The `.body` ReadableStream replays the
    // buffer once, so no native chunk queue is required.
    internals.applyCompleteBody = function (resp) {
        resp.bodyUsed = false;

        var bodyBuf = resp.__body;
        var emitted = false;
        resp.body = new ReadableStream({
            pull: function (controller) {
                if (emitted) { controller.close(); return; }
                emitted = true;
                controller.enqueue(new Uint8Array(bodyBuf.slice(0)));
            }
        });

        resp.text = function () {
            resp.bodyUsed = true;
            var decoder = new TextDecoder();
            var text = decoder.decode(new Uint8Array(this.__body));
            return Promise.resolve(text);
        };
        resp.json = function () {
            return this.text().then(function (t) { return JSON.parse(t); });
        };
        resp.arrayBuffer = function () {
            resp.bodyUsed = true;
            return Promise.resolve(this.__body.slice(0));
        };
        resp.blob = function () {
            var ct = this.headers.get('content-type') || '';
            resp.bodyUsed = true;
            return Promise.resolve(new Blob([new Uint8Array(this.__body)], { type: ct }));
        };
        resp.clone = function () {
            var r = Object.create(Object.getPrototypeOf(this));
            Object.assign(r, this);
            r.__body = this.__body.slice(0);
            return r;
        };
    };

    globalThis.__brokit_fetch_internals = internals;
})();
