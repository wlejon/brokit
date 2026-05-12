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

// Enum set with neither int nor string rejects ("Enum expects int or string")
var cell = FastNoise.create('CellularDistance');
threw = false;
try { cell.set('Distance Function', true); } catch (e) { threw = true; }
assert(threw, 'enum with bool rejects');

threw = false;
try { cell.set('Distance Function', {}); } catch (e) { threw = true; }
assert(threw, 'enum with object rejects');

// Per-dimension matching: DomainAxisScale has Scale X / Y / Z / W
// (If the type doesn't exist we just skip — exercise getMembers regardless.)
var domOff = FastNoise.create('DomainOffset');
var mems = domOff.getMembers();
assert(typeof mems === 'object' && mems !== null, 'getMembers returns object');
// Per-dimension members appear in variables and/or hybrids with "X/Y/Z/W" suffix
function tryPerDim(list) {
    if (!Array.isArray(list)) return;
    for (var mi = 0; mi < list.length; mi++) {
        var nm = list[mi].name;
        if (nm.length > 2 && nm.charAt(nm.length - 2) === ' ' &&
            'XYZW'.indexOf(nm.charAt(nm.length - 1)) >= 0) {
            try { domOff.set(nm, 1.5); } catch (e) {}
            try { domOff.set(nm + ' bogus', 1.5); } catch (e) {}
            return true;
        }
    }
    return false;
}
tryPerDim(mems.variables) || tryPerDim(mems.hybrids);

// Test wrong-suffix per-dim names — exercises queryIdx==-1 branch
try { domOff.set('OffsetX', 1.0); } catch (e) {}   // no space
try { domOff.set('Offset Q', 1.0); } catch (e) {}  // unknown dim letter
try { domOff.set('Offset XY', 1.0); } catch (e) {} // trailing chars

// Setting node source with mismatched node-type — try with a DomainOffset
// supplied where a generator is expected. The "type mismatch" branch fires
// when setFunc returns false.
var bigFbm = FastNoise.create('FractalFBm');
var domainOnly = FastNoise.create('DomainOffset');
try { bigFbm.set('Source', domainOnly); } catch (e) {}

// Hybrid type mismatch — set Gain to a non-FastNoise object via JS_GetOpaque
// returning null path is already covered. Try setting hybrid to NaN to exercise
// the failure return from setValueFunc.
try { bigFbm.set('Gain', NaN); } catch (e) {}
try { bigFbm.set('Gain', Infinity); } catch (e) {}

// set with a name longer than baseLen but no space — exercises early reject
try { domOff.set('OffsetXextra', 1.0); } catch (e) {}

// Generate from a configured fbm
var v = fbm.genSingle2D(1.0, 2.0, 42);
assert(typeof v === 'number', 'configured FBm generates value');
