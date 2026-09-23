// Script-controlled sizes and indices reaching native code: every one of these
// used to overflow, wrap or index out of bounds (or allocate without a cap)
// instead of throwing.

function throws(fn, label) {
    var threw = false;
    try { fn(); } catch (e) { threw = true; }
    assert(threw, label);
}

// ── bro.image ────────────────────────────────────────────────────────────
var img = bro.image;
var lut = img.gradient([[0, 10, 20, 30], [1, 200, 210, 220]], 4);

// A NaN sample indexes LUT entry 0, on both the Float32 kernel and the
// general path, in clamp and wrap mode (it used to become INT_MIN).
[new Float32Array([NaN, Infinity, -Infinity]), new Float64Array([NaN, Infinity, -Infinity])].forEach(function (src) {
    ['clamp', 'wrap'].forEach(function (edge) {
        var out = new Uint8Array(src.length * 4);
        img.lookup(out, src, lut, { lo: 0, hi: 1, edge: edge });
        assert(out[0] === lut[0] && out[1] === lut[1], 'lookup NaN -> entry 0 (' + edge + ')');
    });
});

// Histogram: NaN samples are dropped (a sample just below lo still truncates
// into bin 0, as the Float32 kernel does); bins is capped.
[Float32Array, Float64Array].forEach(function (T) {
    var hist = img.reduce(new T([NaN, 0.25, 0.75, -0.25]), 'histogram', { bins: 2, lo: 0, hi: 1 });
    assert(hist[0] === 2 && hist[1] === 1, 'histogram drops NaN (' + T.name + ')');
});
throws(function () { img.reduce(new Float32Array(4), 'histogram', { bins: 2147483647, lo: 0, hi: 1 }); },
       'histogram bins is capped');

// Sizes whose product wraps size_t used to pass the buffer check.
throws(function () {
    img.resample(new Float32Array(4), new Float32Array(4),
                 { srcW: 65536, srcH: 65536, dstW: 65536, dstH: 65536, channels: 1073741824 });
}, 'resample dims that wrap size_t');
throws(function () { img.alloc(65536, 65536, 1); }, 'alloc past uint32 elements');
throws(function () { img.gradient([[0, 0, 0, 0], [1, 1, 1, 1]], 1 << 30); }, 'gradient n is capped');

// A resizable buffer shrunk by an option getter mid-call.
var rab = new ArrayBuffer(64, { maxByteLength: 64 });
var shrinking = new Float32Array(rab);
throws(function () {
    img.resample(new Float32Array(16), shrinking,
                 { srcW: 4, srcH: 4, dstW: 4, dstH: 4, get filter() { rab.resize(0); return 'nearest'; } });
}, 'resample refuses a source shrunk during the call');

// ── FastNoise ────────────────────────────────────────────────────────────
var SIMPLEX_ENCODED = "E@BBZEG@BD8JFgIECArXIzwECiQIw/UoPwkuAAE@BJDQAH@BC@AIEAJBw@ABZEED0KV78YZmZmPwQDmpkZPwsAAIA/HAMAAHBCBA==";
var gen = new FastNoise(SIMPLEX_ENCODED);
// 2^21 * 2^21 * 2^22 wraps to 0 floats: the dest check passed and FastNoise
// wrote far past an 8-float buffer.
throws(function () {
    gen.genUniformGrid3DInto(new Float32Array(8), 0, 0, 0, 2097152, 2097152, 4194304, 0.01, 1);
}, 'genUniformGrid3DInto dims that wrap');
throws(function () { gen.genUniformGrid2D(0, 0, 65536, 65536, 0.01, 1); }, 'genUniformGrid2D is capped');
var small = gen.genUniformGrid2D(0, 0, 4, 4, 0.01, 1);
assert(small instanceof Float32Array && small.length === 16, 'genUniformGrid2D still works');

// ── fs.readSync / writeSync windows ──────────────────────────────────────
var fs = globalThis.__brokit_fs;
var os = globalThis.__brokit_os;
var path = os.tmpdir() + '/brokit_bounds_' + Date.now() + '.bin';
fs.writeFileSync(path, 'abcdefgh');
var fd = fs.openSync(path, 'r');
// offset + length overflowed int64 into range.
throws(function () { fs.readSync(fd, new Uint8Array(8), 4e18, 6e18, 0); }, 'readSync window that overflows');
throws(function () { fs.readSync(fd, new Uint8Array(8), Infinity, 1, 0); }, 'readSync infinite offset');
var buf = new Uint8Array(8);
assert(fs.readSync(fd, buf, 0, 8, 0) === 8, 'readSync in bounds');
fs.closeSync(fd);
fs.unlinkSync(path);

// ── crypto.subtle BufferSource windows ───────────────────────────────────
(async function () {
    // byteOffset + byteLength wraps size_t to 4, inside the 8-byte buffer.
    var forged = { byteOffset: 18446744073709549568, byteLength: 2052, buffer: new ArrayBuffer(8) };
    var rejected = false;
    try { await crypto.subtle.digest('SHA-256', forged); } catch (e) { rejected = true; }
    assert(rejected, 'digest refuses a window that wraps');
    var ok = await crypto.subtle.digest('SHA-256', { byteOffset: 2, byteLength: 4, buffer: new ArrayBuffer(8) });
    assert(ok instanceof ArrayBuffer && ok.byteLength === 32, 'digest of an in-bounds window');
})().catch(function (e) {
    assert(false, 'crypto bounds failed: ' + (e && e.stack || e));
});

// ── FileReader listener list with a tampered length ──────────────────────
var fr = new FileReader();
fr.addEventListener('load', function () {});
fr.__brokitListeners_load.length = 1e300;
fr.addEventListener('load', function () {});  // must not loop or overflow
assert(true, 'listener list with an absurd length');
