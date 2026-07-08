// Test: events (Node-compat EventEmitter)

assert(typeof EventEmitter === 'function', 'EventEmitter exists');

// --- on/emit passes args in order; emit return value ---
var ee = new EventEmitter();
var received = null;
var noListenerResult = ee.emit('nope', 1, 2);
assertEqual(noListenerResult, false, 'emit with no listeners returns false');

ee.on('greet', function(a, b, c) { received = [a, b, c]; });
var withListenerResult = ee.emit('greet', 'hello', 42, true);
assertEqual(withListenerResult, true, 'emit with a listener returns true');
assertEqual(JSON.stringify(received), JSON.stringify(['hello', 42, true]), 'listener received args in order');

// --- once fires exactly once ---
var ee2 = new EventEmitter();
var onceCount = 0;
ee2.once('ping', function() { onceCount++; });
ee2.emit('ping');
ee2.emit('ping');
assertEqual(onceCount, 1, 'once listener fires exactly once');

// --- off removes a listener ---
var ee3 = new EventEmitter();
var offCount = 0;
function offHandler() { offCount++; }
ee3.on('x', offHandler);
ee3.off('x', offHandler);
ee3.emit('x');
assertEqual(offCount, 0, 'off removes the listener');

// removeListener alias behaves the same as off
var ee3b = new EventEmitter();
var removeListenerCount = 0;
function rlHandler() { removeListenerCount++; }
ee3b.on('y', rlHandler);
ee3b.removeListener('y', rlHandler);
ee3b.emit('y');
assertEqual(removeListenerCount, 0, 'removeListener removes the listener');

// removeListener before a once listener fires prevents it
var ee3c = new EventEmitter();
var onceFired = false;
function onceHandler() { onceFired = true; }
ee3c.once('z', onceHandler);
ee3c.removeListener('z', onceHandler);
ee3c.emit('z');
assertEqual(onceFired, false, 'removeListener prevents a once listener from firing');

// --- removeAllListeners clears ---
var ee4 = new EventEmitter();
ee4.on('a', function() {});
ee4.on('b', function() {});
assertEqual(ee4.eventNames().length, 2, 'two events registered before removeAllListeners');
ee4.removeAllListeners();
assertEqual(ee4.eventNames().length, 0, 'removeAllListeners clears all events');
assertEqual(ee4.emit('a'), false, 'emit after removeAllListeners returns false');

// removeAllListeners(type) clears just one event
var ee4b = new EventEmitter();
ee4b.on('a', function() {});
ee4b.on('b', function() {});
ee4b.removeAllListeners('a');
assertEqual(ee4b.listenerCount('a'), 0, 'removeAllListeners(type) clears just that event');
assertEqual(ee4b.listenerCount('b'), 1, 'removeAllListeners(type) leaves other events alone');

// --- emit('error') with no listener throws ---
var ee5 = new EventEmitter();
var threw = false;
var caught = null;
try {
    ee5.emit('error', new Error('boom'));
} catch (e) {
    threw = true;
    caught = e;
}
assertEqual(threw, true, 'emit(error) with no listener throws');
assert(caught instanceof Error, 'thrown value is an Error');
assertEqual(caught.message, 'boom', 'thrown error preserves message');

// emit('error') with a listener does NOT throw
var ee5b = new EventEmitter();
var errorHandled = false;
ee5b.on('error', function(e) { errorHandled = true; });
var didNotThrow = true;
try {
    ee5b.emit('error', new Error('handled'));
} catch (e) {
    didNotThrow = false;
}
assertEqual(didNotThrow, true, 'emit(error) with a listener does not throw');
assertEqual(errorHandled, true, 'error listener was called');

// --- on returns the emitter (chainable) ---
var ee6 = new EventEmitter();
var chainResult = ee6.on('foo', function() {});
assertEqual(chainResult === ee6, true, 'on() returns the emitter');
var chainResult2 = ee6.on('foo', function() {}).on('bar', function() {});
assertEqual(chainResult2 === ee6, true, 'on() chains');

// --- require('events') ---
assertEqual(require('events') === EventEmitter, true, "require('events') === EventEmitter");
assertEqual(require('events').EventEmitter === EventEmitter, true, "require('events').EventEmitter === EventEmitter");

// --- extra sanity: listeners()/rawListeners()/listenerCount() ---
var ee7 = new EventEmitter();
function h1() {}
ee7.on('multi', h1);
ee7.once('multi', function() {});
assertEqual(ee7.listenerCount('multi'), 2, 'listenerCount counts all listeners incl. once-wrapped');
assertEqual(ee7.listeners('multi').length, 2, 'listeners() returns unwrapped listeners');
assertEqual(ee7.rawListeners('multi').length, 2, 'rawListeners() returns raw (possibly wrapped) listeners');
assertEqual(ee7.listeners('multi')[0] === h1, true, 'listeners() unwraps to the original function for plain on()');

// --- prependListener ordering ---
var ee8 = new EventEmitter();
var order = [];
ee8.on('seq', function() { order.push('second'); });
ee8.prependListener('seq', function() { order.push('first'); });
ee8.emit('seq');
assertEqual(JSON.stringify(order), JSON.stringify(['first', 'second']), 'prependListener runs before existing listeners');

// --- setMaxListeners/getMaxListeners ---
var ee9 = new EventEmitter();
assertEqual(ee9.getMaxListeners(), 10, 'default max listeners is 10');
ee9.setMaxListeners(5);
assertEqual(ee9.getMaxListeners(), 5, 'setMaxListeners updates the instance value');
assertEqual(EventEmitter.defaultMaxListeners, 10, 'EventEmitter.defaultMaxListeners is 10');
