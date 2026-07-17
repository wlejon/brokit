// Test: Node-compat `url` module (fileURLToPath / pathToFileURL / parse / format / resolve)

// --- module registration ---
assert(typeof require === 'function', 'require exists');
var urlMod = require('url');
assert(typeof urlMod === 'object', "require('url') returns an object");
assert(typeof urlMod.fileURLToPath === 'function', 'fileURLToPath is a function');
assert(typeof urlMod.pathToFileURL === 'function', 'pathToFileURL is a function');
assertEqual(urlMod.URL, URL, "require('url').URL === URL");
assertEqual(urlMod.URLSearchParams, URLSearchParams, "require('url').URLSearchParams === URLSearchParams");

// --- pathToFileURL / fileURLToPath round-trip ---
// fileURLToPath / pathToFileURL are platform-dependent (as in Node): a drive
// path is absolute only on Windows, a leading-slash path only on POSIX. Use an
// input that is actually absolute on the host OS so the round-trip is identity.
var fileURLToPath = urlMod.fileURLToPath;
var pathToFileURL = urlMod.pathToFileURL;
var absPath = (typeof process !== 'undefined' && process.platform === 'win32') ? 'D:/x/y.js' : '/x/y.js';

var roundTripped = fileURLToPath(pathToFileURL(absPath)).replace(/\\/g, '/');
assertEqual(roundTripped, absPath, 'fileURLToPath(pathToFileURL(...)) round-trips');

// pathToFileURL returns a URL instance with a file: protocol
var fu = pathToFileURL(absPath);
assert(fu instanceof URL, 'pathToFileURL returns a URL instance');
assertEqual(fu.protocol, 'file:', 'pathToFileURL URL has file: protocol');

// fileURLToPath accepts a URL object directly (not just a string)
var pathFromUrlObj = fileURLToPath(fu).replace(/\\/g, '/');
assertEqual(pathFromUrlObj, absPath, 'fileURLToPath accepts a URL object');

// --- new (require('url').URL) works and behaves like the global URL ---
var u = new (urlMod.URL)('https://a.com/p?q=1');
assertEqual(u.pathname, '/p', "new (require('url').URL)(...).pathname");
assertEqual(u.searchParams.get('q'), '1', "new (require('url').URL)(...).searchParams");

// --- resolve ---
assertEqual(urlMod.resolve('https://a.com/a/b', 'c'), 'https://a.com/a/c', 'resolve() relative path');

// --- legacy parse ---
var parsed = urlMod.parse('https://user@a.com:8080/path?x=1#frag');
assertEqual(parsed.protocol, 'https:', 'parse().protocol');
assertEqual(parsed.hostname, 'a.com', 'parse().hostname');
assertEqual(parsed.port, '8080', 'parse().port');
assertEqual(parsed.pathname, '/path', 'parse().pathname');
assertEqual(parsed.search, '?x=1', 'parse().search');
assertEqual(parsed.query, 'x=1', 'parse().query');
assertEqual(parsed.hash, '#frag', 'parse().hash');

// parse() on an unparseable/relative string should not throw
var parsedBad;
var threw = false;
try {
    parsedBad = urlMod.parse('not a url /really');
} catch (e) {
    threw = true;
}
assertEqual(threw, false, 'parse() does not throw on relative/invalid input');
assertEqual(parsedBad.pathname, 'not a url /really', 'parse() best-effort pathname fallback');

// --- legacy format ---
assertEqual(urlMod.format(u), u.href, 'format(URL instance) === .href');

var formatted = urlMod.format({
    protocol: 'https:',
    host: 'a.com:8080',
    pathname: '/p',
    search: '?q=1',
    hash: '#h'
});
assertEqual(formatted, 'https://a.com:8080/p?q=1#h', 'format(plain object) reconstructs href');

// --- domainToASCII / domainToUnicode (identity stubs) ---
assertEqual(urlMod.domainToASCII('example.com'), 'example.com', 'domainToASCII identity stub');
assertEqual(urlMod.domainToUnicode('example.com'), 'example.com', 'domainToUnicode identity stub');
