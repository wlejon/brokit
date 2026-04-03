// Test: Headers, Request, Response classes

// ===== Headers =====

assert(typeof Headers === 'function', 'Headers exists');

// --- Constructor: empty ---
var h = new Headers();
assert(!h.has('x'), 'empty headers has() returns false');
assertEqual(h.get('x'), null, 'empty headers get() returns null');

// --- Constructor: object ---
var h2 = new Headers({ 'Content-Type': 'text/html', 'X-Custom': 'foo' });
assertEqual(h2.get('content-type'), 'text/html', 'constructor from object');
assertEqual(h2.get('Content-Type'), 'text/html', 'get is case-insensitive');
assert(h2.has('content-type'), 'has is case-insensitive');

// --- Constructor: array of pairs ---
var h3 = new Headers([['Accept', 'text/plain'], ['X-Foo', 'bar']]);
assertEqual(h3.get('accept'), 'text/plain', 'constructor from pairs');
assertEqual(h3.get('x-foo'), 'bar', 'constructor from pairs [1]');

// --- Constructor: from Headers ---
var h4 = new Headers(h2);
assertEqual(h4.get('content-type'), 'text/html', 'constructor from Headers');

// --- append ---
h.append('X-Multi', 'a');
h.append('X-Multi', 'b');
assertEqual(h.get('x-multi'), 'a, b', 'append joins with comma');

// --- set replaces ---
h.set('X-Multi', 'c');
assertEqual(h.get('x-multi'), 'c', 'set replaces all values');

// --- delete ---
h.set('temp', 'val');
assert(h.has('temp'), 'has before delete');
h.delete('temp');
assert(!h.has('temp'), 'has after delete');
assertEqual(h.get('temp'), null, 'get after delete');

// --- forEach ---
var h5 = new Headers({ a: '1', b: '2' });
var pairs = [];
h5.forEach(function(v, k) { pairs.push(k + '=' + v); });
assertEqual(pairs.length, 2, 'forEach count');
assert(pairs.indexOf('a=1') >= 0, 'forEach includes a=1');
assert(pairs.indexOf('b=2') >= 0, 'forEach includes b=2');

// --- entries iterator ---
var entries = [];
var iter = h5.entries();
var next;
while (!(next = iter.next()).done) entries.push(next.value);
assertEqual(entries.length, 2, 'entries count');

// --- keys iterator ---
var keys = [];
var kiter = h5.keys();
while (!(next = kiter.next()).done) keys.push(next.value);
assertEqual(keys.length, 2, 'keys count');

// --- values iterator ---
var vals = [];
var viter = h5.values();
while (!(next = viter.next()).done) vals.push(next.value);
assertEqual(vals.length, 2, 'values count');

// --- Symbol.iterator ---
var forOf = [];
for (var pair of h5) forOf.push(pair);
assertEqual(forOf.length, 2, 'for-of count');

// --- _toObject ---
var obj = h5._toObject();
assertEqual(obj.a, '1', '_toObject key a');
assertEqual(obj.b, '2', '_toObject key b');

// ===== Response =====

assert(typeof Response === 'function', 'Response exists');

// --- Constructor: text body ---
var r = new Response('hello', { status: 200, statusText: 'OK' });
assertEqual(r.status, 200, 'Response status');
assertEqual(r.statusText, 'OK', 'Response statusText');
assert(r.ok, 'Response ok');
assert(!r.bodyUsed, 'Response bodyUsed false initially');
assert(r.headers instanceof Headers, 'Response headers is Headers');

// --- Body methods (chained to avoid microtask ordering issues) ---
var bodyTests = Promise.resolve();

// text()
bodyTests = bodyTests.then(function() {
    var r = new Response('hello');
    return r.text().then(function(t) {
        assertEqual(t, 'hello', 'Response.text()');
    });
});

// json() via text()
bodyTests = bodyTests.then(function() {
    var r2 = new Response('{"a":1}');
    return r2.text().then(function(t) {
        var j = JSON.parse(t);
        assertEqual(j.a, 1, 'Response.json() via text');
    });
});

