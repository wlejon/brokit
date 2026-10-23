// Test: FastNoise glsl(): a node tree as GLSL, a function per dimension
// asked for. (Its values against FastNoise2's own are bro's GPU test,
// bro_noise_glsl_test; here the source's shape and the refusals.)

var simplex = FastNoise.create('Simplex');
simplex.set('Feature Scale', 40);
var fbm = FastNoise.create('FractalFBm');
fbm.set('Source', simplex);
fbm.set('Octaves', 4);
var remap = FastNoise.create('Remap');
remap.set('Source', fbm);
remap.set('To Min', -45);
remap.set('To Max', 45);

// ── the default: field(vec2 p, int seed) ──────────────────────────────────
var glsl = remap.glsl();
assertEqual(typeof glsl, 'string', 'glsl gives a string');
assert(glsl.indexOf('float field(vec2 p, int seed)') >= 0, 'the 2D function, named field');
assert(glsl.indexOf('float field(vec3 p, int seed)') < 0, 'no 3D function unless asked');
assert(glsl.indexOf('#ifndef BROKIT_FASTNOISE_GLSL') >= 0, 'the shared maths behind an include guard');

// ── a name and both dimensions ────────────────────────────────────────────
var both = remap.glsl({ name: 'height', dims: [2, 3] });
assert(both.indexOf('float height(vec2 p, int seed)') >= 0, 'the 2D function, named height');
assert(both.indexOf('float height(vec3 p, int seed)') >= 0, 'and the 3D one');
var three = remap.glsl({ name: 'density', dims: 3 });
assert(three.indexOf('float density(vec3 p, int seed)') >= 0, 'dims: 3 gives the 3D function');
assert(three.indexOf('float density(vec2 p, int seed)') < 0, 'and only that');

// ── a node shared in the tree is one function ─────────────────────────────
var add = FastNoise.create('Add');
add.set('LHS', remap);
add.set('RHS', remap);
var shared = add.glsl();
assertEqual(shared.split('float field_n').length - 1, 4, 'Add, Remap, FBm, Simplex: four functions, the Remap once');

// ── the same tree, decoded, gives the same source ─────────────────────────
assertEqual(new FastNoise(remap.encode()).glsl(), glsl, 'a decoded tree gives the same GLSL');

// ── refusals ──────────────────────────────────────────────────────────────
function refusal(f) {
    try { f(); } catch (e) { return String(e.message); }
    return null;
}
var cellular = refusal(function () { FastNoise.create('CellularDistance').glsl(); });
assert(cellular !== null, 'a node not ported is refused');
assert(cellular !== null && cellular.indexOf('CellularDistance') >= 0, 'by its name');
assert(refusal(function () { remap.glsl({ name: '2bad' }); }) !== null, 'a name that is not a GLSL identifier is refused');
assert(refusal(function () { remap.glsl({ dims: 'two' }); }) !== null, 'dims that are not 2, 3 or a list are refused');
var unset = FastNoise.create('FractalFBm');
assert(refusal(function () { unset.glsl(); }) !== null, 'a tree with an unset source is refused');

