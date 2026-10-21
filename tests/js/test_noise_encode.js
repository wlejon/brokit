// Test: FastNoise encode() and new FastNoise(encoded): a node tree as data,
// the same noise wherever it is decoded.

function grid(node) { return node.genUniformGrid2D(-8, -8, 16, 16, 0.37, 1337); }
function same(a, b) {
    if (a.length !== b.length) return false;
    for (var i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

// ── a tree: variables, a source, a hybrid driven by a node ────────────────
var simplex = FastNoise.create('Simplex');
simplex.set('Feature Scale', 3.5);
var fbm = FastNoise.create('FractalFBm');
fbm.set('Source', simplex);
fbm.set('Octaves', 4);
fbm.set('Gain', FastNoise.create('Value'));
var remap = FastNoise.create('Remap');
remap.set('Source', fbm);
remap.set('To Min', -2);
remap.set('To Max', 5);
var point = FastNoise.create('DistanceToPoint');
point.set('Distance Function', 'Max Axis');
point.set('Point X', 3);
var sum = FastNoise.create('Add');
sum.set('LHS', remap);
sum.set('RHS', point);

var encoded = sum.encode();
assertEqual(typeof encoded, 'string', 'encode gives a string');
assert(encoded.length > 0, 'encode gives a non-empty string');
var decoded = new FastNoise(encoded);
assert(same(grid(sum), grid(decoded)), 'the decoded tree makes the same noise');

// ── a source changed after it is set changes the encoding ─────────────────
simplex.set('Feature Scale', 9);
var again = sum.encode();
assert(again !== encoded, 'a source set later is encoded');
assert(same(grid(sum), grid(new FastNoise(again))), 'and decodes to the same noise');

// ── a hybrid set back to a constant drops its node ─────────────────────────
fbm.set('Gain', 0.3);
assert(same(grid(sum), grid(new FastNoise(sum.encode()))), 'a hybrid constant again encodes as one');

// ── a decoded tree re-encodes, and takes sets ──────────────────────────────
assertEqual(decoded.encode(), encoded, 'a decoded tree encodes as it was');
decoded.set('RHS', FastNoise.create('Constant'));
var swapped = new FastNoise(decoded.encode());
assert(same(grid(decoded), grid(swapped)), 'a decoded tree changed encodes the change');

// ── a node at its defaults, or set to them, makes its decoding's noise ────
// (an encoding leaves out what equals the metadata's default, so a created
// node takes the metadata's defaults, whatever its class's own are)
var plain = FastNoise.create('DistanceToPoint');
assert(same(grid(plain), grid(new FastNoise(plain.encode()))), 'a default node decodes to the same noise');
var euclid = FastNoise.create('DistanceToPoint');
euclid.set('Distance Function', 'Euclidean');
assert(Math.abs(euclid.genSingle2D(3, 4, 1) - 5) < 1e-3, 'Euclidean is the distance');
assert(same(grid(euclid), grid(new FastNoise(euclid.encode()))), 'a set distance function decodes to the same noise');

// ── a required source left unset cannot encode ─────────────────────────────
var threw = false;
try { FastNoise.create('FractalFBm').encode(); } catch (e) { threw = true; }
assert(threw, 'encode refuses a tree with an unset source');
