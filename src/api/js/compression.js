// CompressionStream / DecompressionStream — web-standard Compression Streams
// (https://compression.spec.whatwg.org/). The incremental codec lives in
// native code (__brokit_compression, see compression.cpp); this layer is the
// class shells over TransformStream.
//
// Formats: "gzip" (RFC 1952), "deflate" (ZLIB-wrapped, RFC 1950),
// "deflate-raw" (raw DEFLATE, RFC 1951).
(function() {
    'use strict';

    // Output is enqueued in bounded chunks so large results stream instead of
    // arriving as one giant buffer.
    var OUT_CHUNK = 65536;

    function enqueueOutput(controller, bytes) {
        for (var i = 0; i < bytes.length; i += OUT_CHUNK) {
            controller.enqueue(bytes.subarray(i, Math.min(i + OUT_CHUNK, bytes.length)));
        }
    }

    function toUint8(chunk) {
        if (chunk instanceof Uint8Array) return chunk;
        if (ArrayBuffer.isView(chunk))
            return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
        if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk);
        throw new TypeError('chunk must be a BufferSource (ArrayBuffer or ArrayBufferView)');
    }

    function makeCodecTransform(mode, format) {
        format = String(format);
        if (format !== 'gzip' && format !== 'deflate' && format !== 'deflate-raw') {
            throw new TypeError("Unsupported compression format: '" + format + "'");
        }
        var codec = globalThis.__brokit_compression.create(mode, format);
        return new TransformStream({
            transform: function(chunk, controller) {
                enqueueOutput(controller, codec.push(toUint8(chunk)));
            },
            flush: function(controller) {
                enqueueOutput(controller, codec.finish());
            }
        });
    }

    function CompressionStream(format) {
        if (arguments.length < 1)
            throw new TypeError('CompressionStream constructor requires a format argument');
        var ts = makeCodecTransform('compress', format);
        this.readable = ts.readable;
        this.writable = ts.writable;
    }

    function DecompressionStream(format) {
        if (arguments.length < 1)
            throw new TypeError('DecompressionStream constructor requires a format argument');
        var ts = makeCodecTransform('decompress', format);
        this.readable = ts.readable;
        this.writable = ts.writable;
    }

    globalThis.CompressionStream = CompressionStream;
    globalThis.DecompressionStream = DecompressionStream;
})();
