// bro.image — typed-array kernel API tests

assert(typeof bro === "object", "bro global should exist");
assert(typeof bro.image === "object", "bro.image should exist");

// ==========================================================================
// gradient(stops, n)
// ==========================================================================

// 2-stop gradient: black (t=0) to white (t=1), 4 entries
var lut = bro.image.gradient([[0, 0, 0, 0], [1, 255, 255, 255]], 4);
assert(lut instanceof Uint8Array, "gradient returns Uint8Array");
assertEqual(lut.length, 16); // 4 entries × 4 bytes
// First entry = black
assertEqual(lut[0], 0); assertEqual(lut[1], 0); assertEqual(lut[2], 0);
// Last entry = white
assertEqual(lut[12], 255); assertEqual(lut[13], 255); assertEqual(lut[14], 255);
// Alpha defaults to 255
assertEqual(lut[3], 255); assertEqual(lut[15], 255);
// Middle (t=1/3, ~85): gray-ish (interpolated)
assert(lut[4] > 0 && lut[4] < 255, "midpoint should be interpolated");

// Default n = 256
var defLut = bro.image.gradient([[0, 0, 0, 0], [1, 255, 255, 255]]);
assertEqual(defLut.length, 1024);

// Throws on too few stops
var threw = false;
try { bro.image.gradient([[0, 0, 0, 0]]); } catch (e) { threw = true; }
assert(threw, "single stop should throw");

// ==========================================================================
// alloc
// ==========================================================================

var a = bro.image.alloc(4, 4, 1);
assert(a instanceof Float32Array);
assertEqual(a.length, 16);

var rgba = bro.image.alloc(2, 2, 4, "uint8c");
assert(rgba instanceof Uint8ClampedArray);
assertEqual(rgba.length, 16);

// ==========================================================================
// reduce
// ==========================================================================

var src = new Float32Array([1, 2, 3, 4, -1, 0]);
var mm = bro.image.reduce(src, "minmax");
assertEqual(mm.min, -1);
assertEqual(mm.max, 4);

assertEqual(bro.image.reduce(src, "sum"), 9);
assertEqual(bro.image.reduce(src, "mean"), 1.5);

// histogram: 4 bins over [0, 4); values 1,2,3,4,-1,0 -> bins=[0,1,1,1] (4 out of range, -1 out of range)
var hist = bro.image.reduce(src, "histogram", { bins: 4, lo: 0, hi: 4 });
assert(hist instanceof Uint32Array);
assertEqual(hist.length, 4);
assertEqual(hist[0], 1); // 0
assertEqual(hist[1], 1); // 1
assertEqual(hist[2], 1); // 2
assertEqual(hist[3], 1); // 3 ; 4 is out of range (idx==bins)

// ==========================================================================
// lookup — 2-stop black-to-white gradient, src normalized to [0,1]
// ==========================================================================

var lut2 = bro.image.gradient([[0, 0, 0, 0], [1, 255, 255, 255]], 256);
var fld = new Float32Array([0, 0.5, 1.0, 0.25]);
var out = new Uint8ClampedArray(fld.length * 4);
bro.image.lookup(out, fld, lut2, { lo: 0, hi: 1 });
// Pixel 0: black
assertEqual(out[0], 0); assertEqual(out[1], 0); assertEqual(out[2], 0); assertEqual(out[3], 255);
// Pixel 2: white
assertEqual(out[8], 255); assertEqual(out[9], 255); assertEqual(out[10], 255);
// Pixel 1 (t=0.5): mid gray
assert(out[4] > 100 && out[4] < 160, "mid-gray expected, got " + out[4]);

// ==========================================================================
// map
// ==========================================================================

var ms = new Float32Array([1, 2, 3, 4]);
var md = new Float32Array(4);
bro.image.map(md, ms, { op: "affine", a: 2, b: 1 });
assertEqual(md[0], 3); assertEqual(md[1], 5); assertEqual(md[2], 7); assertEqual(md[3], 9);

bro.image.map(md, ms, { op: "affine", a: 1, b: 0, clamp: [2, 3] });
assertEqual(md[0], 2); assertEqual(md[1], 2); assertEqual(md[2], 3); assertEqual(md[3], 3);

