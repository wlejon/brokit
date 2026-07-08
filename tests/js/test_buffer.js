// Test: Buffer polyfill

assert(typeof Buffer === 'function', 'Buffer exists');

// --- instanceof Uint8Array ---
assert(Buffer.from('x') instanceof Uint8Array, 'Buffer is a Uint8Array');
assert(Buffer.isBuffer(Buffer.alloc(3)), 'isBuffer true for Buffer');
assert(!Buffer.isBuffer(new Uint8Array(3)), 'isBuffer false for Uint8Array');

// --- UTF-8 <-> base64 round-trip (é is 2 bytes) ---
var b64 = Buffer.from('héllo').toString('base64');
assertEqual(Buffer.from(b64, 'base64').toString('utf8'), 'héllo', 'utf8/base64 round-trip');

// --- byteLength counts UTF-8 bytes ---
assertEqual(Buffer.byteLength('héllo'), 6, 'byteLength utf8');

// --- hex ---
assertEqual(Buffer.from('ff00', 'hex')[0], 255, 'hex decode first byte');
assertEqual(Buffer.from('ff00', 'hex')[1], 0, 'hex decode second byte');
assertEqual(Buffer.from([255, 0]).toString('hex'), 'ff00', 'hex encode lowercase');

// --- concat ---
assertEqual(Buffer.concat([Buffer.from('a'), Buffer.from('b')]).toString(), 'ab', 'concat');

// --- default toString is utf8 ---
assertEqual(Buffer.from('héllo').toString(), 'héllo', 'default toString utf8');

// --- fixed-width readers ---
var buf = Buffer.from([0x12, 0x34]);
assertEqual(buf.readUInt16BE(0), 0x1234, 'readUInt16BE');
assertEqual(buf.readUInt16LE(0), 0x3412, 'readUInt16LE');

// --- writers round-trip ---
var w = Buffer.alloc(4);
w.writeUInt32BE(0xdeadbeef, 0);
assertEqual(w.readUInt32BE(0), 0xdeadbeef, 'writeUInt32BE/readUInt32BE');
w.writeUInt32LE(0x01020304, 0);
assertEqual(w.readUInt32LE(0), 0x01020304, 'writeUInt32LE/readUInt32LE');

// --- equals ---
assert(Buffer.from('abc').equals(Buffer.from('abc')), 'equals true');
assert(!Buffer.from('abc').equals(Buffer.from('abd')), 'equals false');

// --- slice is a Buffer view ---
var sl = Buffer.from('hello').slice(1, 3);
assert(Buffer.isBuffer(sl), 'slice returns Buffer');
assertEqual(sl.toString(), 'el', 'slice contents');

// --- module registry ---
assert(globalThis.__brokit_modules['buffer'].Buffer === Buffer, 'registered in module registry');
