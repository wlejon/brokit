// Test: Blob/File extended methods — arrayBuffer(), text()

// ── Blob.arrayBuffer() ───────────────────────────────────────────────────
var b = new Blob(['hello, world']);
var abDone = false;
b.arrayBuffer().then(function(ab) {
    assert(ab instanceof ArrayBuffer, 'arrayBuffer returns ArrayBuffer');
    assertEqual(ab.byteLength, 12, 'arrayBuffer byteLength');
    var view = new Uint8Array(ab);
    assertEqual(view[0], 0x68, 'arrayBuffer first byte');
    assertEqual(view[11], 0x64, 'arrayBuffer last byte');
    abDone = true;
});

// ── Blob.text() ───────────────────────────────────────────────────────────
var txtDone = false;
new Blob(['unicode: éèê']).text().then(function(s) {
    assert(typeof s === 'string', 'text() returns string');
    assert(s.indexOf('unicode:') === 0, 'text content preserved');
    txtDone = true;
});

// ── Empty Blob arrayBuffer ────────────────────────────────────────────────
new Blob([]).arrayBuffer().then(function(ab) {
    assertEqual(ab.byteLength, 0, 'empty Blob arrayBuffer is 0 bytes');
});

// ── File.arrayBuffer / text — File extends Blob ───────────────────────────
var fileBytes = new Uint8Array([1, 2, 3, 4, 5]);
var f = new File([fileBytes], 'data.bin', { type: 'application/octet-stream' });
f.arrayBuffer().then(function(ab) {
    var v = new Uint8Array(ab);
    assertEqual(v.length, 5, 'File arrayBuffer length');
    assertEqual(v[2], 3, 'File arrayBuffer content');
});

var f2 = new File(['hi there'], 'note.txt', { type: 'text/plain' });
f2.text().then(function(s) {
    assertEqual(s, 'hi there', 'File text content');
});

// ── Blob from mixed parts including a nested Blob ─────────────────────────
var inner = new Blob(['inner']);
var outer = new Blob(['<', inner, '>']);
assertEqual(outer.size, 7, 'outer Blob size includes nested');
outer.text().then(function(s) {
    assertEqual(s, '<inner>', 'nested Blob flattened in text');
});

// ── arrayBuffer on a sliced Blob ──────────────────────────────────────────
var big = new Blob(['abcdefghij']);
var mid = big.slice(2, 7);
mid.arrayBuffer().then(function(ab) {
    assertEqual(ab.byteLength, 5, 'sliced arrayBuffer size');
    var v = new Uint8Array(ab);
    assertEqual(v[0], 'c'.charCodeAt(0), 'sliced first byte');
    assertEqual(v[4], 'g'.charCodeAt(0), 'sliced last byte');
});
