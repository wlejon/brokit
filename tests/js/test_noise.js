// FastNoise2 noise generation API tests

// Encoded node trees from FastNoise2 demo (valid encoded strings)
var SIMPLEX_ENCODED = "E@BBZEG@BD8JFgIECArXIzwECiQIw/UoPwkuAAE@BJDQAH@BC@AIEAJBw@ABZEED0KV78YZmZmPwQDmpkZPwsAAIA/HAMAAHBCBA==";

// ==========================================================================
// Constructor (encoded node tree)
// ==========================================================================

var gen = new FastNoise(SIMPLEX_ENCODED);
assert(gen instanceof FastNoise, "should be FastNoise instance");

var threw = false;
try { new FastNoise("not-valid"); } catch (e) { threw = true; }
assert(threw, "invalid encoded string should throw");

threw = false;
try { new FastNoise(); } catch (e) { threw = true; }
assert(threw, "missing argument should throw");

// ==========================================================================
// Generation methods
// ==========================================================================

// --- genSingle2D ---
var val = gen.genSingle2D(0, 0, 1337);
assert(typeof val === "number", "genSingle2D should return a number");
assert(val >= -1.5 && val <= 1.5, "genSingle2D value should be in reasonable range: " + val);

var val2 = gen.genSingle2D(0, 0, 1337);
assertEqual(val, val2);

var val3 = gen.genSingle2D(100.5, 200.5, 1337);
assert(val3 !== val, "different coords should produce different values");

// --- genSingle3D ---
var val3d = gen.genSingle3D(1.0, 2.0, 3.0, 42);
assert(typeof val3d === "number", "genSingle3D should return a number");
assert(val3d >= -1.5 && val3d <= 1.5, "genSingle3D value should be in reasonable range: " + val3d);

// --- genUniformGrid2D ---
var grid = gen.genUniformGrid2D(0, 0, 16, 16, 0.01, 1337);
assert(grid instanceof Float32Array, "genUniformGrid2D should return Float32Array");
assertEqual(grid.length, 256);

for (var i = 0; i < grid.length; i++) {
    assert(grid[i] >= -2.0 && grid[i] <= 2.0,
        "grid value out of range at " + i + ": " + grid[i]);
}

// --- genUniformGrid3D ---
var grid3d = gen.genUniformGrid3D(0, 0, 0, 8, 8, 8, 0.01, 1337);
assert(grid3d instanceof Float32Array, "genUniformGrid3D should return Float32Array");
assertEqual(grid3d.length, 512);

// --- genTileable2D ---
var tiled = gen.genTileable2D(16, 16, 0.1, 1337);
assert(tiled instanceof Float32Array, "genTileable2D should return Float32Array");
assertEqual(tiled.length, 256);

// --- Edge cases ---
threw = false;
try { gen.genUniformGrid2D(0, 0, 0, 16, 0.01, 1337); } catch (e) { threw = true; }
assert(threw, "zero xSize should throw");

threw = false;
try { gen.genUniformGrid3D(0, 0, 0, 4, 4, -1, 0.01, 1337); } catch (e) { threw = true; }
assert(threw, "negative dimension should throw");

// ==========================================================================
// FastNoise.types() — list all available node types
// ==========================================================================

var types = FastNoise.types();
assert(Array.isArray(types), "types() should return an array");
assert(types.length > 20, "should have many registered types, got " + types.length);

// Each entry should have name and groups
var firstType = types[0];
assert(typeof firstType.name === "string", "type entry should have name string");
assert(Array.isArray(firstType.groups), "type entry should have groups array");

// Known types should be present
var typeNames = types.map(function(t) { return t.name; });
assert(typeNames.indexOf("Simplex") >= 0, "Simplex should be in types");
assert(typeNames.indexOf("Perlin") >= 0, "Perlin should be in types");
assert(typeNames.indexOf("FractalFBm") >= 0, "FractalFBm should be in types");
assert(typeNames.indexOf("Add") >= 0, "Add should be in types");
assert(typeNames.indexOf("Multiply") >= 0, "Multiply should be in types");
assert(typeNames.indexOf("Remap") >= 0, "Remap should be in types");
assert(typeNames.indexOf("DomainScale") >= 0, "DomainScale should be in types");

