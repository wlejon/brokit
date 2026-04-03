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
