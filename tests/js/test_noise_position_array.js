// genPositionArray2D / genPositionArray3D — arbitrary-position sampling.
//
// The contract that matters is that these agree with the lattice entry points:
// a position array laid out ON the lattice must reproduce genUniformGrid
// exactly, since it is the same field sampled at the same points. If it does
// not, the offset/frequency convention has drifted between the two paths.

const fbm = FastNoise.create('FractalFBm');
fbm.set('Source', FastNoise.create('Simplex'));
fbm.set('Octaves', 4);

// ---- 3D: position array on the lattice == genUniformGrid3D ----------------
{
    const N = 8, freq = 0.03, seed = 1337;
    const grid = fbm.genUniformGrid3D(0, 0, 0, N, N, N, freq, seed);

    // genUniformGrid3D walks x fastest, then y, then z, and multiplies each
    // integer lattice step by `frequency`. Position arrays take no frequency,
    // so the scaling is ours to apply.
    const n = N * N * N;
    const xs = new Float32Array(n), ys = new Float32Array(n), zs = new Float32Array(n);
    let i = 0;
    for (let z = 0; z < N; z++)
        for (let y = 0; y < N; y++)
            for (let x = 0; x < N; x++, i++) {
                xs[i] = x * freq; ys[i] = y * freq; zs[i] = z * freq;
            }

    const out = new Float32Array(n);
    fbm.genPositionArray3D(out, xs, ys, zs, 0, 0, 0, seed);

    let maxDiff = 0;
    for (let k = 0; k < n; k++) maxDiff = Math.max(maxDiff, Math.abs(out[k] - grid[k]));
    assert(maxDiff < 1e-6, `3D position array vs uniform grid: max diff ${maxDiff}`);
}

// ---- 2D: same check ------------------------------------------------------
{
    const N = 16, freq = 0.05, seed = 99;
    const grid = fbm.genUniformGrid2D(0, 0, N, N, freq, seed);
    const n = N * N;
    const xs = new Float32Array(n), ys = new Float32Array(n);
    let i = 0;
    for (let y = 0; y < N; y++)
        for (let x = 0; x < N; x++, i++) { xs[i] = x * freq; ys[i] = y * freq; }

    const out = new Float32Array(n);
    fbm.genPositionArray2D(out, xs, ys, 0, 0, seed);
    let maxDiff = 0;
    for (let k = 0; k < n; k++) maxDiff = Math.max(maxDiff, Math.abs(out[k] - grid[k]));
    assert(maxDiff < 1e-6, `2D position array vs uniform grid: max diff ${maxDiff}`);
}

// ---- agrees with genSingle3D at scattered points --------------------------
{
    const pts = [[0.5, -1.25, 3.0], [-7.5, 0.0, 0.125], [100.5, -100.5, 42.25]];
    const xs = new Float32Array(pts.map(p => p[0]));
    const ys = new Float32Array(pts.map(p => p[1]));
    const zs = new Float32Array(pts.map(p => p[2]));
    const out = new Float32Array(pts.length);
    fbm.genPositionArray3D(out, xs, ys, zs, 0, 0, 0, 7);
    for (let k = 0; k < pts.length; k++) {
        const single = fbm.genSingle3D(pts[k][0], pts[k][1], pts[k][2], 7);
        assert(Math.abs(out[k] - single) < 1e-6,
               `point ${k}: array ${out[k]} vs single ${single}`);
    }
}

// ---- offset is added to every position ------------------------------------
{
    const xs = new Float32Array([1.0]), ys = new Float32Array([2.0]), zs = new Float32Array([3.0]);
    const a = new Float32Array(1), b = new Float32Array(1);
    fbm.genPositionArray3D(a, xs, ys, zs, 0.5, -0.5, 0.25, 3);
    const xs2 = new Float32Array([1.5]), ys2 = new Float32Array([1.5]), zs2 = new Float32Array([3.25]);
    fbm.genPositionArray3D(b, xs2, ys2, zs2, 0, 0, 0, 3);
    assert(Math.abs(a[0] - b[0]) < 1e-6, `offset not folded in: ${a[0]} vs ${b[0]}`);
}

// ---- the count is the shortest position array, and dest is bounds-checked --
{
    const xs = new Float32Array(4), ys = new Float32Array(2), zs = new Float32Array(8);
    const out = new Float32Array(2);
    fbm.genPositionArray3D(out, xs, ys, zs, 0, 0, 0, 1);   // count = 2, fits

    let threw = false;
    try { fbm.genPositionArray3D(new Float32Array(1), xs, ys, zs, 0, 0, 0, 1); }
    catch (e) { threw = true; }
    assert(threw, 'undersized dest must throw');

    threw = false;
    try { fbm.genPositionArray3D(out, [0, 1], ys, zs, 0, 0, 0, 1); }
    catch (e) { threw = true; }
    assert(threw, 'a plain Array for xs must throw');
}

// ---- the motivating case: 3D noise on a sphere has no seam ----------------
//
// Two points either side of the +X/-Z cube-face boundary, a hair apart on the
// sphere, must return nearly equal values. A 2D-per-face scheme cannot do this.
{
    const R = 1.0;
    function sample(v) {
        const L = Math.hypot(v[0], v[1], v[2]);
        const xs = new Float32Array([v[0] / L * R]);
        const ys = new Float32Array([v[1] / L * R]);
        const zs = new Float32Array([v[2] / L * R]);
        const o = new Float32Array(1);
        fbm.genPositionArray3D(o, xs, ys, zs, 0, 0, 0, 4242);
        return o[0];
    }
    const eps = 1e-4;
    const a = sample([1, 0.3, -1 + eps]);   // on the +X face, at its -Z edge
    const b = sample([1 - eps, 0.3, -1]);   // on the -Z face, at its +X edge
    assert(Math.abs(a - b) < 1e-3,
           `cube-face seam: ${a} vs ${b} (diff ${Math.abs(a - b)})`);
}

console.log('test_noise_position_array: OK');