// ==========================================================================
// FastNoise.create(name) — generic factory
// ==========================================================================

var cs = FastNoise.create("Simplex");
assert(cs instanceof FastNoise, "create('Simplex') should return FastNoise");
var csv = cs.genSingle2D(1, 1, 1);
assert(typeof csv === "number", "created Simplex should generate");

var cp = FastNoise.create("Perlin");
assert(cp instanceof FastNoise, "create('Perlin') should return FastNoise");

// Create all major types
var createTypes = [
    "Simplex", "SuperSimplex", "Perlin", "Value",
    "CellularValue", "CellularDistance", "CellularLookup",
    "FractalFBm", "FractalRidged",
    "DomainWarpGradient",
    "DomainScale", "DomainOffset", "DomainRotate",
    "Add", "Subtract", "Multiply", "Divide", "Min", "Max",
    "MinSmooth", "MaxSmooth", "Fade",
    "PowFloat", "PowInt",
    "Remap", "Terrace",
    "Constant", "White", "Checkerboard", "SineWave",
    "SeedOffset", "GeneratorCache",
    "DomainWarpFractalProgressive", "DomainWarpFractalIndependent"
];
for (var i = 0; i < createTypes.length; i++) {
    var n = FastNoise.create(createTypes[i]);
    assert(n instanceof FastNoise, "create('" + createTypes[i] + "') should return FastNoise");
}

// Unknown type should throw
threw = false;
try { FastNoise.create("NonExistentType"); } catch (e) { threw = true; }
assert(threw, "create with unknown type should throw");

// ==========================================================================
// Named convenience factories (same as before)
// ==========================================================================

var simplex = FastNoise.Simplex();
assert(simplex instanceof FastNoise, "Simplex() shortcut works");
var perlin = FastNoise.Perlin();
assert(perlin instanceof FastNoise, "Perlin() shortcut works");
var ssimplex = FastNoise.SuperSimplex();
assert(ssimplex instanceof FastNoise, "SuperSimplex() shortcut works");
var valueGen = FastNoise.Value();
assert(valueGen instanceof FastNoise, "Value() shortcut works");
var cellVal = FastNoise.CellularValue();
assert(cellVal instanceof FastNoise, "CellularValue() shortcut works");
var cellDist = FastNoise.CellularDistance();
assert(cellDist instanceof FastNoise, "CellularDistance() shortcut works");
var cellLookup = FastNoise.CellularLookup();
assert(cellLookup instanceof FastNoise, "CellularLookup() shortcut works");
var fbmShort = FastNoise.FractalFBm();
assert(fbmShort instanceof FastNoise, "FractalFBm() shortcut works");
var ridgedShort = FastNoise.FractalRidged();
assert(ridgedShort instanceof FastNoise, "FractalRidged() shortcut works");
var warpShort = FastNoise.DomainWarpGradient();
assert(warpShort instanceof FastNoise, "DomainWarpGradient() shortcut works");

// Distinct generators produce different output
var s1 = FastNoise.Simplex().genSingle2D(50, 50, 1);
var s2 = FastNoise.Perlin().genSingle2D(50, 50, 1);
assert(s1 !== s2, "Simplex and Perlin should differ");

// ==========================================================================
// getMembers() — introspection
// ==========================================================================

var fbmMembers = FastNoise.FractalFBm().getMembers();
assert(fbmMembers.type === "FractalFBm", "getMembers type should be FractalFBm");
assert(Array.isArray(fbmMembers.variables), "should have variables array");
assert(Array.isArray(fbmMembers.nodes), "should have nodes array");
assert(Array.isArray(fbmMembers.hybrids), "should have hybrids array");

// FractalFBm should have Source as a node lookup
var hasSource = false;
for (var i = 0; i < fbmMembers.nodes.length; i++) {
    if (fbmMembers.nodes[i].name === "Source") hasSource = true;
}
assert(hasSource, "FractalFBm should have Source node");

// FractalFBm should have Gain as a hybrid
var hasGain = false;
for (var i = 0; i < fbmMembers.hybrids.length; i++) {
    if (fbmMembers.hybrids[i].name === "Gain") hasGain = true;
}
assert(hasGain, "FractalFBm should have Gain hybrid");

