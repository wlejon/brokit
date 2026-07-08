// Test: Node-compat smoke — the exact idioms the `pi` agent harness (and its
// deps) rely on, exercised together in one file. Guards the whole Node-compat
// surface (Buffer, events, util, url, process, registry-driven require) as a
// unit so a regression in any single module is caught here too.

// --- registry-driven require(): node: prefix + built-in aliases ------------
assert(typeof require === 'function', 'require is a function');
assertEqual(require('node:fs'), require('fs'), 'require(node:fs) === require(fs)');
assertEqual(require('node:path'), require('path'), 'require(node:path) === require(path)');
assertEqual(require('node:events'), require('events'), 'require(node:events) === require(events)');
assertEqual(require('node:util'), require('util'), 'require(node:util) === require(util)');
assertEqual(require('node:url'), require('url'), 'require(node:url) === require(url)');
assertEqual(require('node:buffer').Buffer, require('buffer').Buffer, 'require(node:buffer) === require(buffer)');

// --- Buffer: global + module, base64 round-trip (multibyte) ----------------
assert(typeof Buffer === 'function', 'Buffer is a global');
assertEqual(require('buffer').Buffer, Buffer, "require('buffer').Buffer === global Buffer");
var b64 = Buffer.from('héllo').toString('base64');
assertEqual(Buffer.from(b64, 'base64').toString('utf8'), 'héllo', 'Buffer base64 round-trip (multibyte)');
assert(Buffer.from('x') instanceof Uint8Array, 'Buffer instanceof Uint8Array');

// --- events: EventEmitter global + module, on/emit -------------------------
var EE = require('events').EventEmitter;
assert(typeof EE === 'function', "require('events').EventEmitter is a ctor");
assertEqual(require('events'), EE, "require('events') === EventEmitter (Node shape)");
var ee = new EE();
var got = null;
ee.on('ping', function (x) { got = x; });
assertEqual(ee.emit('ping', 42), true, 'emit returns true with a listener');
assertEqual(got, 42, 'listener received the emitted arg');

// --- util: promisify / format ----------------------------------------------
var util = require('util');
assert(typeof util.promisify === 'function', 'util.promisify is a function');
assert(util.promisify(function (v, cb) { cb(null, v); })(1) instanceof Promise, 'promisify returns a Promise');
assertEqual(util.format('%s-%d', 'a', 3), 'a-3', 'util.format specifiers');

// --- url: fileURLToPath <-> pathToFileURL round-trip ------------------------
var url = require('url');
var rt = url.fileURLToPath(url.pathToFileURL('D:/x/y.js')).replace(/\\/g, '/');
assertEqual(rt, 'D:/x/y.js', 'url fileURLToPath(pathToFileURL()) round-trips');
assertEqual(url.URL, URL, "require('url').URL === global URL");

// --- process: shims agents/deps probe --------------------------------------
assert(Array.isArray(process.argv), 'process.argv is an array');
assert(/^\d+\.\d+\.\d+$/.test(process.versions.node), 'process.versions.node is semver');
assert(typeof process.nextTick === 'function', 'process.nextTick is a function');
assertEqual(process.stdout.write('smoke\n'), true, 'process.stdout.write returns true');
process.nextTick(function () {}); // must not throw
