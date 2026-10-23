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

// ---- Large parallel grid (>= 64x64) and position array (>= 4096) agreement --
{
    const W = 128, H = 128, freq = 0.02, seed = 1337;
    const n = W * H;
    const t0 = Date.now();
    const grid2d = fbm.genUniformGrid2D(0, 0, W, H, freq, seed);
    const grid2dInto = new Float32Array(n);
    fbm.genUniformGrid2DInto(grid2dInto, 0, 0, W, H, freq, seed);
    const xs = new Float32Array(n), ys = new Float32Array(n);
    let idx = 0;
    for (let y = 0; y < H; y++) {
        for (let x = 0; x < W; x++, idx++) {
            xs[idx] = x * freq;
            ys[idx] = y * freq;
        }
    }
    const posOut = new Float32Array(n);
    fbm.genPositionArray2D(posOut, xs, ys, 0, 0, seed);
    let maxDiff = 0, maxIntoDiff = 0;
    for (let k = 0; k < n; k++) {
        maxDiff = Math.max(maxDiff, Math.abs(posOut[k] - grid2d[k]));
        maxIntoDiff = Math.max(maxIntoDiff, Math.abs(grid2dInto[k] - grid2d[k]));
    }
    assert(maxIntoDiff === 0, `parallel 2D grid vs Into must match bit-for-bit, got ${maxIntoDiff}`);
    assert(maxDiff < 1e-5, `parallel 128x128 2D grid vs position array: max diff ${maxDiff}`);

    // 3D large parallel grid (32x32x16 = 16384 >= 4096)
    const DX = 32, DY = 32, DZ = 16;
    const n3 = DX * DY * DZ;
    const grid3d = fbm.genUniformGrid3D(0, 0, 0, DX, DY, DZ, freq, seed);
    const xs3 = new Float32Array(n3), ys3 = new Float32Array(n3), zs3 = new Float32Array(n3);
    let i3 = 0;
    for (let z = 0; z < DZ; z++)
        for (let y = 0; y < DY; y++)
            for (let x = 0; x < DX; x++, i3++) {
                xs3[i3] = x * freq; ys3[i3] = y * freq; zs3[i3] = z * freq;
            }
    const posOut3 = new Float32Array(n3);
    fbm.genPositionArray3D(posOut3, xs3, ys3, zs3, 0, 0, 0, seed);
    let maxDiff3 = 0;
    for (let k = 0; k < n3; k++) maxDiff3 = Math.max(maxDiff3, Math.abs(posOut3[k] - grid3d[k]));
    assert(maxDiff3 < 1e-5, `parallel 32x32x16 3D grid vs position array: max diff ${maxDiff3}`);
    const elapsedMs = Date.now() - t0;
    console.log(`parallel grid/pos-array 128x128 + 32x32x16 verified in ${elapsedMs}ms`);
}

console.log('test_noise_position_array: OK');
