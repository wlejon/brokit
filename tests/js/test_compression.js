// Test: CompressionStream / DecompressionStream (Compression Streams spec)
// Formats: gzip (RFC 1952), deflate (zlib, RFC 1950), deflate-raw (RFC 1951)

// ── API existence ────────────────────────────────────────────────────────
assert(typeof CompressionStream === 'function', 'CompressionStream exists');
assert(typeof DecompressionStream === 'function', 'DecompressionStream exists');

// ── Unknown format → TypeError (synchronous, from the constructor) ───────
function expectTypeError(fn, msg) {
    try { fn(); assert(false, msg + ' (did not throw)'); }
    catch (e) { assert(e instanceof TypeError, msg + ' (got ' + e + ')'); }
}
expectTypeError(function() { new CompressionStream('br'); }, 'CompressionStream unknown format throws TypeError');
expectTypeError(function() { new DecompressionStream('lzma'); }, 'DecompressionStream unknown format throws TypeError');
expectTypeError(function() { new CompressionStream(); }, 'CompressionStream no-arg throws TypeError');

var cs0 = new CompressionStream('gzip');
assert(cs0.readable instanceof ReadableStream, 'readable is a ReadableStream');
assert(cs0.writable instanceof WritableStream, 'writable is a WritableStream');

// ── Helpers ──────────────────────────────────────────────────────────────
function b64ToBytes(b64) {
    var bin = atob(b64);
    var out = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
}

