// FastNoise2 noise generation API tests

// Encoded node trees from FastNoise2 demo (valid encoded strings)
var SIMPLEX_ENCODED = "E@BBZEG@BD8JFgIECArXIzwECiQIw/UoPwkuAAE@BJDQAH@BC@AIEAJBw@ABZEED0KV78YZmZmPwQDmpkZPwsAAIA/HAMAAHBCBA==";

// --- Constructor ---
var gen = new FastNoise(SIMPLEX_ENCODED);
assert(gen instanceof FastNoise, "should be FastNoise instance");

// Bad encoded string should throw
var threw = false;
try { new FastNoise("not-valid"); } catch (e) { threw = true; }
assert(threw, "invalid encoded string should throw");

// Missing arg should throw
threw = false;
try { new FastNoise(); } catch (e) { threw = true; }
assert(threw, "missing argument should throw");

// --- genSingle2D ---
var val = gen.genSingle2D(0, 0, 1337);
assert(typeof val === "number", "genSingle2D should return a number");
assert(val >= -1.5 && val <= 1.5, "genSingle2D value should be in reasonable range: " + val);

// Same inputs produce same output (deterministic)
var val2 = gen.genSingle2D(0, 0, 1337);
assertEqual(val, val2);

// Different coords produce different values
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

// Values should be in noise range
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
// Zero/negative dimensions should throw
threw = false;
try { gen.genUniformGrid2D(0, 0, 0, 16, 0.01, 1337); } catch (e) { threw = true; }
assert(threw, "zero xSize should throw");

threw = false;
try { gen.genUniformGrid3D(0, 0, 0, 4, 4, -1, 0.01, 1337); } catch (e) { threw = true; }
assert(threw, "negative dimension should throw");

// ==========================================================================
// Static factory functions
// ==========================================================================

// --- FastNoise.Simplex() ---
var simplex = FastNoise.Simplex();
assert(simplex instanceof FastNoise, "Simplex should be FastNoise instance");
var sv = simplex.genSingle2D(10.5, 20.5, 42);
assert(typeof sv === "number", "Simplex genSingle2D should return number");

// --- FastNoise.SuperSimplex() ---
var ssimplex = FastNoise.SuperSimplex();
assert(ssimplex instanceof FastNoise, "SuperSimplex should be FastNoise instance");
var ssv = ssimplex.genSingle3D(1, 2, 3, 99);
assert(typeof ssv === "number", "SuperSimplex genSingle3D should return number");

// --- FastNoise.Perlin() ---
var perlin = FastNoise.Perlin();
assert(perlin instanceof FastNoise, "Perlin should be FastNoise instance");
var pv = perlin.genSingle2D(5, 5, 1);
assert(typeof pv === "number", "Perlin genSingle2D should return number");

// --- FastNoise.Value() ---
var value = FastNoise.Value();
assert(value instanceof FastNoise, "Value should be FastNoise instance");
var vv = value.genSingle2D(0, 0, 0);
assert(typeof vv === "number", "Value genSingle2D should return number");

// --- FastNoise.CellularValue() ---
var cellVal = FastNoise.CellularValue();
assert(cellVal instanceof FastNoise, "CellularValue should be FastNoise instance");
var cv = cellVal.genSingle2D(3.3, 7.7, 10);
assert(typeof cv === "number", "CellularValue genSingle2D should return number");

// --- FastNoise.CellularDistance() ---
var cellDist = FastNoise.CellularDistance();
assert(cellDist instanceof FastNoise, "CellularDistance should be FastNoise instance");
var cd = cellDist.genSingle2D(3.3, 7.7, 10);
assert(typeof cd === "number", "CellularDistance genSingle2D should return number");

// --- FastNoise.FractalFBm() ---
var fbm = FastNoise.FractalFBm();
assert(fbm instanceof FastNoise, "FractalFBm should be FastNoise instance");

// --- FastNoise.FractalRidged() ---
var ridged = FastNoise.FractalRidged();
assert(ridged instanceof FastNoise, "FractalRidged should be FastNoise instance");

// --- FastNoise.DomainWarpGradient() ---
var warp = FastNoise.DomainWarpGradient();
assert(warp instanceof FastNoise, "DomainWarpGradient should be FastNoise instance");

// Each factory produces distinct generator types (different output)
var s1 = FastNoise.Simplex().genSingle2D(50, 50, 1);
var s2 = FastNoise.Perlin().genSingle2D(50, 50, 1);
assert(s1 !== s2, "Simplex and Perlin should produce different values at same coords");

// Factories produce independent instances
var a = FastNoise.Simplex();
var b = FastNoise.Simplex();
assert(a !== b, "Each factory call should return a new instance");

// Factory nodes work with grid generation
var fGrid = FastNoise.Simplex().genUniformGrid2D(0, 0, 8, 8, 0.05, 7);
assert(fGrid instanceof Float32Array, "Factory node grid gen should return Float32Array");
assertEqual(fGrid.length, 64);

// ==========================================================================
// Configuration methods
// ==========================================================================

// --- setSource: FractalFBm + Simplex ---
var src = FastNoise.Simplex();
var fractal = FastNoise.FractalFBm();
fractal.setSource(src);
var fv = fractal.genSingle2D(10, 10, 1337);
assert(typeof fv === "number", "FractalFBm with source should generate");

// --- setSource: FractalRidged + Perlin ---
var rsrc = FastNoise.Perlin();
var rfrac = FastNoise.FractalRidged();
rfrac.setSource(rsrc);
var rv = rfrac.genSingle2D(10, 10, 1337);
assert(typeof rv === "number", "FractalRidged with source should generate");

