// Test: FastNoise validation paths and genUniformGrid3DInto

var gen = FastNoise.create('Simplex');

// ── genUniformGrid3DInto — in-place fill ──────────────────────────────────
var dest = new Float32Array(8 * 8 * 8);
var ret = gen.genUniformGrid3DInto(dest, 0, 0, 0, 8, 8, 8, 0.05, 1337);
assertEqual(ret, undefined, 'genUniformGrid3DInto returns undefined');
// Sanity: at least some values should be non-zero
var nonZero = 0;
for (var i = 0; i < dest.length; i++) if (dest[i] !== 0) nonZero++;
assert(nonZero > 0, 'genUniformGrid3DInto wrote values');

// ── genUniformGrid3DInto wrong typed-array type rejects ───────────────────
var threw = false;
try {
    var bad = new Float64Array(64);  // bpe=8, won't match sizeof(float)
    gen.genUniformGrid3DInto(bad, 0, 0, 0, 4, 4, 4, 0.05, 1337);
} catch (e) { threw = true; }
assert(threw, 'genUniformGrid3DInto rejects non-Float32 array');

// ── genUniformGrid3DInto not a typed array rejects ────────────────────────
threw = false;
try { gen.genUniformGrid3DInto({}, 0, 0, 0, 4, 4, 4, 0.05, 1337); } catch (e) { threw = true; }
assert(threw, 'genUniformGrid3DInto rejects non-typed-array');

// ── genUniformGrid3DInto too-small dest rejects ───────────────────────────
threw = false;
try {
    var small = new Float32Array(4);
    gen.genUniformGrid3DInto(small, 0, 0, 0, 4, 4, 4, 0.05, 1337);
} catch (e) { threw = true; }
assert(threw, 'genUniformGrid3DInto rejects too-small dest');

// ── genUniformGrid3DInto negative dim rejects ─────────────────────────────
threw = false;
try {
    gen.genUniformGrid3DInto(dest, 0, 0, 0, -1, 4, 4, 0.05, 1337);
} catch (e) { threw = true; }
assert(threw, 'genUniformGrid3DInto rejects negative dim');

// ── genUniformGrid3DInto missing args rejects ─────────────────────────────
threw = false;
try { gen.genUniformGrid3DInto(dest, 0, 0, 0); } catch (e) { threw = true; }
assert(threw, 'genUniformGrid3DInto missing args rejects');

// ── argument-count validation on all generators ──────────────────────────
threw = false;
try { gen.genSingle2D(); } catch (e) { threw = true; }
assert(threw, 'genSingle2D needs args');

threw = false;
try { gen.genSingle3D(0, 0); } catch (e) { threw = true; }
assert(threw, 'genSingle3D needs args');

threw = false;
try { gen.genUniformGrid2D(0); } catch (e) { threw = true; }
assert(threw, 'genUniformGrid2D needs args');

threw = false;
try { gen.genUniformGrid3D(0, 0, 0); } catch (e) { threw = true; }
assert(threw, 'genUniformGrid3D needs args');

threw = false;
try { gen.genTileable2D(0); } catch (e) { threw = true; }
assert(threw, 'genTileable2D needs args');

// ── genTileable2D zero dim rejects ────────────────────────────────────────
threw = false;
try { gen.genTileable2D(0, 16, 0.1, 1337); } catch (e) { threw = true; }
assert(threw, 'genTileable2D zero dim rejects');

// ── FastNoise.create wrong arg count rejects ──────────────────────────────
threw = false;
try { FastNoise.create(); } catch (e) { threw = true; }
assert(threw, 'FastNoise.create() needs name');

// ── set() with various types — exercises set-variable, set-enum branches ──
// FractalFBm has float members (Gain, Lacunarity, etc.) and a source.
var fbm = FastNoise.create('FractalFBm');
var src = FastNoise.create('Simplex');
fbm.set('Source', src);
fbm.set('Gain', 0.5);
fbm.set('Lacunarity', 2.0);
fbm.set('Octaves', 4);

// set with invalid member name should throw
threw = false;
try { fbm.set('NoSuchMember', 1); } catch (e) { threw = true; }
assert(threw, 'set unknown member throws');

// set with no args
threw = false;
try { fbm.set(); } catch (e) { threw = true; }
assert(threw, 'set with no args throws');

// Hybrid member (float-or-node)
var domScale = FastNoise.create('DomainScale');
domScale.set('Source', src);

// Generate from a configured fbm
var v = fbm.genSingle2D(1.0, 2.0, 42);
assert(typeof v === 'number', 'configured FBm generates value');
