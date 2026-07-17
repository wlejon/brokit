(function() {
    'use strict';

    // ── WritableStreamDefaultController ──────────────────────────────────────

    function WritableStreamDefaultController(stream) {
        this._stream = stream;
    }

    WritableStreamDefaultController.prototype.error = function(e) {
        var stream = this._stream;
        if (stream._state !== 'writable') return;
        stream._state = 'errored';
        stream._storedError = e;
        _rejectPending(stream, e);
    };

    Object.defineProperty(WritableStreamDefaultController.prototype, 'signal', {
        get: function() {
            // AbortSignal for writer abort — simplified stub
            return undefined;
        }
    });

    // ── WritableStreamDefaultWriter ──────────────────────────────────────────

    function WritableStreamDefaultWriter(stream) {
        if (stream._locked) throw new TypeError('Stream is already locked');
        this._stream = stream;
        stream._writer = this;
        stream._locked = true;

        var self = this;
        this._readyResolve = null;
        this._readyReject = null;
        this._readyPromise = new Promise(function(resolve, reject) {
            if (stream._state === 'writable') { resolve(); return; }
            if (stream._state === 'errored') { reject(stream._storedError); return; }
            self._readyResolve = resolve;
            self._readyReject = reject;
        });

        this._closedResolve = null;
        this._closedReject = null;
        this._closedPromise = new Promise(function(resolve, reject) {
            if (stream._state === 'closed') { resolve(); return; }
            if (stream._state === 'errored') { reject(stream._storedError); return; }
            self._closedResolve = resolve;
            self._closedReject = reject;
        });

        // The spec stores ready/closed with [[PromiseIsHandled]] = true: a
        // stream erroring must not fire an unhandled rejection just because
        // nobody awaited writer.closed. Callers who do await still see it.
        this._readyPromise.then(null, function() {});
        this._closedPromise.then(null, function() {});
    }

    Object.defineProperties(WritableStreamDefaultWriter.prototype, {
        closed: {
            get: function() { return this._closedPromise; }
        },
        ready: {
            get: function() { return this._readyPromise; }
        },
        desiredSize: {
            get: function() {
                if (!this._stream) return null;
                if (this._stream._state === 'errored') return null;
                if (this._stream._state === 'closed') return 0;
                return 1 - this._stream._queue.length;
            }
        }
    });

    WritableStreamDefaultWriter.prototype.write = function(chunk) {
        var stream = this._stream;
        if (!stream) return Promise.reject(new TypeError('Writer has been released'));
        if (stream._state === 'closed')
            return Promise.reject(new TypeError('Stream is closed'));
        if (stream._state === 'errored')
            return Promise.reject(stream._storedError);

        return new Promise(function(resolve, reject) {
            stream._queue.push({ chunk: chunk, resolve: resolve, reject: reject });
            _processQueue(stream);
        });
    };

    WritableStreamDefaultWriter.prototype.close = function() {
        var stream = this._stream;
        if (!stream) return Promise.reject(new TypeError('Writer has been released'));
        if (stream._state === 'closed' || stream._closeRequested)
            return Promise.reject(new TypeError('Stream is already closing/closed'));

        stream._closeRequested = true;

        // If queue is empty, close now
        if (stream._queue.length === 0 && !stream._writing) {
            return _closeStream(stream);
        }

        // Otherwise it will close after queue drains
        return new Promise(function(resolve, reject) {
            stream._closePending = { resolve: resolve, reject: reject };
        });
    };

    WritableStreamDefaultWriter.prototype.abort = function(reason) {
        var stream = this._stream;
        if (!stream) return Promise.reject(new TypeError('Writer has been released'));
        return _abortStream(stream, reason);
    };

    WritableStreamDefaultWriter.prototype.releaseLock = function() {
        if (!this._stream) return;
        this._stream._writer = null;
        this._stream._locked = false;
        this._stream = null;
    };

    // ── WritableStream ───────────────────────────────────────────────────────

    function WritableStream(underlyingSink, strategy) {
        this._state = 'writable'; // 'writable' | 'closed' | 'errored'
        this._storedError = undefined;
        this._writer = null;
        this._locked = false;
        this._queue = [];
        this._writing = false;
        this._closeRequested = false;
        this._closePending = null;
        this._sink = underlyingSink || {};
        this._controller = new WritableStreamDefaultController(this);
        this._started = false;

        var self = this;
        if (this._sink.start) {
            try {
                var startResult = this._sink.start(this._controller);
                Promise.resolve(startResult).then(function() {
                    self._started = true;
                    _processQueue(self);
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

    Object.defineProperty(WritableStream.prototype, 'locked', {
        get: function() { return this._locked; }
    });

    WritableStream.prototype.getWriter = function() {
        return new WritableStreamDefaultWriter(this);
    };

    WritableStream.prototype.abort = function(reason) {
        if (this._locked) return Promise.reject(new TypeError('Stream is locked'));
        return _abortStream(this, reason);
    };

    WritableStream.prototype.close = function() {
        if (this._locked) return Promise.reject(new TypeError('Stream is locked'));
        if (this._state === 'closed' || this._closeRequested)
            return Promise.reject(new TypeError('Stream is already closing/closed'));
        this._closeRequested = true;
        if (this._queue.length === 0 && !this._writing) {
            return _closeStream(this);
        }
        return new Promise(function(resolve, reject) {
            this._closePending = { resolve: resolve, reject: reject };
        }.bind(this));
    };

    // ── Internal helpers ─────────────────────────────────────────────────────

    function _processQueue(stream) {
        if (!stream._started) return;
        if (stream._writing) return;
        if (stream._state !== 'writable') return;
        if (stream._queue.length === 0) {
            if (stream._closeRequested) {
                _closeStream(stream).then(function() {
                    if (stream._closePending) stream._closePending.resolve();
                }, function(e) {
                    if (stream._closePending) stream._closePending.reject(e);
                });
            }
            return;
        }

        var entry = stream._queue.shift();
        stream._writing = true;

        if (stream._sink.write) {
            try {
                var writeResult = stream._sink.write(entry.chunk, stream._controller);
                Promise.resolve(writeResult).then(function() {
                    stream._writing = false;
                    entry.resolve();
                    _processQueue(stream);
                }, function(e) {
                    stream._writing = false;
                    entry.reject(e);
                    stream._controller.error(e);
                });
            } catch (e) {
                stream._writing = false;
                entry.reject(e);
                stream._controller.error(e);
            }
        } else {
            // No write callback — just resolve
            stream._writing = false;
            entry.resolve();
            _processQueue(stream);
        }
    }

    function _closeStream(stream) {
        stream._state = 'closed';
        var writer = stream._writer;

        var closePromise;
        if (stream._sink.close) {
            try {
                closePromise = Promise.resolve(stream._sink.close());
            } catch (e) {
                closePromise = Promise.reject(e);
            }
        } else {
            closePromise = Promise.resolve();
        }

        return closePromise.then(function() {
            if (writer && writer._closedResolve) {
                writer._closedResolve();
                writer._closedResolve = null;
            }
        });
    }

    function _abortStream(stream, reason) {
        if (stream._state === 'closed') return Promise.resolve();
        stream._state = 'errored';
        stream._storedError = reason;
        _rejectPending(stream, reason);

        if (stream._sink.abort) {
            try {
                return Promise.resolve(stream._sink.abort(reason));
            } catch (e) {
                return Promise.reject(e);
            }
        }
        return Promise.resolve();
    }

    function _rejectPending(stream, error) {
        var queue = stream._queue;
        while (queue.length > 0) {
            var entry = queue.shift();
            entry.reject(error);
        }
        var writer = stream._writer;
        if (writer) {
            if (writer._closedReject) {
                writer._closedReject(error);
                writer._closedReject = null;
            }
            if (writer._readyReject) {
                writer._readyReject(error);
                writer._readyReject = null;
            }
        }
        if (stream._closePending) {
            stream._closePending.reject(error);
            stream._closePending = null;
        }
    }

    // ── TransformStreamDefaultController ─────────────────────────────────────

    function TransformStreamDefaultController(readable) {
        this._readableController = null;
        this._readable = readable;
    }

    TransformStreamDefaultController.prototype.enqueue = function(chunk) {
        if (this._readableController) {
            this._readableController.enqueue(chunk);
        }
    };

    TransformStreamDefaultController.prototype.error = function(e) {
        if (this._readableController) {
            this._readableController.error(e);
        }
    };

    TransformStreamDefaultController.prototype.terminate = function() {
        if (this._readableController) {
            this._readableController.close();
        }
    };

    Object.defineProperty(TransformStreamDefaultController.prototype, 'desiredSize', {
        get: function() {
            if (this._readableController) {
                return this._readableController.desiredSize;
            }
            return null;
        }
    });

    // ── TransformStream ──────────────────────────────────────────────────────

    function TransformStream(transformer, writableStrategy, readableStrategy) {
        transformer = transformer || {};

        var transformController = new TransformStreamDefaultController();
        var self = this;

        // Create the readable side
        this.readable = new ReadableStream({
            start: function(controller) {
                transformController._readableController = controller;
            }
        });

        // The transform function
        var transformFn = transformer.transform || function(chunk, controller) {
            // Identity transform — pass through
            controller.enqueue(chunk);
        };

        var flushFn = transformer.flush || null;

        // Create the writable side
        this.writable = new WritableStream({
            start: function(controller) {
                if (transformer.start) {
                    return transformer.start(transformController);
                }
            },
            write: function(chunk) {
                try {
                    var result = transformFn(chunk, transformController);
                    return Promise.resolve(result);
                } catch (e) {
                    transformController.error(e);
                    return Promise.reject(e);
                }
            },
            close: function() {
                if (flushFn) {
                    try {
                        var result = flushFn(transformController);
                        return Promise.resolve(result).then(function() {
                            transformController.terminate();
                        });
                    } catch (e) {
                        transformController.error(e);
                        return Promise.reject(e);
                    }
                }
                transformController.terminate();
            },
            abort: function(reason) {
                transformController.error(reason);
            }
        });
    }

    // ── Refactor TextDecoderStream to use real TransformStream ───────────────

    function TextDecoderStreamNew(encoding) {
        var decoder = new TextDecoder(encoding || 'utf-8');
        var ts = new TransformStream({
            transform: function(chunk, controller) {
                var text = decoder.decode(chunk, { stream: true });
                if (text) controller.enqueue(text);
            },
            flush: function(controller) {
                var final = decoder.decode();
                if (final) controller.enqueue(final);
            }
        });
        this.readable = ts.readable;
        this.writable = ts.writable;
        this.encoding = (encoding || 'utf-8').toLowerCase();
    }

    // ── TextEncoderStream ────────────────────────────────────────────────────

    function TextEncoderStream() {
        var encoder = new TextEncoder();
        var ts = new TransformStream({
            transform: function(chunk, controller) {
                controller.enqueue(encoder.encode(String(chunk)));
            }
        });
        this.readable = ts.readable;
        this.writable = ts.writable;
        this.encoding = 'utf-8';
    }

    // ── Expose ───────────────────────────────────────────────────────────────

    globalThis.WritableStream = WritableStream;
    globalThis.WritableStreamDefaultWriter = WritableStreamDefaultWriter;
    globalThis.WritableStreamDefaultController = WritableStreamDefaultController;
    globalThis.TransformStream = TransformStream;
    globalThis.TransformStreamDefaultController = TransformStreamDefaultController;

    // Replace the minimal TextDecoderStream with the real one
    globalThis.TextDecoderStream = TextDecoderStreamNew;
    globalThis.TextEncoderStream = TextEncoderStream;
})();