// FractalFBm should have Octaves as a variable
var hasOctaves = false;
for (var i = 0; i < fbmMembers.variables.length; i++) {
    if (fbmMembers.variables[i].name === "Octaves") hasOctaves = true;
}
assert(hasOctaves, "FractalFBm should have Octaves variable");

// CellularDistance should have DistanceFunction as enum variable
var cdMembers = FastNoise.CellularDistance().getMembers();
var hasDistFunc = false;
var distFuncEntry = null;
for (var i = 0; i < cdMembers.variables.length; i++) {
    if (cdMembers.variables[i].name === "Distance Function") {
        hasDistFunc = true;
        distFuncEntry = cdMembers.variables[i];
    }
}
assert(hasDistFunc, "CellularDistance should have Distance Function");
assertEqual(distFuncEntry.type, "enum");
assert(Array.isArray(distFuncEntry.enumValues), "enum should have values");
assert(distFuncEntry.enumValues.length > 0, "enum values should not be empty");

// ==========================================================================
// set(name, value) — generic metadata-driven property setter
// ==========================================================================

// --- Node connections (MemberNodeLookup) ---
var fbm = FastNoise.create("FractalFBm");
var src = FastNoise.create("Simplex");
fbm.set("Source", src);
var fv = fbm.genSingle2D(10, 10, 1337);
assert(typeof fv === "number", "FractalFBm with Source should generate");

// --- Int variable (Octaves) ---
fbm.set("Octaves", 3);
var ov3 = fbm.genSingle2D(5, 5, 1);
fbm.set("Octaves", 8);
var ov8 = fbm.genSingle2D(5, 5, 1);
assert(ov3 !== ov8, "Different octave counts should produce different output");

// --- Float variable (Lacunarity) ---
fbm.set("Lacunarity", 1.5);
var lv1 = fbm.genSingle2D(5, 5, 1);
fbm.set("Lacunarity", 3.0);
var lv2 = fbm.genSingle2D(5, 5, 1);
assert(lv1 !== lv2, "Different lacunarities should produce different output");

// --- Hybrid with float value (Gain) ---
fbm.set("Gain", 0.3);
var gv1 = fbm.genSingle2D(5, 5, 1);
fbm.set("Gain", 0.9);
var gv2 = fbm.genSingle2D(5, 5, 1);
assert(gv1 !== gv2, "Different gains should produce different output");

// --- Hybrid with node value (Gain driven by noise) ---
var gainNode = FastNoise.create("Simplex");
fbm.set("Gain", gainNode);
var gv3 = fbm.genSingle2D(5, 5, 1);
assert(typeof gv3 === "number", "Gain as node should still generate");

// --- Hybrid: WeightedStrength ---
fbm.set("Weighted Strength", 0.0);
var wsv1 = fbm.genSingle2D(5, 5, 1);
fbm.set("Weighted Strength", 1.0);
var wsv2 = fbm.genSingle2D(5, 5, 1);
assert(wsv1 !== wsv2, "Different weighted strengths should differ");

// --- Enum variable by string name ---
var cell = FastNoise.create("CellularDistance");
cell.set("Distance Function", "Manhattan");
var cv1 = cell.genSingle2D(5.5, 5.5, 1);
cell.set("Distance Function", "Euclidean");
var cv2 = cell.genSingle2D(5.5, 5.5, 1);
assert(cv1 !== cv2, "Different distance functions should produce different output");

// --- Enum variable by int index ---
cell.set("Distance Function", 0);
var cv3 = cell.genSingle2D(5.5, 5.5, 1);
assert(typeof cv3 === "number", "Enum by index should work");

// Invalid enum string should throw
threw = false;
try { cell.set("Distance Function", "InvalidEnum"); } catch (e) { threw = true; }
assert(threw, "Invalid enum string should throw");

// --- Unknown member should throw ---
threw = false;
try { fbm.set("NonExistent", 5); } catch (e) { threw = true; }
assert(threw, "set with unknown member should throw");

// --- set requires FastNoise node for node lookups ---
threw = false;
try { fbm.set("Source", "not a node"); } catch (e) { threw = true; }
assert(threw, "set Source with string should throw");

