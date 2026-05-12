// bro.image — additional error paths and lesser-exercised dtype/options

assert(typeof bro === 'object' && typeof bro.image === 'object', 'bro.image exists');

var img = bro.image;

// ── alloc dtype variants (int16, int32, uint16) ──────────────────────────
var a16  = img.alloc(2, 2, 1, 'int16');
assert(a16 instanceof Int16Array, 'alloc int16');
var a32  = img.alloc(2, 2, 1, 'int32');
assert(a32 instanceof Int32Array, 'alloc int32');
var au16 = img.alloc(2, 2, 1, 'uint16');
assert(au16 instanceof Uint16Array, 'alloc uint16');

// ── alloc non-positive dimensions rejects ────────────────────────────────
var threw = false;
try { img.alloc(0, 4, 1); } catch (e) { threw = true; }
assert(threw, 'alloc zero width rejects');

threw = false;
try { img.alloc(-1, 4, 1); } catch (e) { threw = true; }
assert(threw, 'alloc negative width rejects');

// ── gradient with non-array stops ────────────────────────────────────────
threw = false;
try { img.gradient('not-array'); } catch (e) { threw = true; }
assert(threw, 'gradient non-array stops rejects');

// ── gradient with a non-array stop ───────────────────────────────────────
threw = false;
try { img.gradient(['not-array', [1, 255, 255, 255]]); } catch (e) { threw = true; }
assert(threw, 'gradient bad stop rejects');

// ── gradient with stop that has too few elements ─────────────────────────
threw = false;
try { img.gradient([[0, 0], [1, 255]]); } catch (e) { threw = true; }
assert(threw, 'gradient stop too short rejects');

// ── gradient with alpha (5-element stop) ─────────────────────────────────
var alphaLut = img.gradient([[0, 0, 0, 0, 0], [1, 255, 255, 255, 128]], 4);
assertEqual(alphaLut.length, 16, 'alpha gradient length');
// First alpha = 0, last alpha = 128
assertEqual(alphaLut[3], 0, 'alpha-stop first alpha');
assertEqual(alphaLut[15], 128, 'alpha-stop last alpha');

// ── lookup with wrap=true ────────────────────────────────────────────────
var lut2 = img.gradient([[0, 0, 0, 0], [1, 255, 255, 255]], 4);
var fld = new Float32Array([1.5, -0.5, 2.5]);  // out-of-[lo,hi]
var out = new Uint8ClampedArray(fld.length * 4);
img.lookup(out, fld, lut2, { lo: 0, hi: 1, wrap: true });
// Wrap mode: just check it ran without error
assert(out[3] === 255, 'lookup wrap alpha 255');

// ── lookup with too-small dst ────────────────────────────────────────────
var smallOut = new Uint8ClampedArray(4);  // only fits 1 pixel
threw = false;
try { img.lookup(smallOut, fld, lut2, { lo: 0, hi: 1 }); } catch (e) { threw = true; }
assert(threw, 'lookup small dst rejects');

// ── reduce with non-Float32 source ───────────────────────────────────────
// minmax works on any numeric TypedArray via read_at — but Int8/Int16 paths
var i16Src = new Int16Array([1, -3, 5, 7]);
var i16Mm  = img.reduce(i16Src, 'minmax');
assertEqual(i16Mm.min, -3, 'reduce minmax int16');
assertEqual(i16Mm.max, 7, 'reduce minmax int16');

var i32Src = new Int32Array([1, 100, -50, 200]);
var i32Sum = img.reduce(i32Src, 'sum');
assertEqual(i32Sum, 251, 'reduce sum int32');

var u8Src = new Uint8Array([10, 20, 30, 40]);
var u8Mean = img.reduce(u8Src, 'mean');
assertEqual(u8Mean, 25, 'reduce mean uint8');

var u16Src = new Uint16Array([100, 200]);
var u16Sum = img.reduce(u16Src, 'sum');
assertEqual(u16Sum, 300, 'reduce sum uint16');

var f64Src = new Float64Array([1.5, 2.5]);
var f64Sum = img.reduce(f64Src, 'sum');
assertEqual(f64Sum, 4, 'reduce sum float64');

