// Test: structuredClone

assert(typeof structuredClone === 'function', 'structuredClone exists');

// --- Primitives ---
assertEqual(structuredClone(42), 42, 'clone number');
assertEqual(structuredClone('hello'), 'hello', 'clone string');
assertEqual(structuredClone(true), true, 'clone boolean');
assertEqual(structuredClone(null), null, 'clone null');
assertEqual(structuredClone(undefined), undefined, 'clone undefined');

// --- Plain objects ---
var obj = { a: 1, b: 'two', c: true };
var cloned = structuredClone(obj);
assertEqual(cloned.a, 1, 'cloned obj.a');
assertEqual(cloned.b, 'two', 'cloned obj.b');
assertEqual(cloned.c, true, 'cloned obj.c');
obj.a = 999;
assertEqual(cloned.a, 1, 'clone is independent');

// --- Nested objects ---
var nested = { x: { y: { z: 42 } } };
var cn = structuredClone(nested);
assertEqual(cn.x.y.z, 42, 'deep nested clone');
nested.x.y.z = 0;
assertEqual(cn.x.y.z, 42, 'deep clone is independent');

// --- Arrays ---
var arr = [1, 'two', [3, 4]];
var ca = structuredClone(arr);
assertEqual(ca.length, 3, 'array length');
assertEqual(ca[0], 1, 'array[0]');
assertEqual(ca[2][0], 3, 'nested array');
arr[2][0] = 99;
assertEqual(ca[2][0], 3, 'array clone is independent');

// --- Date ---
var d = new Date(1234567890000);
var cd = structuredClone(d);
assert(cd instanceof Date, 'cloned Date is Date');
assertEqual(cd.getTime(), 1234567890000, 'Date time preserved');

// --- RegExp ---
var re = /foo(bar)/gi;
var cr = structuredClone(re);
assert(cr instanceof RegExp, 'cloned RegExp is RegExp');
assertEqual(cr.source, 'foo(bar)', 'RegExp source');
assertEqual(cr.flags, 'gi', 'RegExp flags');

// --- Map ---
var m = new Map();
m.set('a', 1);
m.set('b', { nested: true });
var cm = structuredClone(m);
assert(cm instanceof Map, 'cloned Map is Map');
assertEqual(cm.get('a'), 1, 'Map value');
assertEqual(cm.get('b').nested, true, 'Map nested value');
m.get('b').nested = false;
assertEqual(cm.get('b').nested, true, 'Map clone is independent');

// --- Set ---
var s = new Set([1, 2, 3]);
var cs = structuredClone(s);
assert(cs instanceof Set, 'cloned Set is Set');
assertEqual(cs.size, 3, 'Set size');
assert(cs.has(1), 'Set has 1');
assert(cs.has(2), 'Set has 2');

// --- ArrayBuffer ---
var ab = new ArrayBuffer(4);
var view = new Uint8Array(ab);
view[0] = 10; view[1] = 20;
var cab = structuredClone(ab);
assert(cab instanceof ArrayBuffer, 'cloned ArrayBuffer');
assertEqual(cab.byteLength, 4, 'ArrayBuffer length');
var cv = new Uint8Array(cab);
assertEqual(cv[0], 10, 'ArrayBuffer data preserved');
view[0] = 99;
assertEqual(cv[0], 10, 'ArrayBuffer clone independent');

// --- TypedArray ---
var ta = new Uint8Array([5, 10, 15]);
var cta = structuredClone(ta);
assert(cta instanceof Uint8Array, 'cloned Uint8Array');
assertEqual(cta.length, 3, 'TypedArray length');
assertEqual(cta[1], 10, 'TypedArray data');
ta[1] = 0;
assertEqual(cta[1], 10, 'TypedArray clone independent');

// --- Error ---
var err = new Error('test error');
var cerr = structuredClone(err);
assert(cerr instanceof Error, 'cloned Error');
assertEqual(cerr.message, 'test error', 'Error message');

// --- Circular reference ---
var circular = { a: 1 };
circular.self = circular;
var cc = structuredClone(circular);
assertEqual(cc.a, 1, 'circular.a');
assert(cc.self === cc, 'circular reference preserved');

// --- Throws on functions ---
var threwOnFn = false;
try { structuredClone(function(){}); } catch(e) { threwOnFn = true; }
assertEqual(threwOnFn, true, 'throws on function');

// --- Throws on symbols ---
var threwOnSym = false;
try { structuredClone(Symbol('x')); } catch(e) { threwOnSym = true; }
assertEqual(threwOnSym, true, 'throws on symbol');
