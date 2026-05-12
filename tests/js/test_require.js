// Test: require() error paths and module aliases
assert(typeof require === 'function', 'require exists');

// require() with no args throws
var threw = false;
try { require(); } catch (e) { threw = true; }
assert(threw, 'require() with no args throws');

// require() with non-string throws
threw = false;
try { require(42); } catch (e) { threw = true; }
assert(threw, 'require(number) throws');

// require unknown module throws
threw = false;
try { require('not_a_real_module_brokit_xyz'); } catch (e) { threw = true; }
assert(threw, 'require unknown module throws');

// require child_process alias
var cp = require('child_process');
assert(typeof cp === 'object', 'require child_process works');
var cpNs = require('node:child_process');
assert(typeof cpNs === 'object', 'require node:child_process works');

// require core modules
var fs = require('fs');
assert(typeof fs === 'object', 'require fs works');
var fsNs = require('node:fs');
assert(typeof fsNs === 'object', 'require node:fs works');

var path = require('path');
assert(typeof path === 'object', 'require path works');

var os = require('os');
assert(typeof os === 'object', 'require os works');

var crypto = require('crypto');
assert(typeof crypto === 'object', 'require crypto works');

// Console.log with a value that can't be stringified ([object] fallback)
console.log(Symbol('cant-stringify'));
console.warn(Symbol('warn-sym'));
console.error(Symbol('err-sym'));
console.info(Symbol('info-sym'));
console.debug(Symbol('dbg-sym'));
assert(true, 'console.* tolerates unstringifiable values');