// ── reduce histogram missing args ────────────────────────────────────────
threw = false;
try { img.reduce(new Float32Array([1, 2]), 'histogram'); } catch (e) { threw = true; }
assert(threw, 'reduce histogram missing args rejects');

// ── reduce unknown op rejects ────────────────────────────────────────────
threw = false;
try { img.reduce(new Float32Array([1]), 'unknown_op_xyz'); } catch (e) { threw = true; }
assert(threw, 'reduce unknown op rejects');

// ── map with non-Float32 dst (Int16 bpe!=4) rejects ──────────────────────
threw = false;
try {
    img.map(new Int16Array(4), new Float32Array([1, 2, 3, 4]), { op: 'abs' });
} catch (e) { threw = true; }
assert(threw, 'map non-Float32 dst rejects');

// ── map with too-small dst rejects ───────────────────────────────────────
threw = false;
try {
    img.map(new Float32Array(2), new Float32Array([1, 2, 3, 4]), { op: 'abs' });
} catch (e) { threw = true; }
assert(threw, 'map small dst rejects');

// ── map unknown op rejects ───────────────────────────────────────────────
threw = false;
try {
    img.map(new Float32Array(2), new Float32Array([1, 2]), { op: 'nope' });
} catch (e) { threw = true; }
assert(threw, 'map unknown op rejects');

// ── combine length mismatch ──────────────────────────────────────────────
threw = false;
try {
    img.combine(new Float32Array(4), new Float32Array(4), new Float32Array(2),
                { op: 'add' });
} catch (e) { threw = true; }
assert(threw, 'combine length mismatch rejects');

// ── combine small dst ────────────────────────────────────────────────────
threw = false;
try {
    img.combine(new Float32Array(2), new Float32Array(4), new Float32Array(4),
                { op: 'add' });
} catch (e) { threw = true; }
assert(threw, 'combine small dst rejects');

// ── combine non-Float32 rejects ──────────────────────────────────────────
threw = false;
try {
    img.combine(new Float32Array(4), new Int16Array(4), new Float32Array(4),
                { op: 'add' });
} catch (e) { threw = true; }
assert(threw, 'combine non-Float32 rejects');

// ── combine unknown op rejects ───────────────────────────────────────────
threw = false;
try {
    img.combine(new Float32Array(4), new Float32Array(4), new Float32Array(4),
                { op: 'nope' });
} catch (e) { threw = true; }
assert(threw, 'combine unknown op rejects');

// ── stencil non-Float32 rejects ──────────────────────────────────────────
threw = false;
try {
    img.stencil(new Int16Array(16), new Float32Array(16),
                { data: new Float32Array(9), w: 3, h: 3 },
                { srcW: 4, srcH: 4 });
} catch (e) { threw = true; }
assert(threw, 'stencil non-Float32 rejects');

// ── alloc dtype uint32 ───────────────────────────────────────────────────
var au32 = img.alloc(2, 2, 1, 'uint32');
assert(au32 instanceof Uint32Array, 'alloc uint32');

// ── alloc unknown dtype rejects ──────────────────────────────────────────
threw = false;
try { img.alloc(2, 2, 1, 'not_a_dtype'); } catch (e) { threw = true; }
assert(threw, 'alloc unknown dtype rejects');

// ── lookup with wrap and negative fi (force fi < 0 branch) ───────────────
var lut3 = img.gradient([[0, 0, 0, 0], [1, 255, 0, 0]], 8);
var negSrc = new Float32Array([-2.5, -1.5, 3.5]);
var negOut = new Uint8ClampedArray(negSrc.length * 4);
img.lookup(negOut, negSrc, lut3, { lo: 0, hi: 1, wrap: true });
assert(negOut[3] === 255, 'lookup wrap negative alpha 255');

// ── lookup with single-channel src (non-RGBA src is invalid? or fine) ────
// (Source is a typed array of scalars; output is RGBA.)
var oneCh = new Float32Array([0, 0.5, 1]);
var ocOut = new Uint8ClampedArray(12);
img.lookup(ocOut, oneCh, lut3, { lo: 0, hi: 1 });
assert(ocOut[11] === 255, 'lookup 1-ch src works');