// ==========================================================================
// Operator/blend nodes via create() + set()
// ==========================================================================

// --- Add ---
var add = FastNoise.create("Add");
add.set("LHS", FastNoise.create("Simplex"));
add.set("RHS", 0.5);
var addVal = add.genSingle2D(5, 5, 1);
assert(typeof addVal === "number", "Add node should generate");

// RHS can also be a node
add.set("RHS", FastNoise.create("Perlin"));
var addVal2 = add.genSingle2D(5, 5, 1);
assert(typeof addVal2 === "number", "Add with two node sources should generate");

// --- Multiply ---
var mul = FastNoise.create("Multiply");
mul.set("LHS", FastNoise.create("Simplex"));
mul.set("RHS", 2.0);
var mulVal = mul.genSingle2D(5, 5, 1);
assert(typeof mulVal === "number", "Multiply node should generate");

// --- Min ---
var mn = FastNoise.create("Min");
mn.set("LHS", FastNoise.create("Simplex"));
mn.set("RHS", FastNoise.create("Perlin"));
var mnVal = mn.genSingle2D(5, 5, 1);
assert(typeof mnVal === "number", "Min node should generate");

// --- Max ---
var mx = FastNoise.create("Max");
mx.set("LHS", FastNoise.create("Simplex"));
mx.set("RHS", FastNoise.create("Perlin"));
var mxVal = mx.genSingle2D(5, 5, 1);
assert(typeof mxVal === "number", "Max node should generate");

// --- Subtract ---
var sub = FastNoise.create("Subtract");
sub.set("LHS", FastNoise.create("Simplex"));
sub.set("RHS", FastNoise.create("Perlin"));
var subVal = sub.genSingle2D(5, 5, 1);
assert(typeof subVal === "number", "Subtract node should generate");

// --- Divide ---
var div = FastNoise.create("Divide");
div.set("LHS", FastNoise.create("Simplex"));
div.set("RHS", 2.0);
var divVal = div.genSingle2D(5, 5, 1);
assert(typeof divVal === "number", "Divide node should generate");

// --- Fade ---
var fade = FastNoise.create("Fade");
fade.set("A", FastNoise.create("Simplex"));
fade.set("B", FastNoise.create("Perlin"));
fade.set("Fade", 0.5);
var fadeVal = fade.genSingle2D(5, 5, 1);
assert(typeof fadeVal === "number", "Fade node should generate");

// --- MinSmooth / MaxSmooth ---
var ms = FastNoise.create("MinSmooth");
ms.set("LHS", FastNoise.create("Simplex"));
ms.set("RHS", FastNoise.create("Perlin"));
ms.set("Smoothness", 0.5);
var msVal = ms.genSingle2D(5, 5, 1);
assert(typeof msVal === "number", "MinSmooth should generate");

// ==========================================================================
// Modifier nodes
// ==========================================================================

// --- Remap ---
var remap = FastNoise.create("Remap");
remap.set("Source", FastNoise.create("Simplex"));
remap.set("From Min", -1.0);
remap.set("From Max", 1.0);
remap.set("To Min", 0.0);
remap.set("To Max", 1.0);
var rmVal = remap.genSingle2D(5, 5, 1);
assert(typeof rmVal === "number", "Remap should generate");

// --- DomainScale ---
var ds = FastNoise.create("DomainScale");
ds.set("Source", FastNoise.create("Simplex"));
ds.set("Scaling", 2.0);
var dsVal = ds.genSingle2D(5, 5, 1);
assert(typeof dsVal === "number", "DomainScale should generate");

// --- Terrace ---
var terr = FastNoise.create("Terrace");
terr.set("Source", FastNoise.create("Simplex"));
terr.set("Smoothness", 0.5);
var terrVal = terr.genSingle2D(5, 5, 1);
assert(typeof terrVal === "number", "Terrace should generate");

// --- Constant ---
var constant = FastNoise.create("Constant");
constant.set("Value", 0.75);
var constVal = constant.genSingle2D(0, 0, 0);
// Constant should return the set value everywhere
var constVal2 = constant.genSingle2D(100, 100, 999);
assertEqual(constVal, constVal2);

