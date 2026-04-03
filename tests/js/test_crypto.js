// Test: crypto API
assert(typeof crypto === 'object', 'crypto exists');
assert(typeof crypto.randomUUID === 'function', 'crypto.randomUUID exists');
assert(typeof crypto.getRandomValues === 'function', 'crypto.getRandomValues exists');

// randomUUID format
var uuid = crypto.randomUUID();
assert(typeof uuid === 'string', 'randomUUID returns string');
assertEqual(uuid.length, 36, 'UUID is 36 chars');

// UUID v4 format: xxxxxxxx-xxxx-4xxx-[89ab]xxx-xxxxxxxxxxxx
var parts = uuid.split('-');
assertEqual(parts.length, 5, 'UUID has 5 parts');
assertEqual(parts[0].length, 8, 'UUID part 0 length');
assertEqual(parts[1].length, 4, 'UUID part 1 length');
assertEqual(parts[2].length, 4, 'UUID part 2 length');
assertEqual(parts[3].length, 4, 'UUID part 3 length');
assertEqual(parts[4].length, 12, 'UUID part 4 length');
assertEqual(parts[2][0], '4', 'UUID version is 4');
assert('89ab'.indexOf(parts[3][0]) !== -1, 'UUID variant is correct');

// Two UUIDs should be different
var uuid2 = crypto.randomUUID();
assert(uuid !== uuid2, 'two UUIDs are different');

// getRandomValues
var arr = new Uint8Array(16);
var result = crypto.getRandomValues(arr);
assert(result === arr, 'getRandomValues returns same array');

// Check it actually filled with something (extremely unlikely to be all zeros)
var sum = 0;
for (var i = 0; i < arr.length; i++) sum += arr[i];
assert(sum > 0, 'getRandomValues produces non-zero bytes');

// Works with Uint32Array
var arr32 = new Uint32Array(4);
crypto.getRandomValues(arr32);
var sum32 = 0;
for (var i = 0; i < arr32.length; i++) sum32 += arr32[i];
assert(sum32 > 0, 'getRandomValues works with Uint32Array');
