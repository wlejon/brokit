// Test: URL.createObjectURL / revokeObjectURL
assert(typeof URL.createObjectURL === 'function', 'createObjectURL exists');
assert(typeof URL.revokeObjectURL === 'function', 'revokeObjectURL exists');

// Create a blob URL
var blob = new Blob(['hello'], { type: 'text/plain' });
var url = URL.createObjectURL(blob);
assert(typeof url === 'string', 'createObjectURL returns string');
assert(url.indexOf('blob:') === 0, 'URL starts with blob:');

// Retrieve via internal helper
var retrieved = globalThis.__brokit_getBlobByURL(url);
assert(retrieved === blob, 'blob retrieved by URL');

// Revoke
URL.revokeObjectURL(url);
var after = globalThis.__brokit_getBlobByURL(url);
assertEqual(after, null, 'blob is null after revoke');

// Multiple blobs get unique URLs
var b1 = new Blob(['a']);
var b2 = new Blob(['b']);
var u1 = URL.createObjectURL(b1);
var u2 = URL.createObjectURL(b2);
assert(u1 !== u2, 'different blobs get different URLs');

// Revoke one doesn't affect the other
URL.revokeObjectURL(u1);
assertEqual(globalThis.__brokit_getBlobByURL(u1), null, 'u1 revoked');
assert(globalThis.__brokit_getBlobByURL(u2) === b2, 'u2 still valid');
URL.revokeObjectURL(u2);

// Revoking non-existent URL is a no-op
URL.revokeObjectURL('blob:nonexistent');

// --- fetch('blob:...') serves the registry: bytes and type round-trip ---
var rtBytes = new Uint8Array([0, 127, 128, 255]);
var rtBlob = new Blob([rtBytes], { type: 'application/x-round-trip' });
var rtUrl = URL.createObjectURL(rtBlob);
fetch(rtUrl).then(function (r) {
    assertEqual(r.status, 200, 'blob: fetch status 200');
    assert(r.ok, 'blob: fetch ok');
    assertEqual(r.url, rtUrl, 'blob: fetch response url');
    assertEqual(r.headers.get('content-type'), 'application/x-round-trip', 'blob: fetch content-type is the blob type');
    assertEqual(r.headers.get('content-length'), '4', 'blob: fetch content-length');
    return r.arrayBuffer();
}).then(function (ab) {
    var v = new Uint8Array(ab);
    assertEqual(v.length, 4, 'blob: fetch byte length');
    assertEqual(v[2], 128, 'blob: fetch byte 2 intact');
    assertEqual(v[3], 255, 'blob: fetch byte 3 intact');
    return fetch(rtUrl).then(function (r) { return r.blob(); });
}).then(function (b) {
    assert(b instanceof Blob, 'blob: fetch .blob() is a Blob');
    assertEqual(b.type, 'application/x-round-trip', 'blob: fetch .blob() keeps the type');
    assertEqual(b.size, 4, 'blob: fetch .blob() size');
    URL.revokeObjectURL(rtUrl);
    return fetch(rtUrl).then(function (r) {
        assertEqual(r.status, 404, 'revoked blob: fetch status 404');
        assert(!r.ok, 'revoked blob: fetch not ok');
    });
}, function (e) {
    assert(false, 'blob: fetch failed: ' + (e && e.message));
});

// A blob with no type gets no content-type header.
var untypedUrl = URL.createObjectURL(new Blob(['plain']));
fetch(untypedUrl).then(function (r) {
    assertEqual(r.headers.get('content-type'), null, 'untyped blob: fetch has no content-type');
    return r.text();
}).then(function (t) {
    assertEqual(t, 'plain', 'untyped blob: fetch text');
    URL.revokeObjectURL(untypedUrl);
});
