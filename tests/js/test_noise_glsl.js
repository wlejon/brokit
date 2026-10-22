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
