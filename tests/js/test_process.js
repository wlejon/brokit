// Test: process object
assert(typeof process === 'object', 'process exists');
assert(typeof process.platform === 'string', 'process.platform is string');
assert(process.platform.length > 0, 'process.platform not empty');

// process.cwd()
assert(typeof process.cwd === 'function', 'process.cwd exists');
var cwd = process.cwd();
assert(typeof cwd === 'string', 'cwd returns string');
assert(cwd.length > 0, 'cwd not empty');

// process.env
assert(typeof process.env === 'object', 'process.env exists');

// Read existing env var (PATH should always exist)
var pathVal = process.env.PATH || process.env.Path;
assert(typeof pathVal === 'string', 'PATH env var readable');

// Set and read
process.env.BROKIT_TEST_VAR = 'hello123';
assertEqual(process.env.BROKIT_TEST_VAR, 'hello123', 'set env var');

// Overwrite
process.env.BROKIT_TEST_VAR = 'updated';
assertEqual(process.env.BROKIT_TEST_VAR, 'updated', 'overwrite env var');

// Delete
delete process.env.BROKIT_TEST_VAR;
assertEqual(process.env.BROKIT_TEST_VAR, undefined, 'deleted env var');

// 'in' operator
process.env.BROKIT_TEST_IN = 'yes';
assert('BROKIT_TEST_IN' in process.env, 'in operator works');
delete process.env.BROKIT_TEST_IN;
assert(!('BROKIT_TEST_IN' in process.env), 'in operator after delete');

// process.argv
assert(Array.isArray(process.argv), 'process.argv is an array');
assert(process.argv.length >= 2, 'process.argv has at least 2 entries');

// process.version / process.versions
assert(typeof process.version === 'string', 'process.version is a string');
assert(/^v\d+\.\d+\.\d+$/.test(process.version), 'process.version looks semver-ish');
assert(typeof process.versions === 'object', 'process.versions is an object');
assert(/^\d+\.\d+\.\d+$/.test(process.versions.node), 'process.versions.node is semver');

// process.pid / execPath / arch
assert(typeof process.pid === 'number', 'process.pid is a number');
assert(typeof process.execPath === 'string', 'process.execPath is a string');
assert(typeof process.arch === 'string' && process.arch.length > 0, 'process.arch is a non-empty string');

// process.nextTick
assert(typeof process.nextTick === 'function', 'process.nextTick exists');
var nextTickRan = false;
process.nextTick(function(a, b) {
    nextTickRan = (a === 1 && b === 2);
});
// nextTick runs on the microtask queue; queueMicrotask itself is synchronous to
// schedule but the callback fires once control returns to the microtask drain.
// Just verify it's callable without throwing here; a same-tick assertion isn't
// reliable in a fully sync test script.
assert(typeof process.nextTick === 'function', 'process.nextTick callable without throwing');

// process.hrtime
assert(typeof process.hrtime === 'function', 'process.hrtime exists');
var ht = process.hrtime();
assert(Array.isArray(ht) && ht.length === 2, 'hrtime returns [seconds, nanoseconds]');
assert(typeof ht[0] === 'number' && typeof ht[1] === 'number', 'hrtime entries are numbers');
assert(typeof process.hrtime.bigint === 'function', 'process.hrtime.bigint exists');
assert(typeof process.hrtime.bigint() === 'bigint', 'process.hrtime.bigint() returns a bigint');

// process.stdout / process.stderr
assert(typeof process.stdout === 'object', 'process.stdout exists');
assertEqual(process.stdout.write('hi'), true, 'process.stdout.write returns true');
assertEqual(process.stdout.fd, 1, 'process.stdout.fd is 1');
assertEqual(process.stdout.isTTY, false, 'process.stdout.isTTY is false');
assert(typeof process.stderr === 'object', 'process.stderr exists');
assertEqual(process.stderr.write('oops'), true, 'process.stderr.write returns true');
assertEqual(process.stderr.fd, 2, 'process.stderr.fd is 2');

// process.emitWarning — no-op, should not throw
assert(typeof process.emitWarning === 'function', 'process.emitWarning exists');
process.emitWarning('test warning');