// --- SeedOffset modifier ---
var so = FastNoise.create("SeedOffset");
so.set("Source", FastNoise.create("Simplex"));
so.set("Seed Offset", 42);
var soVal = so.genSingle2D(5, 5, 1);
assert(typeof soVal === "number", "SeedOffset should generate");

// --- GeneratorCache ---
var gc = FastNoise.create("GeneratorCache");
gc.set("Source", FastNoise.create("Simplex"));
var gcVal = gc.genSingle2D(5, 5, 1);
assert(typeof gcVal === "number", "GeneratorCache should generate");

// --- DomainWarpGradient with set() ---
var dw = FastNoise.create("DomainWarpGradient");
dw.set("Source", FastNoise.create("Simplex"));
dw.set("Warp Amplitude", 10.0);
var dwv1 = dw.genSingle2D(5, 5, 1);
dw.set("Warp Amplitude", 100.0);
var dwv2 = dw.genSingle2D(5, 5, 1);
assert(dwv1 !== dwv2, "Different warp amplitudes should differ");

// ==========================================================================
// Domain warp fractal nodes
// ==========================================================================

var dwfp = FastNoise.create("DomainWarpFractalProgressive");
var dwGrad = FastNoise.create("DomainWarpGradient");
dwGrad.set("Source", FastNoise.create("Simplex"));
dwfp.set("Domain Warp Source", dwGrad);
dwfp.set("Octaves", 3);
var dwfpVal = dwfp.genSingle2D(5, 5, 1);
assert(typeof dwfpVal === "number", "DomainWarpFractalProgressive should generate");

var dwfi = FastNoise.create("DomainWarpFractalIndependent");
var dwGrad2 = FastNoise.create("DomainWarpGradient");
dwGrad2.set("Source", FastNoise.create("Simplex"));
dwfi.set("Domain Warp Source", dwGrad2);
dwfi.set("Octaves", 3);
var dwfiVal = dwfi.genSingle2D(5, 5, 1);
assert(typeof dwfiVal === "number", "DomainWarpFractalIndependent should generate");

// ==========================================================================
// Full pipeline: programmatic terrain via set()
// ==========================================================================

var terrainSrc = FastNoise.create("Simplex");
var terrainFbm = FastNoise.create("FractalFBm");
terrainFbm.set("Source", terrainSrc);
terrainFbm.set("Octaves", 5);
terrainFbm.set("Gain", 0.5);
terrainFbm.set("Lacunarity", 2.0);
var heightmap = terrainFbm.genUniformGrid2D(0, 0, 32, 32, 0.02, 1337);
assert(heightmap instanceof Float32Array, "Pipeline should produce Float32Array");
assertEqual(heightmap.length, 1024);

// Determinism
var heightmap2 = terrainFbm.genUniformGrid2D(0, 0, 32, 32, 0.02, 1337);
for (var i = 0; i < heightmap.length; i++) {
    assertEqual(heightmap[i], heightmap2[i]);
}

// 3D voxel pipeline
var voxSrc = FastNoise.create("Perlin");
var voxFrac = FastNoise.create("FractalRidged");
voxFrac.set("Source", voxSrc);
voxFrac.set("Octaves", 4);
var voxels = voxFrac.genUniformGrid3D(0, 0, 0, 16, 16, 16, 0.05, 42);
assert(voxels instanceof Float32Array, "3D pipeline should produce Float32Array");
assertEqual(voxels.length, 4096);

// Complex terrain: FBm + ridged blend
var base = FastNoise.create("FractalFBm");
base.set("Source", FastNoise.create("Simplex"));
base.set("Octaves", 4);
var ridges = FastNoise.create("FractalRidged");
ridges.set("Source", FastNoise.create("Simplex"));
ridges.set("Octaves", 3);
var blend = FastNoise.create("Fade");
blend.set("A", base);
blend.set("B", ridges);
blend.set("Fade", 0.4);
var blended = blend.genUniformGrid2D(0, 0, 32, 32, 0.01, 1337);
assert(blended instanceof Float32Array, "Blended terrain should produce Float32Array");
assertEqual(blended.length, 1024);

console.log("All FastNoise tests passed");
