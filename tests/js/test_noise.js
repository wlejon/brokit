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

console.log("All FastNoise tests passed");
