// Test: console API
assert(typeof console === 'object', 'console exists');
assert(typeof globalThis.console.log === 'function', 'console.log is a function');
assert(typeof globalThis.console.warn === 'function', 'console.warn is a function');
assert(typeof globalThis.console.error === 'function', 'console.error is a function');
assert(typeof globalThis.console.debug === 'function', 'console.debug is a function');
assert(typeof globalThis.console.info === 'function', 'console.info is a function');
assert(typeof console.assert === 'function', 'console.assert is a function');
assert(typeof console.time === 'function', 'console.time is a function');
assert(typeof console.timeEnd === 'function', 'console.timeEnd is a function');
assert(typeof console.timeLog === 'function', 'console.timeLog is a function');

// These should not throw
console.log('test message');
console.warn('test warning');
console.error('test error');
console.debug('test debug');
console.info('test info');
console.assert(true, 'should not print');
console.assert(false, 'expected assertion failure');
console.time('test');
console.timeLog('test');
console.timeEnd('test');

assert(true, 'console methods do not throw');

// Formatting goes through util.format: objects are inspected, not "[object]",
// and printf-style substitutions apply.
console.log({ a: 1, b: [2, 3] });
assert(__test_lastLog().indexOf('a: 1') !== -1, 'console.log inspects objects: ' + __test_lastLog());
console.log('n=%d s=%s', 42, 'hi');
assertEqual(__test_lastLog(), 'n=42 s=hi', 'console.log printf substitution');
console.log('x', 1, true, null, undefined);
assertEqual(__test_lastLog(), 'x 1 true null undefined', 'console.log joins primitives');
console.assert(false, 'boom %s', 'here');
assertEqual(__test_lastLog(), 'Assertion failed: boom here', 'console.assert formats its message');

// A long line is not truncated at a fixed buffer size.
var long = new Array(5001).join('x');
console.log(long);
assertEqual(__test_lastLog().length, 5000, 'long console line is not truncated');