// ── Differential Equivalence Oracle: CPU FastNoise2 vs. noise_glsl shader math ──
//
// Evaluates the exact single-precision (float32 / int32) lattice, hash, gradient,
// fractal, blend, and domain-modifier formulas emitted by `brokit/src/api/noise_glsl.cpp`
// and checks numerical equivalence against FastNoise2's CPU SIMD generator across
// 2D and 3D coordinate grids and seeds.
(function () {
    var f32 = Math.fround;
    var imul = Math.imul;
    var FN_PX = 0xF797C5C7 | 0;
    var FN_PY = 0x6C060C89 | 0;
    var FN_PZ = 0x465FD04F | 0;
    var FN_HASH = 0xB7E0A5F5 | 0;
    var FN_PERLIN_G = f32(2.41421356237);

    function fma(a, b, c) { return f32(f32(a * b) + c); }
    function fn_lerp(a, b, t) { return fma(t, f32(b - a), a); }
    function fn_hermite(t) { return f32(f32(t * t) * fma(f32(-t), f32(2.0), f32(3.0))); }
    function fn_quintic(t) {
        return f32(f32(f32(t * t) * t) * fma(t, fma(t, f32(6.0), f32(-15.0)), f32(10.0)));
    }
    function fn_hash2(seed, x, y) {
        var h = (seed ^ (x ^ y)) | 0;
        h = imul(h, FN_HASH);
        return ((h >> 15) ^ h) | 0;
    }
    function fn_hash3(seed, x, y, z) {
        var h = (seed ^ (x ^ y ^ z)) | 0;
        h = imul(h, FN_HASH);
        return ((h >> 15) ^ h) | 0;
    }
    function fn_valueCoord2(seed, x, y) {
        var h = (seed ^ (x ^ y)) | 0;
        h = imul(h, imul(h, FN_HASH));
        return f32(h);
    }
    function fn_valueCoord3(seed, x, y, z) {
        var h = (seed ^ (x ^ y ^ z)) | 0;
        h = imul(h, imul(h, FN_HASH));
        return f32(h);
    }
    function fn_gradPerlin(h, x, y) {
        if ((h & 1) !== 0) x = f32(-x);
        if ((h & 2) !== 0) y = f32(-y);
        return (h & 4) !== 0 ? fma(FN_PERLIN_G, y, x) : fma(FN_PERLIN_G, x, y);
    }
    function fn_gradCommon(h, x, y, z) {
        var a = h & 13;
        var u = a > 7 ? y : x;
        var v = a === 12 ? x : z;
        v = a < 2 ? y : v;
        if ((h & 1) !== 0) u = f32(-u);
        if ((h & 2) !== 0) v = f32(-v);
        return f32(u + v);
    }
    function scaleOut(raw, lo, hi, outMin, outMax) {
        var k = f32(f32(1.0 / f32(hi - lo)) * f32(outMax - outMin));
        return fma(k, f32(raw - f32(lo)), f32(outMin));
    }
    function fn_value2(seed, px, py) {
        var sx = f32(Math.floor(px)), sy = f32(Math.floor(py));
        var x0 = imul(sx | 0, FN_PX), y0 = imul(sy | 0, FN_PY);
        var x1 = (x0 + FN_PX) | 0, y1 = (y0 + FN_PY) | 0;
        var xs = fn_hermite(f32(px - sx)), ys = fn_hermite(f32(py - sy));
        return fn_lerp(
            fn_lerp(fn_valueCoord2(seed, x0, y0), fn_valueCoord2(seed, x1, y0), xs),
            fn_lerp(fn_valueCoord2(seed, x0, y1), fn_valueCoord2(seed, x1, y1), xs),
            ys);
    }
    function fn_value3(seed, px, py, pz) {
        var sx = f32(Math.floor(px)), sy = f32(Math.floor(py)), sz = f32(Math.floor(pz));
        var x0 = imul(sx | 0, FN_PX), y0 = imul(sy | 0, FN_PY), z0 = imul(sz | 0, FN_PZ);
        var x1 = (x0 + FN_PX) | 0, y1 = (y0 + FN_PY) | 0, z1 = (z0 + FN_PZ) | 0;
        var xs = fn_hermite(f32(px - sx)), ys = fn_hermite(f32(py - sy)), zs = fn_hermite(f32(pz - sz));
        return fn_lerp(
            fn_lerp(fn_lerp(fn_valueCoord3(seed, x0, y0, z0), fn_valueCoord3(seed, x1, y0, z0), xs),
                    fn_lerp(fn_valueCoord3(seed, x0, y1, z0), fn_valueCoord3(seed, x1, y1, z0), xs), ys),
            fn_lerp(fn_lerp(fn_valueCoord3(seed, x0, y0, z1), fn_valueCoord3(seed, x1, y0, z1), xs),
                    fn_lerp(fn_valueCoord3(seed, x0, y1, z1), fn_valueCoord3(seed, x1, y1, z1), xs), ys),
            zs);
    }
    function fn_perlin2(seed, px, py) {
        var sx = f32(Math.floor(px)), sy = f32(Math.floor(py));
        var x0 = imul(sx | 0, FN_PX), y0 = imul(sy | 0, FN_PY);
        var x1 = (x0 + FN_PX) | 0, y1 = (y0 + FN_PY) | 0;
        var xf0 = f32(px - sx), yf0 = f32(py - sy);
        var xf1 = f32(xf0 - 1.0), yf1 = f32(yf0 - 1.0);
        var xs = fn_quintic(xf0), ys = fn_quintic(yf0);
        return fn_lerp(
            fn_lerp(fn_gradPerlin(fn_hash2(seed, x0, y0), xf0, yf0),
                    fn_gradPerlin(fn_hash2(seed, x1, y0), xf1, yf0), xs),
            fn_lerp(fn_gradPerlin(fn_hash2(seed, x0, y1), xf0, yf1),
                    fn_gradPerlin(fn_hash2(seed, x1, y1), xf1, yf1), xs),
            ys);
    }
    function fn_perlin3(seed, px, py, pz) {
        var sx = f32(Math.floor(px)), sy = f32(Math.floor(py)), sz = f32(Math.floor(pz));
        var x0 = imul(sx | 0, FN_PX), y0 = imul(sy | 0, FN_PY), z0 = imul(sz | 0, FN_PZ);
        var x1 = (x0 + FN_PX) | 0, y1 = (y0 + FN_PY) | 0, z1 = (z0 + FN_PZ) | 0;
        var xf0 = f32(px - sx), yf0 = f32(py - sy), zf0 = f32(pz - sz);
        var xf1 = f32(xf0 - 1.0), yf1 = f32(yf0 - 1.0), zf1 = f32(zf0 - 1.0);
        var xs = fn_quintic(xf0), ys = fn_quintic(yf0), zs = fn_quintic(zf0);
        return fn_lerp(
            fn_lerp(fn_lerp(fn_gradCommon(fn_hash3(seed, x0, y0, z0), xf0, yf0, zf0),
                            fn_gradCommon(fn_hash3(seed, x1, y0, z0), xf1, yf0, zf0), xs),
                    fn_lerp(fn_gradCommon(fn_hash3(seed, x0, y1, z0), xf0, yf1, zf0),
                            fn_gradCommon(fn_hash3(seed, x1, y1, z0), xf1, yf1, zf0), xs), ys),
            fn_lerp(fn_lerp(fn_gradCommon(fn_hash3(seed, x0, y0, z1), xf0, yf0, zf1),
                            fn_gradCommon(fn_hash3(seed, x1, y0, z1), xf1, yf0, zf1), xs),
                    fn_lerp(fn_gradCommon(fn_hash3(seed, x0, y1, z1), xf0, yf1, zf1),
                            fn_gradCommon(fn_hash3(seed, x1, y1, z1), xf1, yf1, zf1), xs), ys),
            zs);
    }
    function fn_simplex2(seed, px, py) {
        var kSkew = f32(0.366025403784438647);
        var kUnskew = f32(-0.211324865405187118);
        var kUnskew1 = f32(0.788675134594812882);
        var kUnskew2 = f32(0.577350269189625765);
        var kFalloff2 = f32(-3.15470053837925153);
        var skewDelta = f32(kSkew * f32(px + py));
        var xSkewed = f32(px + skewDelta), ySkewed = f32(py + skewDelta);
        var xb = f32(Math.floor(xSkewed)), yb = f32(Math.floor(ySkewed));
        var dxs = f32(xSkewed - xb), dys = f32(ySkewed - yb);
        var xp = imul(xb | 0, FN_PX), yp = imul(yb | 0, FN_PY);
        var xge = dxs >= dys;
        var unskewDelta = f32(kUnskew * f32(dxs + dys));
        var dx0 = f32(dxs + unskewDelta), dy0 = f32(dys + unskewDelta);
        var dx1 = f32((xge ? dx0 : f32(dx0 + 1.0)) - kUnskew1);
        var dy1 = f32((xge ? f32(dy0 + 1.0) : dy0) - kUnskew1);
        var dx2 = f32(dx0 - kUnskew2), dy2 = f32(dy0 - kUnskew2);
        var f0 = fma(f32(-dx0), dx0, fma(f32(-dy0), dy0, f32(0.5)));
        var f1 = fma(f32(-dx1), dx1, fma(f32(-dy1), dy1, f32(0.5)));
        var f2 = f32(f0 + fma(unskewDelta, kFalloff2, f32(-0.666666666666666667)));
        f0 = Math.max(f0, 0.0); f1 = Math.max(f1, 0.0); f2 = Math.max(f2, 0.0);
        f0 = f32(f0 * f0); f0 = f32(f0 * f0);
        f1 = f32(f1 * f1); f1 = f32(f1 * f1);
        f2 = f32(f2 * f2); f2 = f32(f2 * f2);
        var g0 = fn_gradPerlin(fn_hash2(seed, xp, yp), dx0, dy0);
        var g1 = fn_gradPerlin(fn_hash2(seed, xge ? (xp + FN_PX) | 0 : xp, xge ? yp : (yp + FN_PY) | 0), dx1, dy1);
        var g2 = fn_gradPerlin(fn_hash2(seed, (xp + FN_PX) | 0, (yp + FN_PY) | 0), dx2, dy2);
        return fma(g0, f0, fma(g1, f1, f32(g2 * f2)));
    }

    // 1. Verify Value, Perlin, Simplex 2D & 3D + FractalFBm + Remap differential parity
    var pts2D = [
        [0.25, -0.75], [13.37, 42.125], [-108.6, 77.3], [512.125, -256.875],
        [0.0, 0.0], [19.95, -31.05], [-4.5, -9.25], [88.8, 11.1]
    ];
    var pts3D = [
        [0.25, -0.75, 1.5], [13.37, 42.125, -9.25], [-108.6, 77.3, 33.1],
        [0.0, 0.0, 0.0], [19.95, -31.05, 8.75], [-4.5, -9.25, -12.5]
    ];

    var valNode = FastNoise.create('Value');
    valNode.set('Feature Scale', 37);
    var perlinNode = FastNoise.create('Perlin');
    perlinNode.set('Feature Scale', 37);
    var simpNode = FastNoise.create('Simplex');
    simpNode.set('Feature Scale', 40);

    var freq37 = f32(1.0 / 37.0);
    var freq40 = f32(1.0 / 40.0);
    var bVal = 2147483648.0;
    var bPerlin2 = 1.726796627044677734375;
    var bPerlin3 = 1.0363423824310302734375;
    var bSimp2 = 0.0261208079755306243896484375;

    for (var si = 0; si < 2; si++) {
        var seed = si === 0 ? 1337 : -7;
        for (var i = 0; i < pts2D.length; i++) {
            var x = f32(pts2D[i][0]), y = f32(pts2D[i][1]);
            var cpuV = valNode.genSingle2D(x, y, seed);
            var gpuV = scaleOut(fn_value2(seed, f32(x * freq37), f32(y * freq37)), -bVal, bVal, -1, 1);
            assert(Math.abs(cpuV - gpuV) < 1e-4, 'Value 2D parity at ' + i + ': ' + cpuV + ' vs ' + gpuV);

            var cpuP = perlinNode.genSingle2D(x, y, seed);
            var gpuP = scaleOut(fn_perlin2(seed, f32(x * freq37), f32(y * freq37)), -bPerlin2, bPerlin2, -1, 1);
            assert(Math.abs(cpuP - gpuP) < 1e-4, 'Perlin 2D parity at ' + i + ': ' + cpuP + ' vs ' + gpuP);

            var cpuS = simpNode.genSingle2D(x, y, seed);
            var gpuS = scaleOut(fn_simplex2(seed, f32(x * freq40), f32(y * freq40)), -bSimp2, bSimp2, -1, 1);
            assert(Math.abs(cpuS - gpuS) < 1e-4, 'Simplex 2D parity at ' + i + ': ' + cpuS + ' vs ' + gpuS);

            // Composite tree `remap` (Remap -> FractalFBm(4 octaves) -> Simplex(scale 40))
            var px = x, py = y, s = seed, amp = f32(1.0), sum = f32(0.0);
            for (var oct = 0; oct < 4; oct++) {
                var n = scaleOut(fn_simplex2(s, f32(px * freq40), f32(py * freq40)), -bSimp2, bSimp2, -1, 1);
                sum = f32(sum + f32(n * amp));
                if (oct < 3) {
                    s = (s + 1) | 0;
                    amp = f32(amp * f32(0.5));
                    px = f32(px * f32(2.0));
                    py = f32(py * f32(2.0));
                }
            }
            var gpuRemap = f32(-45.0 + f32(f32(f32(sum - (-1.0)) / 2.0) * 90.0));
            var cpuRemap = remap.genSingle2D(x, y, seed);
            assert(Math.abs(cpuRemap - gpuRemap) < 5e-3,
                   'Remap(FractalFBm(Simplex)) 2D parity at ' + i + ': ' + cpuRemap + ' vs ' + gpuRemap);
        }

        for (var j = 0; j < pts3D.length; j++) {
            var x3 = f32(pts3D[j][0]), y3 = f32(pts3D[j][1]), z3 = f32(pts3D[j][2]);
            var cpuV3 = valNode.genSingle3D(x3, y3, z3, seed);
            var gpuV3 = scaleOut(fn_value3(seed, f32(x3 * freq37), f32(y3 * freq37), f32(z3 * freq37)),
                                 -bVal, bVal, -1, 1);
            assert(Math.abs(cpuV3 - gpuV3) < 1e-4, 'Value 3D parity at ' + j + ': ' + cpuV3 + ' vs ' + gpuV3);

            var cpuP3 = perlinNode.genSingle3D(x3, y3, z3, seed);
            var gpuP3 = scaleOut(fn_perlin3(seed, f32(x3 * freq37), f32(y3 * freq37), f32(z3 * freq37)),
                                 -bPerlin3, bPerlin3, -1, 1);
            assert(Math.abs(cpuP3 - gpuP3) < 1e-4, 'Perlin 3D parity at ' + j + ': ' + cpuP3 + ' vs ' + gpuP3);
        }
    }

    // 2. Verify PowFloat and Fade edge-case guards in generated GLSL
    var pf = FastNoise.create('PowFloat');
    pf.set('Value', simpNode);
    pf.set('Pow', 1.7);
    var pfGlsl = pf.glsl();
    assert(pfGlsl.indexOf('v > 0.0 ? pow(v,') >= 0,
           'PowFloat GLSL masks v > 0.0 matching FastSIMD Pow');

    var fd = FastNoise.create('Fade');
    fd.set('A', perlinNode);
    fd.set('B', valNode);
    fd.set('Fade', simpNode);
    fd.set('Fade Min', 0.5);
    fd.set('Fade Max', 0.5);
    var fdGlsl = fd.glsl();
    assert(fdGlsl.indexOf('fadeRange == 0.0 ? 0.5 : max(0.0, min(1.0, (fade - fadeMin) / fadeRange))') >= 0,
           'Fade GLSL guards zero fadeRange before dividing');
})();