// --- setSource: DomainWarpGradient + Simplex ---
var wsrc = FastNoise.Simplex();
var dwarp = FastNoise.DomainWarpGradient();
dwarp.setSource(wsrc);
var wv = dwarp.genSingle2D(10, 10, 1337);
assert(typeof wv === "number", "DomainWarp with source should generate");

// setSource on a non-source node should throw
threw = false;
try { FastNoise.Simplex().setSource(FastNoise.Perlin()); } catch (e) { threw = true; }
assert(threw, "setSource on Simplex should throw");

// setSource with non-FastNoise arg should throw
threw = false;
try { fractal.setSource("not a node"); } catch (e) { threw = true; }
assert(threw, "setSource with string should throw");

// --- setOctaveCount ---
var oct = FastNoise.FractalFBm();
oct.setSource(FastNoise.Simplex());
oct.setOctaveCount(3);
var ov3 = oct.genSingle2D(5, 5, 1);
oct.setOctaveCount(8);
var ov8 = oct.genSingle2D(5, 5, 1);
assert(ov3 !== ov8, "Different octave counts should produce different values");

// setOctaveCount on non-fractal should throw
threw = false;
try { FastNoise.Simplex().setOctaveCount(3); } catch (e) { threw = true; }
assert(threw, "setOctaveCount on Simplex should throw");

// --- setGain ---
var gn = FastNoise.FractalFBm();
gn.setSource(FastNoise.Simplex());
gn.setGain(0.3);
var gv1 = gn.genSingle2D(5, 5, 1);
gn.setGain(0.9);
var gv2 = gn.genSingle2D(5, 5, 1);
assert(gv1 !== gv2, "Different gains should produce different values");

// setGain on non-fractal should throw
threw = false;
try { FastNoise.Simplex().setGain(0.5); } catch (e) { threw = true; }
assert(threw, "setGain on Simplex should throw");

// --- setLacunarity ---
var ln = FastNoise.FractalFBm();
ln.setSource(FastNoise.Simplex());
ln.setLacunarity(1.5);
var lv1 = ln.genSingle2D(5, 5, 1);
ln.setLacunarity(3.0);
var lv2 = ln.genSingle2D(5, 5, 1);
assert(lv1 !== lv2, "Different lacunarities should produce different values");

// setLacunarity on non-fractal should throw
threw = false;
try { FastNoise.Simplex().setLacunarity(2.0); } catch (e) { threw = true; }
assert(threw, "setLacunarity on Simplex should throw");

// --- setWeightedStrength ---
var ws = FastNoise.FractalFBm();
ws.setSource(FastNoise.Simplex());
ws.setWeightedStrength(0.0);
var wsv1 = ws.genSingle2D(5, 5, 1);
ws.setWeightedStrength(1.0);
var wsv2 = ws.genSingle2D(5, 5, 1);
assert(wsv1 !== wsv2, "Different weighted strengths should produce different values");

// setWeightedStrength on non-fractal should throw
threw = false;
try { FastNoise.Simplex().setWeightedStrength(0.5); } catch (e) { threw = true; }
assert(threw, "setWeightedStrength on Simplex should throw");

// --- setWarpAmplitude ---
var wa = FastNoise.DomainWarpGradient();
wa.setSource(FastNoise.Simplex());
wa.setWarpAmplitude(10.0);
var wav1 = wa.genSingle2D(5, 5, 1);
wa.setWarpAmplitude(100.0);
var wav2 = wa.genSingle2D(5, 5, 1);
assert(wav1 !== wav2, "Different warp amplitudes should produce different values");

// setWarpAmplitude on non-warp should throw
threw = false;
try { FastNoise.Simplex().setWarpAmplitude(1.0); } catch (e) { threw = true; }
assert(threw, "setWarpAmplitude on Simplex should throw");

// --- setFrequency (convenience for 1/scale) ---
threw = false;
try { FastNoise.Simplex().setFrequency(0); } catch (e) { threw = true; }
assert(threw, "setFrequency(0) should throw");

// setFrequency on non-scalable should throw
threw = false;
try { FastNoise.FractalFBm().setFrequency(0.01); } catch (e) { threw = true; }
assert(threw, "setFrequency on FractalFBm should throw");

// ==========================================================================
// Full pipeline: programmatic noise graph → grid output
// ==========================================================================

var terrain_src = FastNoise.Simplex();
var terrain = FastNoise.FractalFBm();
terrain.setSource(terrain_src);
terrain.setOctaveCount(5);
terrain.setGain(0.5);
terrain.setLacunarity(2.0);
var heightmap = terrain.genUniformGrid2D(0, 0, 32, 32, 0.02, 1337);
assert(heightmap instanceof Float32Array, "Pipeline should produce Float32Array");
assertEqual(heightmap.length, 1024);

// Verify determinism of full pipeline
var heightmap2 = terrain.genUniformGrid2D(0, 0, 32, 32, 0.02, 1337);
assertEqual(heightmap.length, heightmap2.length);
for (var i = 0; i < heightmap.length; i++) {
    assertEqual(heightmap[i], heightmap2[i]);
}

// 3D pipeline for volumetric/voxel data
var vol = FastNoise.FractalRidged();
vol.setSource(FastNoise.Perlin());
vol.setOctaveCount(4);
var voxels = vol.genUniformGrid3D(0, 0, 0, 16, 16, 16, 0.05, 42);
assert(voxels instanceof Float32Array, "3D pipeline should produce Float32Array");
assertEqual(voxels.length, 4096);

console.log("All FastNoise tests passed");
