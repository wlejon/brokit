// Test: util module (Node-compat)

var util = require('util');
assert(typeof util === 'object', 'require("util") returns an object');
assert(typeof util.format === 'function', 'util.format is a function');

// --- promisify ---
assert(typeof util.promisify === 'function', 'util.promisify is a function');
function cb(x, done) { done(null, x * 2); }
var p = util.promisify(cb)(21);
assert(p instanceof Promise, 'promisify returns a Promise');

async function testPromisify() {
    var result = await util.promisify(cb)(21);
    assertEqual(result, 42, 'promisify resolves to fn result');

    // Rejection path
    function cbErr(done) { done(new Error('boom')); }
    var threw = false;
    try {
        await util.promisify(cbErr)();
    } catch (e) {
        threw = true;
        assertEqual(e.message, 'boom', 'promisify rejects with the error');
    }
    assertEqual(threw, true, 'promisify rejection path threw');

    // promisify.custom honored
    assert(typeof util.promisify.custom === 'symbol', 'promisify.custom is a symbol');
    function orig() {}
    orig[util.promisify.custom] = function() { return Promise.resolve('custom!'); };
    var cval = await util.promisify(orig)();
    assertEqual(cval, 'custom!', 'promisify honors [promisify.custom]');
}
testPromisify();

// --- callbackify ---
assert(typeof util.callbackify === 'function', 'util.callbackify is a function');
async function testCallbackify() {
    var asyncFn = function(x) { return Promise.resolve(x + 1); };
    var cbified = util.callbackify(asyncFn);
    await new Promise(function(resolve) {
        cbified(4, function(err, value) {
            assertEqual(err, null, 'callbackify err is null on success');
            assertEqual(value, 5, 'callbackify yields resolved value');
            resolve();
        });
    });
}
testCallbackify();

// --- format ---
assertEqual(util.format('%s-%d', 'a', 3), 'a-3', 'format %s and %d');
assertEqual(util.format('%% %s', 'x'), '% x', 'format literal %% and %s');
assertEqual(util.format('%s', 'only'), 'only', 'format single %s');
assertEqual(util.format('a', 'b', 'c'), 'a b c', 'format appends extra args');
assertEqual(util.format('%d', 'nope'), 'NaN', 'format %d of non-number is NaN');
assertEqual(util.format('%s %s', 'one'), 'one %s', 'format leaves missing specifier literal');
assertEqual(util.format('%j', { a: 1 }), '{"a":1}', 'format %j stringifies');

// --- inspect ---
assert(typeof util.inspect === 'function', 'util.inspect is a function');
var ins = util.inspect({ a: 1, b: [2, 3] });
assert(ins.indexOf('a') !== -1, 'inspect contains key a');
assert(ins.indexOf('2') !== -1, 'inspect contains nested value 2');
assertEqual(util.inspect('hi'), "'hi'", 'inspect quotes strings');
assertEqual(util.inspect(null), 'null', 'inspect null');
assertEqual(util.inspect(undefined), 'undefined', 'inspect undefined');
assert(util.inspect(function foo() {}).indexOf('Function: foo') !== -1, 'inspect names functions');

// circular
var circ = { name: 'x' };
circ.self = circ;
var circStr;
var circThrew = false;
try { circStr = util.inspect(circ); } catch (e) { circThrew = true; }
assertEqual(circThrew, false, 'inspect of circular does not throw');
assert(circStr.indexOf('Circular') !== -1, 'inspect marks circular refs');

assert(typeof util.inspect.custom === 'symbol', 'inspect.custom is a symbol');

// --- types ---
assertEqual(util.types.isDate(new Date()), true, 'types.isDate true');
assertEqual(util.types.isDate({}), false, 'types.isDate false');
assertEqual(util.types.isRegExp(/x/), true, 'types.isRegExp true');
assertEqual(util.types.isTypedArray(new Uint8Array(1)), true, 'types.isTypedArray true');
assertEqual(util.types.isTypedArray([]), false, 'types.isTypedArray false for array');
assertEqual(util.types.isArrayBuffer(new ArrayBuffer(4)), true, 'types.isArrayBuffer true');
assertEqual(util.types.isMap(new Map()), true, 'types.isMap true');
assertEqual(util.types.isSet(new Set()), true, 'types.isSet true');
assertEqual(util.types.isNativeError(new Error('x')), true, 'types.isNativeError true');
assertEqual(util.types.isPromise(Promise.resolve()), true, 'types.isPromise true');

// --- isDeepStrictEqual ---
assertEqual(util.isDeepStrictEqual({ a: [1, 2] }, { a: [1, 2] }), true, 'deepEqual nested true');
assertEqual(util.isDeepStrictEqual({ a: 1 }, { a: 2 }), false, 'deepEqual differing values false');
assertEqual(util.isDeepStrictEqual({ a: 1 }, { a: 1, b: 2 }), false, 'deepEqual differing key sets false');
assertEqual(util.isDeepStrictEqual(NaN, NaN), true, 'deepEqual NaN===NaN');
assertEqual(util.isDeepStrictEqual(new Date(1), new Date(1)), true, 'deepEqual Date by time');
assertEqual(util.isDeepStrictEqual(new Date(1), new Date(2)), false, 'deepEqual Date different time false');

// --- inherits ---
assert(typeof util.inherits === 'function', 'util.inherits is a function');
function Base() {}
Base.prototype.hello = function() { return 'hi'; };
function Derived() {}
util.inherits(Derived, Base);
assertEqual(Derived.super_, Base, 'inherits sets super_');
assertEqual(new Derived().hello(), 'hi', 'inherits wires prototype chain');

// --- deprecate ---
assert(typeof util.deprecate === 'function', 'util.deprecate is a function');
var dep = util.deprecate(function(x) { return x + 1; }, 'old');
assertEqual(dep(1), 2, 'deprecate delegates to fn');

// --- TextEncoder / TextDecoder re-export ---
assertEqual(util.TextEncoder, globalThis.TextEncoder, 'util.TextEncoder re-exported');
assertEqual(util.TextDecoder, globalThis.TextDecoder, 'util.TextDecoder re-exported');