// ── stencil missing srcW/srcH rejects ────────────────────────────────────
threw = false;
try {
    img.stencil(new Float32Array(16), new Float32Array(16),
                { data: new Float32Array(9), w: 3, h: 3 },
                {});
} catch (e) { threw = true; }
assert(threw, 'stencil missing srcW/srcH rejects');

// ── stencil kernel.data too small ────────────────────────────────────────
threw = false;
try {
    img.stencil(new Float32Array(16), new Float32Array(16),
                { data: new Float32Array(4), w: 3, h: 3 },  // 4 < 3*3
                { srcW: 4, srcH: 4 });
} catch (e) { threw = true; }
assert(threw, 'stencil kernel too small rejects');

// ── stencil with edge='wrap' ─────────────────────────────────────────────
var wrapSrc = new Float32Array(16);
for (var i = 0; i < 16; i++) wrapSrc[i] = i;
var wrapDst = new Float32Array(16);
img.stencil(wrapDst, wrapSrc,
            { data: new Float32Array([0, 0, 0, 0, 1, 0, 0, 0, 0]), w: 3, h: 3 },
            { srcW: 4, srcH: 4, edge: 'wrap' });
assert(wrapDst[0] === wrapSrc[0], 'stencil wrap identity');

// ── stencil with bias ────────────────────────────────────────────────────
var biasDst = new Float32Array(16);
img.stencil(biasDst, wrapSrc,
            { data: new Float32Array([0, 0, 0, 0, 1, 0, 0, 0, 0]), w: 3, h: 3 },
            { srcW: 4, srcH: 4, bias: 5 });
assertEqual(biasDst[5], wrapSrc[5] + 5, 'stencil bias adds');

// ── stencil bad edge rejects ─────────────────────────────────────────────
threw = false;
try {
    img.stencil(new Float32Array(16), new Float32Array(16),
                { data: new Float32Array(9), w: 3, h: 3 },
                { srcW: 4, srcH: 4, edge: 'invalid_edge_xyz' });
} catch (e) { threw = true; }
assert(threw, 'stencil bad edge rejects');

// ── resample with src too small rejects ─────────────────────────────────
threw = false;
try {
    img.resample(new Float32Array(16), new Float32Array(2),
                 { srcW: 4, srcH: 4, dstW: 4, dstH: 4, channels: 1, filter: 'nearest' });
} catch (e) { threw = true; }
assert(threw, 'resample src too small rejects');

// ── resample with dst too small rejects ─────────────────────────────────
threw = false;
try {
    img.resample(new Float32Array(2), new Float32Array(16),
                 { srcW: 4, srcH: 4, dstW: 4, dstH: 4, channels: 1, filter: 'nearest' });
} catch (e) { threw = true; }
assert(threw, 'resample dst too small rejects');

// ── resample with multi-channel (RGBA) ──────────────────────────────────
var rgbaSrc = new Float32Array(2 * 2 * 4);
for (var i = 0; i < rgbaSrc.length; i++) rgbaSrc[i] = i % 256;
var rgbaDst = new Float32Array(4 * 4 * 4);
img.resample(rgbaDst, rgbaSrc, { srcW: 2, srcH: 2, dstW: 4, dstH: 4, channels: 4, filter: 'nearest' });
assert(rgbaDst[0] === rgbaSrc[0], 'resample rgba nearest');

// ── reduce histogram with bins > 0 produces output ───────────────────────
var hist = img.reduce(new Float32Array([0.1, 0.3, 0.7, 0.9]), 'histogram',
                       { bins: 4, lo: 0, hi: 1 });
assert(hist.length === 4, 'histogram bin count');

// ── stencil with even kernel rejects ─────────────────────────────────────
threw = false;
try {
    img.stencil(new Float32Array(16), new Float32Array(16),
                { data: new Float32Array(4), w: 2, h: 2 },
                { srcW: 4, srcH: 4 });
} catch (e) { threw = true; }
assert(threw, 'stencil even-kernel rejects');
