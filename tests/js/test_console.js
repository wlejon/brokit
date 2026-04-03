// Test: console API
assert(typeof console === 'object', 'console exists');
assert(typeof console.log === 'function', 'console.log is a function');
assert(typeof console.warn === 'function', 'console.warn is a function');
assert(typeof console.error === 'function', 'console.error is a function');
assert(typeof console.debug === 'function', 'console.debug is a function');
assert(typeof console.info === 'function', 'console.info is a function');
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
