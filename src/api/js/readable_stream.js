(function() {
    'use strict';

    // ── ReadableStreamDefaultController ───────────────────────────────────────

    function ReadableStreamDefaultController(stream) {
        this._stream = stream;
        this._closeRequested = false;
    }

    ReadableStreamDefaultController.prototype.enqueue = function(chunk) {
        if (this._closeRequested) throw new TypeError('Controller is closed');
        if (this._stream._state !== 'readable') return;
        var stream = this._stream;

        // If a reader has a pending read, resolve it directly
        if (stream._reader && stream._reader._pendingReads.length > 0) {
            var pending = stream._reader._pendingReads.shift();
            pending.resolve({ value: chunk, done: false });
            return;
        }

        // Otherwise queue it
        stream._queue.push(chunk);
    };

    ReadableStreamDefaultController.prototype.close = function() {
        if (this._closeRequested) return;
        this._closeRequested = true;
        var stream = this._stream;
        stream._state = 'closed';

        // Resolve any pending reads with done
        if (stream._reader) {
            var pending = stream._reader._pendingReads;
            while (pending.length > 0) {
                var p = pending.shift();
                p.resolve({ value: undefined, done: true });
            }
            // Resolve closed promise
            if (stream._reader._closedResolve) {
                stream._reader._closedResolve();
                stream._reader._closedResolve = null;
            }
        }
    };

    ReadableStreamDefaultController.prototype.error = function(e) {
        var stream = this._stream;
        if (stream._state !== 'readable') return;
        stream._state = 'errored';
        stream._storedError = e;

        // Reject any pending reads
        if (stream._reader) {
            var pending = stream._reader._pendingReads;
            while (pending.length > 0) {
                var p = pending.shift();
                p.reject(e);
            }
            // Reject closed promise
            if (stream._reader._closedReject) {
                stream._reader._closedReject(e);
                stream._reader._closedReject = null;
            }
        }
    };

    Object.defineProperty(ReadableStreamDefaultController.prototype, 'desiredSize', {
        get: function() {
            if (this._stream._state !== 'readable') return null;
            return 1 - this._stream._queue.length;
        }
    });

    // ── ReadableStreamDefaultReader ───────────────────────────────────────────

    function ReadableStreamDefaultReader(stream) {
        if (stream._locked) throw new TypeError('Stream is already locked');
        this._stream = stream;
        this._pendingReads = [];
        this._closedResolve = null;
        this._closedReject = null;
        stream._reader = this;
        stream._locked = true;

        // Set up closed promise
        var self = this;
        this._closedPromise = new Promise(function(resolve, reject) {
            if (stream._state === 'closed') { resolve(); return; }
            if (stream._state === 'errored') { reject(stream._storedError); return; }
            self._closedResolve = resolve;
            self._closedReject = reject;
        });
        // The spec stores closed with [[PromiseIsHandled]] = true: a stream
        // erroring must not fire an unhandled rejection just because nobody
        // awaited reader.closed. Callers who do await still see it.
        this._closedPromise.then(null, function() {});
    }

    Object.defineProperty(ReadableStreamDefaultReader.prototype, 'closed', {
        get: function() { return this._closedPromise; }
    });

    ReadableStreamDefaultReader.prototype.read = function() {
        var stream = this._stream;
        if (!stream) return Promise.reject(new TypeError('Reader has been released'));

        // If data in queue, return it
        if (stream._queue.length > 0) {
            var chunk = stream._queue.shift();
            // Trigger pull if source wants to produce more
            stream._pullIfNeeded();
            return Promise.resolve({ value: chunk, done: false });
        }

        // If closed, return done
        if (stream._state === 'closed') {
            return Promise.resolve({ value: undefined, done: true });
        }

        // If errored, reject
        if (stream._state === 'errored') {
            return Promise.reject(stream._storedError);
        }

        // No data — enqueue a pending read and trigger pull
        var self = this;
        return new Promise(function(resolve, reject) {
            self._pendingReads.push({ resolve: resolve, reject: reject });
            stream._pullIfNeeded();
        });
    };

    ReadableStreamDefaultReader.prototype.releaseLock = function() {
        if (!this._stream) return;
        // Reject pending reads
        var pending = this._pendingReads;
        while (pending.length > 0) {
            var p = pending.shift();
            p.reject(new TypeError('Reader was released'));
        }
        this._stream._reader = null;
        this._stream._locked = false;
        this._stream = null;
    };

    ReadableStreamDefaultReader.prototype.cancel = function(reason) {
        if (!this._stream) return Promise.reject(new TypeError('Reader has been released'));
        return this._stream.cancel(reason);
    };

    // ── ReadableStream ────────────────────────────────────────────────────────

    function ReadableStream(underlyingSource, strategy) {
        this._state = 'readable';  // 'readable' | 'closed' | 'errored'
        this._storedError = undefined;
        this._reader = null;
        this._locked = false;
        this._queue = [];
        this._pullInProgress = false;
        this._pullAgain = false;
        this._source = underlyingSource || {};
        this._started = false;

        // Create controller
        this._controller = new ReadableStreamDefaultController(this);

        // Call start
        var self = this;
        if (this._source.start) {
            try {
                var startResult = this._source.start(this._controller);
                Promise.resolve(startResult).then(function() {
                    self._started = true;
                    self._pullIfNeeded();
                }, function(e) {
                    self._controller.error(e);
                });
            } catch (e) {
                this._controller.error(e);
            }
        } else {
            this._started = true;
        }
    }

    ReadableStream.prototype._pullIfNeeded = function() {
        if (!this._started) return;
        if (this._pullInProgress) { this._pullAgain = true; return; }
        if (this._state !== 'readable') return;
        if (!this._source.pull) return;

        // Only pull if reader is waiting or queue is empty
        var shouldPull = false;
        if (this._reader && this._reader._pendingReads.length > 0) shouldPull = true;
        if (this._queue.length === 0 && !this._controller._closeRequested) shouldPull = true;

        if (!shouldPull) return;

        this._pullInProgress = true;
        var self = this;
        try {
            var pullResult = this._source.pull(this._controller);
            Promise.resolve(pullResult).then(function() {
                self._pullInProgress = false;
                if (self._pullAgain) {
                    self._pullAgain = false;
                    self._pullIfNeeded();
                }
            }, function(e) {
                self._pullInProgress = false;
                self._controller.error(e);
            });
        } catch (e) {
            this._pullInProgress = false;
            this._controller.error(e);
        }
    };

    Object.defineProperty(ReadableStream.prototype, 'locked', {
        get: function() { return this._locked; }
    });

    ReadableStream.prototype.getReader = function(options) {
        if (options && options.mode === 'byob') {
            throw new TypeError('BYOB readers not supported');
        }
        return new ReadableStreamDefaultReader(this);
    };

    ReadableStream.prototype.cancel = function(reason) {
        if (this._state === 'closed') return Promise.resolve();
        if (this._state === 'errored') return Promise.reject(this._storedError);
        this._state = 'closed';

        // Resolve any pending reads
        if (this._reader) {
            var pending = this._reader._pendingReads;
            while (pending.length > 0) {
                var p = pending.shift();
                p.resolve({ value: undefined, done: true });
            }
            if (this._reader._closedResolve) {
                this._reader._closedResolve();
                this._reader._closedResolve = null;
            }
        }

        if (this._source.cancel) {
            try {
                return Promise.resolve(this._source.cancel(reason));
            } catch (e) {
                return Promise.reject(e);
            }
        }
        return Promise.resolve();
    };

    ReadableStream.prototype.tee = function() {
        if (this._locked) throw new TypeError('Stream is locked');
        var reader = this.getReader();
        var canceled1 = false, canceled2 = false;
        var reason1, reason2;
        var branch1Controller, branch2Controller;

        function pump() {
            reader.read().then(function(result) {
                if (result.done) {
                    if (!canceled1) branch1Controller.close();
                    if (!canceled2) branch2Controller.close();
                    return;
                }
                if (!canceled1) branch1Controller.enqueue(result.value);
                if (!canceled2) branch2Controller.enqueue(result.value);
                pump();
            });
        }

        var branch1 = new ReadableStream({
            start: function(c) { branch1Controller = c; },
            cancel: function(reason) { canceled1 = true; reason1 = reason; }
        });
        var branch2 = new ReadableStream({
            start: function(c) { branch2Controller = c; },
            cancel: function(reason) { canceled2 = true; reason2 = reason; }
        });

        pump();
        return [branch1, branch2];
    };

    ReadableStream.prototype.pipeThrough = function(transform, options) {
        if (this._locked) throw new TypeError('Stream is locked');
        var reader = this.getReader();
        var writer = transform.writable && transform.writable.getWriter
            ? transform.writable.getWriter()
            : null;

        if (writer) {
            // Errors surface on transform.readable (the transform errors it);
            // the handlers here only stop the pump and silence what would
            // otherwise be unhandled rejections (e.g. a codec transform
            // rejecting a write, or a flush failing at close).
            var noop = function() {};
            function pump() {
                reader.read().then(function(result) {
                    if (result.done) {
                        var p = writer.close();
                        if (p && p.then) p.then(null, noop);
                        return;
                    }
                    writer.write(result.value).then(pump, function(e) {
                        var p = reader.cancel(e);
                        if (p && p.then) p.then(null, noop);
                    });
                }, function(e) {
                    var p = writer.abort(e);
                    if (p && p.then) p.then(null, noop);
                });
            }
            pump();
        }
        return transform.readable;
    };

    ReadableStream.prototype.pipeTo = function(dest, options) {
        if (this._locked) throw new TypeError('Stream is locked');
        var reader = this.getReader();
        var writer = dest.getWriter ? dest.getWriter() : null;
        if (!writer) return Promise.reject(new TypeError('Destination is not writable'));

        return new Promise(function(resolve, reject) {
            function pump() {
                reader.read().then(function(result) {
                    if (result.done) {
                        writer.close().then(resolve, reject);
                        return;
                    }
                    writer.write(result.value).then(pump, reject);
                }, reject);
            }
            pump();
        });
    };

    // Async iteration: for await (const chunk of stream)
    ReadableStream.prototype[Symbol.asyncIterator] = function() {
        var reader = this.getReader();
        return {
            next: function() {
                return reader.read();
            },
            return: function() {
                reader.releaseLock();
                return Promise.resolve({ value: undefined, done: true });
            }
        };
    };

    // Static from() helper — create from iterable/async iterable
    ReadableStream.from = function(source) {
        if (source && typeof source[Symbol.asyncIterator] === 'function') {
            var iter = source[Symbol.asyncIterator]();
            return new ReadableStream({
                pull: function(controller) {
                    return iter.next().then(function(result) {
                        if (result.done) { controller.close(); return; }
                        controller.enqueue(result.value);
                    });
                }
            });
        }
        if (source && typeof source[Symbol.iterator] === 'function') {
            var iter = source[Symbol.iterator]();
            return new ReadableStream({
                start: function(controller) {
                    var result;
                    while (!(result = iter.next()).done) {
                        controller.enqueue(result.value);
                    }
                    controller.close();
                }
            });
        }
        throw new TypeError('ReadableStream.from: not iterable');
    };

    // ── TextDecoderStream ─────────────────────────────────────────────────────
    // Minimal transform stream for decoding Uint8Array chunks to strings

    function TextDecoderStream(encoding) {
        this._decoder = new TextDecoder(encoding || 'utf-8');
        var decoder = this._decoder;
        var readableController;

        this.writable = {
            getWriter: function() {
                var chunks = [];
                var closeResolve;
                return {
                    write: function(chunk) {
                        var text = decoder.decode(chunk, { stream: true });
                        if (text) readableController.enqueue(text);
                        return Promise.resolve();
                    },
                    close: function() {
                        var final = decoder.decode();
                        if (final) readableController.enqueue(final);
                        readableController.close();
                        return Promise.resolve();
                    },
                    releaseLock: function() {}
                };
            }
        };

        this.readable = new ReadableStream({
            start: function(c) { readableController = c; }
        });
    }

    // ── Expose ────────────────────────────────────────────────────────────────

    globalThis.ReadableStream = ReadableStream;
    globalThis.ReadableStreamDefaultReader = ReadableStreamDefaultReader;
    globalThis.ReadableStreamDefaultController = ReadableStreamDefaultController;
    globalThis.TextDecoderStream = TextDecoderStream;
})();