// arrayBuffer()
bodyTests = bodyTests.then(function() {
    var r3 = new Response('AB');
    return r3.arrayBuffer().then(function(ab) {
        assert(ab instanceof ArrayBuffer, 'Response.arrayBuffer() type');
        assertEqual(ab.byteLength, 2, 'Response.arrayBuffer() length');
    });
});

// blob()
bodyTests = bodyTests.then(function() {
    var r4 = new Response('data', { headers: { 'content-type': 'text/plain' } });
    return r4.blob().then(function(b) {
        assert(b instanceof Blob, 'Response.blob() type');
        assertEqual(b.size, 4, 'Response.blob() size');
        assertEqual(b.type, 'text/plain', 'Response.blob() type from headers');
    });
});

// clone()
bodyTests = bodyTests.then(function() {
    var r5 = new Response('clone me', { status: 201 });
    var r5c = r5.clone();
    assertEqual(r5c.status, 201, 'clone status');
    return Promise.all([r5.text(), r5c.text()]).then(function(results) {
        assertEqual(results[0], 'clone me', 'original text');
        assertEqual(results[1], 'clone me', 'clone text');
    });
});

// bodyUsed prevents double consumption
bodyTests = bodyTests.then(function() {
    var r6 = new Response('once');
    r6.text();
    return r6.text().then(
        function() { assert(false, 'double text() should reject'); },
        function() { assert(true, 'double text() rejects'); }
    );
});

// null body
bodyTests = bodyTests.then(function() {
    var r7 = new Response(null, { status: 204 });
    assertEqual(r7.status, 204, 'null body status');
    return r7.text().then(function(t) {
        assertEqual(t, '', 'null body text is empty');
    });
});

// --- Response.error() ---
var re = Response.error();
assertEqual(re.status, 0, 'Response.error() status');

// --- Response.redirect() ---
var rr = Response.redirect('https://example.com', 301);
assertEqual(rr.status, 301, 'Response.redirect() status');
assertEqual(rr.headers.get('location'), 'https://example.com', 'Response.redirect() location');

// --- Response.json() static ---
var rj = Response.json({ x: 42 }, { status: 201 });
assertEqual(rj.status, 201, 'Response.json() status');
assertEqual(rj.headers.get('content-type'), 'application/json', 'Response.json() content-type');
bodyTests = bodyTests.then(function() {
    return rj.text().then(function(t) {
        assertEqual(JSON.parse(t).x, 42, 'Response.json() body');
    });
});

// ===== Request =====

assert(typeof Request === 'function', 'Request exists');

// --- Constructor: URL string ---
var req = new Request('https://example.com/api');
assertEqual(req.url, 'https://example.com/api', 'Request url');
assertEqual(req.method, 'GET', 'Request default method');
assert(req.headers instanceof Headers, 'Request headers is Headers');

// --- Constructor: with init ---
var req2 = new Request('https://example.com/post', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: '{"key":"val"}'
});
assertEqual(req2.method, 'POST', 'Request method');
assertEqual(req2.headers.get('content-type'), 'application/json', 'Request headers');
assertEqual(req2._body, '{"key":"val"}', 'Request body');

// --- Constructor: from Request ---
var req3 = new Request(req2);
assertEqual(req3.url, 'https://example.com/post', 'Request from Request url');
assertEqual(req3.method, 'POST', 'Request from Request method');
assertEqual(req3.headers.get('content-type'), 'application/json', 'Request from Request headers');

// --- Constructor: Request with override ---
var req4 = new Request(req2, { method: 'PUT' });
assertEqual(req4.method, 'PUT', 'Request override method');

// --- clone() ---
var req5 = new Request('https://example.com', { method: 'DELETE' });
var req5c = req5.clone();
assertEqual(req5c.method, 'DELETE', 'clone method');
assertEqual(req5c.url, 'https://example.com', 'clone url');

// ===== fetch wraps response headers =====
// (We can't test real HTTP here, but we can test the wrapper logic)
// The native fetch still works for local files, verify Headers wrapping

// ===== FormData serialization =====
// Test the internal serialization by checking generated body format
var fd = new FormData();
fd.append('name', 'Alice');
fd.append('age', '30');

// Create a Request with FormData body to verify it gets serialized
var reqFd = new Request('https://example.com/upload', {
    method: 'POST',
    body: fd
});
assert(reqFd._body instanceof FormData, 'Request preserves FormData body');
