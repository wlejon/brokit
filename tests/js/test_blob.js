// Test: Blob and File

// --- Blob basics ---
assert(typeof Blob === 'function', 'Blob exists');

var b = new Blob();
assertEqual(b.size, 0, 'empty Blob size');
assertEqual(b.type, '', 'empty Blob type');

// Blob from strings
var b1 = new Blob(['hello', ' ', 'world']);
assertEqual(b1.size, 11, 'string Blob size');
assertEqual(b1.type, '', 'string Blob default type');

// Blob with type option
var b2 = new Blob(['test'], { type: 'text/plain' });
assertEqual(b2.type, 'text/plain', 'Blob type option');

// Type is normalized to lowercase
var b3 = new Blob([], { type: 'Text/HTML' });
assertEqual(b3.type, 'text/html', 'Blob type lowercased');

// --- Blob from ArrayBuffer ---
var ab = new ArrayBuffer(4);
var u8 = new Uint8Array(ab);
u8[0] = 65; u8[1] = 66; u8[2] = 67; u8[3] = 68; // ABCD
var b4 = new Blob([ab]);
assertEqual(b4.size, 4, 'ArrayBuffer Blob size');

// --- Blob from TypedArray ---
var ta = new Uint8Array([72, 105]); // Hi
var b5 = new Blob([ta]);
assertEqual(b5.size, 2, 'TypedArray Blob size');

// --- Blob from mixed parts ---
var b6 = new Blob(['AB', new Uint8Array([67, 68])]);
assertEqual(b6.size, 4, 'mixed parts Blob size');

// --- Blob from another Blob ---
var inner = new Blob(['inner']);
var outer = new Blob([inner, '-outer']);
assertEqual(outer.size, 11, 'Blob-from-Blob size');

// --- slice ---
var bs = new Blob(['Hello, World!']);
var sliced = bs.slice(0, 5);
assertEqual(sliced.size, 5, 'slice size');
assertEqual(sliced.type, '', 'slice default type');

// Slice with content type
var sliced2 = bs.slice(0, 5, 'text/plain');
assertEqual(sliced2.type, 'text/plain', 'slice content type');

// Negative indices
var sliced3 = bs.slice(-6);
assertEqual(sliced3.size, 6, 'negative start slice');

// Slice beyond bounds
var sliced4 = bs.slice(0, 100);
assertEqual(sliced4.size, 13, 'slice clamped to size');

// Empty slice
var sliced5 = bs.slice(5, 5);
assertEqual(sliced5.size, 0, 'empty slice');

// --- File basics ---
assert(typeof File === 'function', 'File exists');

var f = new File(['content'], 'test.txt');
assertEqual(f.name, 'test.txt', 'File name');
assertEqual(f.size, 7, 'File size');
assertEqual(f.type, '', 'File default type');

// File with options
var f2 = new File(['data'], 'doc.html', { type: 'text/html', lastModified: 1000 });
assertEqual(f2.type, 'text/html', 'File type option');
assertEqual(f2.lastModified, 1000, 'File lastModified');
assertEqual(f2.name, 'doc.html', 'File name with options');
assertEqual(f2.size, 4, 'File size with options');

// File from binary data
var f3 = new File([new Uint8Array([0xFF, 0xD8])], 'photo.jpg', { type: 'image/jpeg' });
assertEqual(f3.size, 2, 'binary File size');
assertEqual(f3.type, 'image/jpeg', 'binary File type');

// File requires 2 args
var threwOnFile = false;
try { new File(['data']); } catch(e) { threwOnFile = true; }
assertEqual(threwOnFile, true, 'File requires name argument');

// --- File extends Blob ---
assert(f instanceof File, 'File instanceof File');
assert(f instanceof Blob, 'File instanceof Blob');
assert(!(new Blob() instanceof File), 'Blob not instanceof File');