function bytesEqual(a, b) {
    if (a.length !== b.length) return false;
    for (var i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

async function readAll(stream) {
    var reader = stream.getReader();
    var chunks = [];
    var total = 0;
    for (;;) {
        var r = await reader.read();
        if (r.done) break;
        chunks.push(r.value);
        total += r.value.length;
    }
    var out = new Uint8Array(total);
    var off = 0;
    for (var i = 0; i < chunks.length; i++) { out.set(chunks[i], off); off += chunks[i].length; }
    return { bytes: out, chunkCount: chunks.length };
}

// Write `chunks` (array of BufferSource) into ts.writable, close, read all output.
async function runThrough(ts, chunks) {
    var writer = ts.writable.getWriter();
    for (var i = 0; i < chunks.length; i++) await writer.write(chunks[i]);
    await writer.close();
    return readAll(ts.readable);
}

// Deterministic incompressible-ish data (xorshift PRNG).
function patternedBytes(n) {
    var out = new Uint8Array(n);
    var x = 123456789;
    for (var i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >>> 17; x ^= x << 5; x |= 0;
        out[i] = x & 0xff;
    }
    return out;
}

var FIXTURE_TEXT = 'The quick brown fox jumps over the lazy dog. bro CompressionStream interop fixture 0123456789.';
// python3: base64(gzip.compress(text, mtime=0)) / zlib.compress / zlib raw (wbits=-15)
var GZIP_B64 = 'H4sIAAAAAAAC/xXKWxpDMBAG0K38K/BRl/JsCbWBYFRoMjFJSq2+PJ/TzYQt6mFFL7xbTHxgicZ58JcE4eKPOn8Y+Z3cBS0bJ+S9ZvsKQspA20DCDpM+QhRCmj3yoqyedZP8ATFF+ZZeAAAA';
var ZLIB_B64 = 'eJwVylEWgiAQBdCtvBVwyjLtuyXUBlBHwYLBYTBq9Z3u9304wlb8+MQg/I6YuWItIWXwTgJ1hJf9fjDxYv4FNw5JKGfP8a5CNsBHJeGE2VctQjgcm9O5vXT91fwAZYMhLA==';
var RAW_B64 = 'FcpRFoIgEAXQrbwVcMoy7bsl1AZQR8GCwWEwavWd7vd9OMJW/PjEIPyOmLliLSFl8E4CdYSX/X4w8WL+BTcOSShnz/GuQjbARyXhhNlXLUI4HJvTub10/dX8AA==';

var done = {};

(async function() {
    var enc = new TextEncoder();
    var dec = new TextDecoder();
    var fixtureBytes = enc.encode(FIXTURE_TEXT);

    // ── Round-trip, all three formats ────────────────────────────────────
    for (var fi = 0; fi < 3; fi++) {
        var format = ['gzip', 'deflate', 'deflate-raw'][fi];
        var compressed = await runThrough(new CompressionStream(format), [fixtureBytes]);
        assert(compressed.bytes.length > 0, format + ': produced output');
        var decompressed = await runThrough(new DecompressionStream(format), [compressed.bytes]);
        assert(bytesEqual(decompressed.bytes, fixtureBytes), format + ': round-trip byte-equal');
    }

    // ── Direct native codec layer (unit-level) ───────────────────────────
    var codec = __brokit_compression.create('compress', 'deflate-raw');
    var part1 = codec.push(enc.encode('hello '));
    var part2 = codec.push(enc.encode('world'));
    var tail = codec.finish();
    var joined = new Uint8Array(part1.length + part2.length + tail.length);
    joined.set(part1, 0); joined.set(part2, part1.length);
    joined.set(tail, part1.length + part2.length);
    var back = __brokit_compression.create('decompress', 'deflate-raw');
    var out1 = back.push(joined);
    var out2 = back.finish();
    var backJoined = new Uint8Array(out1.length + out2.length);
    backJoined.set(out1, 0); backJoined.set(out2, out1.length);
    assertEqual(dec.decode(backJoined), 'hello world', 'native codec incremental round-trip');
    expectTypeError(function() { __brokit_compression.create('compress', 'nope'); },
                    'native codec unknown format throws TypeError');

    // ── gzip wrapper details on compressed output ────────────────────────
    var g = await runThrough(new CompressionStream('gzip'), [fixtureBytes]);
    assert(g.bytes[0] === 0x1f && g.bytes[1] === 0x8b && g.bytes[2] === 8,
           'gzip: magic bytes + deflate CM');
    var isize = g.bytes[g.bytes.length - 4] | (g.bytes[g.bytes.length - 3] << 8) |
                (g.bytes[g.bytes.length - 2] << 16) | (g.bytes[g.bytes.length - 1] << 24);
    assertEqual(isize >>> 0, fixtureBytes.length, 'gzip: footer ISIZE matches input length');

    // zlib header: CMF 0x78, and (CMF*256+FLG) % 31 === 0
    var z = await runThrough(new CompressionStream('deflate'), [fixtureBytes]);
    assertEqual(z.bytes[0], 0x78, 'deflate: zlib CMF byte');
    assertEqual((z.bytes[0] * 256 + z.bytes[1]) % 31, 0, 'deflate: zlib FCHECK valid');

    // ── Interop: decompress python-produced fixtures ─────────────────────
    var pg = await runThrough(new DecompressionStream('gzip'), [b64ToBytes(GZIP_B64)]);
    assertEqual(dec.decode(pg.bytes), FIXTURE_TEXT, 'gzip: python fixture decompresses to exact text');
    var pz = await runThrough(new DecompressionStream('deflate'), [b64ToBytes(ZLIB_B64)]);
    assertEqual(dec.decode(pz.bytes), FIXTURE_TEXT, 'deflate: python fixture decompresses to exact text');
    var pr = await runThrough(new DecompressionStream('deflate-raw'), [b64ToBytes(RAW_B64)]);
    assertEqual(dec.decode(pr.bytes), FIXTURE_TEXT, 'deflate-raw: python fixture decompresses to exact text');

    // ── Incremental: many small writes, streamed output ──────────────────
    var big = patternedBytes(256 * 1024);
    var writes = [];
    for (var off = 0; off < big.length; off += 4096) writes.push(big.subarray(off, off + 4096));
    var bigCompressed = await runThrough(new CompressionStream('gzip'), writes);
    assert(bigCompressed.chunkCount > 1,
           'incremental: more than one output chunk (' + bigCompressed.chunkCount + ')');
    var bigBack = await runThrough(new DecompressionStream('gzip'), [bigCompressed.bytes]);
    assert(bigBack.chunkCount > 1,
           'incremental: decompressed output in multiple chunks (' + bigBack.chunkCount + ')');
    assert(bytesEqual(bigBack.bytes, big), 'incremental: 256K round-trip byte-equal');

    // ── Chunk type flexibility: ArrayBuffer / DataView / Float-view ─────
    var abIn = fixtureBytes.slice().buffer;
    var viaAb = await runThrough(new CompressionStream('deflate'), [abIn]);
    var viaAbBack = await runThrough(new DecompressionStream('deflate'), [new DataView(viaAb.bytes.slice().buffer)]);
    assert(bytesEqual(viaAbBack.bytes, fixtureBytes), 'ArrayBuffer + DataView chunks accepted');

    // ── Empty input ──────────────────────────────────────────────────────
    var emptyGz = await runThrough(new CompressionStream('gzip'), []);
    assert(emptyGz.bytes.length >= 18, 'gzip: empty input still yields header+trailer');
    var emptyBack = await runThrough(new DecompressionStream('gzip'), [emptyGz.bytes]);
    assertEqual(emptyBack.bytes.length, 0, 'gzip: empty round-trip');

    // ── Error: non-BufferSource write ────────────────────────────────────
    try {
        await runThrough(new CompressionStream('gzip'), ['a plain string']);
        assert(false, 'string chunk should error the stream');
    } catch (e) {
        assert(e instanceof TypeError, 'string chunk rejects with TypeError (got ' + e + ')');
    }

    // ── Error: corrupt compressed data ───────────────────────────────────
    var corrupt = b64ToBytes(GZIP_B64);
    corrupt[20] ^= 0xff; corrupt[21] ^= 0xff; corrupt[22] ^= 0xff;
    try {
        await runThrough(new DecompressionStream('gzip'), [corrupt]);
        assert(false, 'corrupt gzip should error');
    } catch (e) {
        assert(e instanceof TypeError, 'corrupt gzip rejects with TypeError (got ' + e + ')');
    }

    // ── Error: truncated stream (close before deflate end) ───────────────
    var whole = b64ToBytes(GZIP_B64);
    try {
        await runThrough(new DecompressionStream('gzip'), [whole.subarray(0, whole.length - 12)]);
        assert(false, 'truncated gzip should error');
    } catch (e) {
        assert(e instanceof TypeError, 'truncated gzip rejects with TypeError (got ' + e + ')');
    }
    // Truncated zlib: missing adler trailer
    var zwhole = b64ToBytes(ZLIB_B64);
    try {
        await runThrough(new DecompressionStream('deflate'), [zwhole.subarray(0, zwhole.length - 4)]);
        assert(false, 'truncated zlib should error');
    } catch (e) {
        assert(e instanceof TypeError, 'truncated zlib rejects with TypeError (got ' + e + ')');
    }
    // Zero-length decompression input is also a truncation
    try {
        await runThrough(new DecompressionStream('gzip'), []);
        assert(false, 'empty decompression input should error');
    } catch (e) {
        assert(e instanceof TypeError, 'empty decompression input rejects with TypeError (got ' + e + ')');
    }

    // ── Error: trailing garbage after the compressed stream ──────────────
    var withJunk = new Uint8Array(whole.length + 4);
    withJunk.set(whole, 0);
    withJunk.set([1, 2, 3, 4], whole.length);
    try {
        await runThrough(new DecompressionStream('gzip'), [withJunk]);
        assert(false, 'trailing garbage should error');
    } catch (e) {
        assert(e instanceof TypeError, 'trailing garbage rejects with TypeError (got ' + e + ')');
    }
    // Same, delivered as a separate later write
    try {
        await runThrough(new DecompressionStream('deflate'), [zwhole, new Uint8Array([9, 9])]);
        assert(false, 'trailing garbage in later write should error');
    } catch (e) {
        assert(e instanceof TypeError, 'late trailing garbage rejects with TypeError (got ' + e + ')');
    }

    // ── Corrupt gzip CRC in trailer ──────────────────────────────────────
    var badCrc = b64ToBytes(GZIP_B64);
    badCrc[badCrc.length - 8] ^= 0xff; // first CRC byte
    try {
        await runThrough(new DecompressionStream('gzip'), [badCrc]);
        assert(false, 'bad gzip crc should error');
    } catch (e) {
        assert(e instanceof TypeError, 'bad gzip crc rejects with TypeError (got ' + e + ')');
    }

    // ── Abandoned mid-write stream (teardown safety; leak-checked builds) ─
    {
        var abandoned = new CompressionStream('gzip');
        var w = abandoned.writable.getWriter();
        await w.write(patternedBytes(8192));
        // Neither closed nor aborted — dropped on the floor.
    }

    done.all = true;
})().then(function() {
    assert(done.all === true, 'async compression test suite ran to completion');
}, function(e) {
    assert(false, 'async compression test suite failed: ' + e + (e && e.stack ? '\n' + e.stack : ''));
});
