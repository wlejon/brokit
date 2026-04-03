// Test: URL and URLSearchParams

// --- URLSearchParams ---
assert(typeof URLSearchParams === 'function', 'URLSearchParams exists');

var sp = new URLSearchParams('foo=bar&baz=qux');
assertEqual(sp.get('foo'), 'bar', 'get foo');
assertEqual(sp.get('baz'), 'qux', 'get baz');
assertEqual(sp.get('missing'), null, 'get missing returns null');
assert(sp.has('foo'), 'has foo');
assert(!sp.has('missing'), 'has missing is false');

sp.set('foo', 'updated');
assertEqual(sp.get('foo'), 'updated', 'set updates value');

sp.append('multi', 'a');
sp.append('multi', 'b');
var all = sp.getAll('multi');
assertEqual(all.length, 2, 'getAll returns 2');
assertEqual(all[0], 'a', 'getAll[0]');
assertEqual(all[1], 'b', 'getAll[1]');

sp.delete('multi');
assertEqual(sp.has('multi'), false, 'delete removes key');

sp.sort();
var str = sp.toString();
assert(str.indexOf('baz=qux') < str.indexOf('foo=updated'), 'sort orders alphabetically');

// forEach
var keys = [];
sp.forEach(function(value, key) { keys.push(key); });
assert(keys.length >= 2, 'forEach iterates entries');

// Iterators
var sp2 = new URLSearchParams('a=1&b=2');
var keysArr = [];
for (var k of sp2.keys()) keysArr.push(k);
assertEqual(keysArr.length, 2, 'keys iterator count');
assertEqual(keysArr[0], 'a', 'keys[0]');
assertEqual(keysArr[1], 'b', 'keys[1]');

// Leading ? is stripped
var sp3 = new URLSearchParams('?x=1');
assertEqual(sp3.get('x'), '1', 'leading ? stripped');

// --- URL ---
assert(typeof URL === 'function', 'URL exists');

var u = new URL('https://example.com:8080/path?q=1#hash');
assertEqual(u.protocol, 'https:', 'protocol');
assertEqual(u.hostname, 'example.com', 'hostname');
assertEqual(u.port, '8080', 'port');
assertEqual(u.pathname, '/path', 'pathname');
assertEqual(u.search, '?q=1', 'search');
assertEqual(u.hash, '#hash', 'hash');
assertEqual(u.host, 'example.com:8080', 'host');
assertEqual(u.origin, 'https://example.com:8080', 'origin');
assertEqual(u.searchParams.get('q'), '1', 'searchParams from URL');

// Default port stripping
var u2 = new URL('https://example.com:443/');
assertEqual(u2.port, '', 'default HTTPS port stripped');

var u3 = new URL('http://example.com:80/');
assertEqual(u3.port, '', 'default HTTP port stripped');

// Relative URL resolution
var u4 = new URL('/other', 'https://example.com/base/page');
assertEqual(u4.pathname, '/other', 'absolute path resolution');
assertEqual(u4.hostname, 'example.com', 'inherits hostname from base');

var u5 = new URL('sibling', 'https://example.com/base/page');
assertEqual(u5.pathname, '/base/sibling', 'relative path resolution');

// Path normalization
var u6 = new URL('https://example.com/a/b/../c/./d');
assertEqual(u6.pathname, '/a/c/d', 'path normalization with .. and .');

// toString / toJSON
assertEqual(u.toString(), u.href, 'toString returns href');
assertEqual(u.toJSON(), u.href, 'toJSON returns href');

// href setter
var u7 = new URL('https://old.com/path');
u7.href = 'https://new.com/other';
assertEqual(u7.hostname, 'new.com', 'href setter updates hostname');
assertEqual(u7.pathname, '/other', 'href setter updates pathname');

// Invalid URL throws
var threw = false;
try { new URL('not-a-url'); } catch(e) { threw = true; }
assert(threw, 'invalid URL throws TypeError');

// userinfo
var u8 = new URL('https://user:pass@example.com/');
assertEqual(u8.username, 'user', 'username');
assertEqual(u8.password, 'pass', 'password');
