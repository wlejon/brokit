// Test: FormData

// --- Constructor ---
assert(typeof FormData === 'function', 'FormData exists');

var fd = new FormData();
assert(fd !== null, 'FormData constructor works');

// --- append / get / has ---
fd.append('name', 'Alice');
assertEqual(fd.get('name'), 'Alice', 'get returns appended value');
assert(fd.has('name'), 'has returns true for existing key');
assert(!fd.has('missing'), 'has returns false for missing key');

// Append duplicate key
fd.append('name', 'Bob');
assertEqual(fd.get('name'), 'Alice', 'get returns first value');
assertEqual(fd.getAll('name').length, 2, 'getAll returns all values');
assertEqual(fd.getAll('name')[0], 'Alice', 'getAll[0]');
assertEqual(fd.getAll('name')[1], 'Bob', 'getAll[1]');

// --- set ---
fd.set('name', 'Charlie');
assertEqual(fd.get('name'), 'Charlie', 'set replaces value');
assertEqual(fd.getAll('name').length, 1, 'set removes duplicates');

// set on new key
fd.set('age', '30');
assertEqual(fd.get('age'), '30', 'set adds new key');

// --- delete ---
fd.delete('age');
assert(!fd.has('age'), 'delete removes key');
assertEqual(fd.get('age'), null, 'get returns null for deleted key');
assertEqual(fd.getAll('age').length, 0, 'getAll returns empty for deleted key');

// delete removes all entries with that name
var fd2 = new FormData();
fd2.append('x', '1');
fd2.append('x', '2');
fd2.append('x', '3');
fd2.delete('x');
assertEqual(fd2.getAll('x').length, 0, 'delete removes all entries with name');

// --- Value coercion ---
var fd3 = new FormData();
fd3.append('num', 42);
assertEqual(fd3.get('num'), '42', 'non-string value coerced to string');
fd3.append('bool', true);
assertEqual(fd3.get('bool'), 'true', 'boolean coerced to string');
fd3.append('undef', undefined);
assertEqual(fd3.get('undef'), 'undefined', 'undefined coerced to string');
fd3.append('nil', null);
assertEqual(fd3.get('nil'), 'null', 'null coerced to string');

// --- Blob append ---
var blob = new Blob(['hello'], { type: 'text/plain' });
var fd4 = new FormData();
fd4.append('file', blob);
var val = fd4.get('file');
assert(val instanceof File, 'Blob wrapped as File');
assertEqual(val.name, 'blob', 'default filename for Blob');
assertEqual(val.type, 'text/plain', 'File preserves Blob type');
assertEqual(val.size, 5, 'File preserves Blob size');

// Blob with custom filename
fd4.append('file2', blob, 'custom.txt');
var val2 = fd4.get('file2');
assert(val2 instanceof File, 'Blob with filename is File');
assertEqual(val2.name, 'custom.txt', 'custom filename');

// --- File append ---
var file = new File(['content'], 'test.txt', { type: 'text/plain' });
var fd5 = new FormData();
fd5.append('doc', file);
var got = fd5.get('doc');
assert(got instanceof File, 'File stays as File');
assertEqual(got.name, 'test.txt', 'File name preserved');

// File with overridden filename
fd5.append('doc2', file, 'renamed.txt');
assertEqual(fd5.get('doc2').name, 'renamed.txt', 'File filename overridden');

// --- set with Blob ---
var fd6 = new FormData();
fd6.set('pic', new Blob(['img'], { type: 'image/png' }), 'photo.png');
var pic = fd6.get('pic');
assert(pic instanceof File, 'set wraps Blob as File');
assertEqual(pic.name, 'photo.png', 'set filename');

// --- entries iterator ---
var fd7 = new FormData();
fd7.append('a', '1');
fd7.append('b', '2');
fd7.append('c', '3');

var entries = [];
var iter = fd7.entries();
var next;
while (!(next = iter.next()).done) {
    entries.push(next.value);
}
assertEqual(entries.length, 3, 'entries iterator count');
assertEqual(entries[0][0], 'a', 'entries[0] key');
assertEqual(entries[0][1], '1', 'entries[0] value');
assertEqual(entries[1][0], 'b', 'entries[1] key');
assertEqual(entries[2][0], 'c', 'entries[2] key');

// --- keys iterator ---
var keys = [];
var kiter = fd7.keys();
while (!(next = kiter.next()).done) {
    keys.push(next.value);
}
assertEqual(keys.length, 3, 'keys count');
assertEqual(keys[0], 'a', 'keys[0]');
assertEqual(keys[1], 'b', 'keys[1]');
assertEqual(keys[2], 'c', 'keys[2]');

// --- values iterator ---
var values = [];
var viter = fd7.values();
while (!(next = viter.next()).done) {
    values.push(next.value);
}
assertEqual(values.length, 3, 'values count');
assertEqual(values[0], '1', 'values[0]');
assertEqual(values[1], '2', 'values[1]');
assertEqual(values[2], '3', 'values[2]');

// --- Symbol.iterator (same as entries) ---
var iterEntries = [];
for (var pair of fd7) {
    iterEntries.push(pair);
}
assertEqual(iterEntries.length, 3, 'for-of count');
assertEqual(iterEntries[0][0], 'a', 'for-of [0] key');
assertEqual(iterEntries[0][1], '1', 'for-of [0] value');

// --- forEach ---
var forEachKeys = [];
var forEachValues = [];
fd7.forEach(function(value, key) {
    forEachKeys.push(key);
    forEachValues.push(value);
});
assertEqual(forEachKeys.length, 3, 'forEach count');
assertEqual(forEachKeys[0], 'a', 'forEach key[0]');
assertEqual(forEachValues[0], '1', 'forEach value[0]');

// --- forEach thisArg ---
var obj = { items: [] };
fd7.forEach(function(value, key) {
    this.items.push(key + '=' + value);
}, obj);
assertEqual(obj.items.length, 3, 'forEach thisArg count');
assertEqual(obj.items[0], 'a=1', 'forEach thisArg item[0]');

// --- Empty FormData ---
var empty = new FormData();
assert(!empty.has('anything'), 'empty has returns false');
assertEqual(empty.get('anything'), null, 'empty get returns null');
assertEqual(empty.getAll('anything').length, 0, 'empty getAll returns empty');
var emptyIter = empty.entries();
assert(emptyIter.next().done, 'empty entries done immediately');

// --- Name coercion ---
var fd8 = new FormData();
fd8.append(123, 'numeric key');
assertEqual(fd8.get('123'), 'numeric key', 'numeric key coerced to string');
assert(fd8.has('123'), 'has with coerced key');

// --- set preserves order ---
var fd9 = new FormData();
fd9.append('a', '1');
fd9.append('b', '2');
fd9.append('a', '3');
fd9.append('c', '4');
fd9.set('a', '5');
var order = [];
fd9.forEach(function(v, k) { order.push(k + '=' + v); });
assertEqual(order.length, 3, 'set preserves order count');
assertEqual(order[0], 'a=5', 'set replaces first occurrence');
assertEqual(order[1], 'b=2', 'set keeps other entries');
assertEqual(order[2], 'c=4', 'set removes later duplicates');