bro.image.map(md, new Float32Array([-1, 4, 9, 16]), { op: "abs" });
assertEqual(md[0], 1);

bro.image.map(md, new Float32Array([1, 4, 9, 16]), { op: "sqrt" });
assertEqual(md[0], 1); assertEqual(md[1], 2); assertEqual(md[2], 3); assertEqual(md[3], 4);

bro.image.map(md, new Float32Array([2, 3, 4, 5]), { op: "pow", exp: 2 });
assertEqual(md[0], 4); assertEqual(md[1], 9); assertEqual(md[2], 16); assertEqual(md[3], 25);

// ==========================================================================
// combine
// ==========================================================================

var ca = new Float32Array([1, 2, 3, 4]);
var cb = new Float32Array([10, 20, 30, 40]);
var cd = new Float32Array(4);
bro.image.combine(cd, ca, cb, { op: "add" });
assertEqual(cd[0], 11); assertEqual(cd[3], 44);

bro.image.combine(cd, ca, cb, { op: "lerp", t: 0.5 });
assertEqual(cd[0], 5.5); assertEqual(cd[3], 22);

bro.image.combine(cd, ca, cb, { op: "wsum", wa: 2, wb: 1 });
assertEqual(cd[0], 12); // 2*1 + 1*10
assertEqual(cd[3], 48); // 2*4 + 1*40

bro.image.combine(cd, ca, cb, { op: "min" });
assertEqual(cd[0], 1);
bro.image.combine(cd, ca, cb, { op: "max" });
assertEqual(cd[0], 10);

// ==========================================================================
// stencil — 3x3 box blur with divisor=9 on 4x4 constant field of 1s
// ==========================================================================

var sw = 4, sh = 4;
var ssrc = new Float32Array(sw * sh);
for (var i = 0; i < ssrc.length; i++) ssrc[i] = 1;
var sdst = new Float32Array(sw * sh);
var box = { data: new Float32Array([1,1,1, 1,1,1, 1,1,1]), w: 3, h: 3 };
bro.image.stencil(sdst, ssrc, box, { srcW: sw, srcH: sh, edge: "clamp", divisor: 9 });
// All 1s → all 1s
for (var i = 0; i < sdst.length; i++) {
    assert(Math.abs(sdst[i] - 1) < 1e-5, "box on flat 1s should be 1, got " + sdst[i] + " at " + i);
}

// Identity kernel
var identity = { data: new Float32Array([0,0,0, 0,1,0, 0,0,0]), w: 3, h: 3 };
var ramp = new Float32Array(sw * sh);
for (var i = 0; i < ramp.length; i++) ramp[i] = i;
bro.image.stencil(sdst, ramp, identity, { srcW: sw, srcH: sh, edge: "clamp" });
for (var i = 0; i < sdst.length; i++) assertEqual(sdst[i], ramp[i]);

// Zero edge mode: corner sums fewer elements
bro.image.stencil(sdst, ssrc, box, { srcW: sw, srcH: sh, edge: "zero", divisor: 1 });
// Corner (0,0): 4 valid neighbors out of 9 → sum=4
assertEqual(sdst[0], 4);

// ==========================================================================
// resample
// ==========================================================================

// Nearest 2x2 -> 4x4
var rs2 = new Float32Array([1, 2, 3, 4]);
var rd4 = new Float32Array(16);
bro.image.resample(rd4, rs2, { srcW: 2, srcH: 2, dstW: 4, dstH: 4, channels: 1, filter: "nearest" });
// Each src cell maps to 2x2 block
assertEqual(rd4[0], 1); assertEqual(rd4[1], 1);
assertEqual(rd4[2], 2); assertEqual(rd4[3], 2);

// Bilinear: identity 2x2 -> 2x2
var rd2 = new Float32Array(4);
bro.image.resample(rd2, rs2, { srcW: 2, srcH: 2, dstW: 2, dstH: 2, channels: 1, filter: "bilinear" });
assertEqual(rd2[0], 1); assertEqual(rd2[1], 2); assertEqual(rd2[2], 3); assertEqual(rd2[3], 4);
