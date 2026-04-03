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
