// Test: AbortController / AbortSignal

// --- DOMException ---
assert(typeof DOMException === 'function', 'DOMException exists');
var ex = new DOMException('test', 'AbortError');
assertEqual(ex.message, 'test', 'DOMException message');
assertEqual(ex.name, 'AbortError', 'DOMException name');

// --- AbortController basics ---
assert(typeof AbortController === 'function', 'AbortController exists');
assert(typeof AbortSignal === 'function', 'AbortSignal exists');

var ac = new AbortController();
assert(ac.signal instanceof AbortSignal, 'signal is AbortSignal');
assertEqual(ac.signal.aborted, false, 'signal starts not aborted');
assertEqual(ac.signal.reason, undefined, 'signal starts with no reason');

// --- abort() ---
ac.abort();
assertEqual(ac.signal.aborted, true, 'signal is aborted after abort()');
assert(ac.signal.reason instanceof DOMException, 'reason is DOMException');
assertEqual(ac.signal.reason.name, 'AbortError', 'reason name is AbortError');

// Double abort is a no-op
ac.abort('other reason');
assertEqual(ac.signal.reason.name, 'AbortError', 'double abort preserves original reason');

// --- abort with custom reason ---
var ac2 = new AbortController();
ac2.abort('custom');
assertEqual(ac2.signal.reason, 'custom', 'custom reason preserved');

// --- aborted / reason are read-only accessors on the prototype ---
var abortedDesc = Object.getOwnPropertyDescriptor(AbortSignal.prototype, 'aborted');
assert(abortedDesc && typeof abortedDesc.get === 'function' && abortedDesc.set === undefined, 'aborted is a getter-only accessor');
var reasonDesc = Object.getOwnPropertyDescriptor(AbortSignal.prototype, 'reason');
assert(reasonDesc && typeof reasonDesc.get === 'function' && reasonDesc.set === undefined, 'reason is a getter-only accessor');

// --- addEventListener ---
var ac3 = new AbortController();
var called = false;
ac3.signal.addEventListener('abort', function() { called = true; });
ac3.abort();
assertEqual(called, true, 'abort listener called');

// --- the abort event is a real Event whose target is the signal ---
var acEv = new AbortController();
var seen = [];
acEv.signal.onabort = function(e) {
    seen.push('onabort');
    assert(e instanceof Event, 'onabort receives an Event');
    assertEqual(e.type, 'abort', 'event type is abort');
    assert(e.target === acEv.signal, 'event.target is the signal in onabort');
    assert(e.currentTarget === acEv.signal, 'event.currentTarget is the signal in onabort');
    assert(this === acEv.signal, 'onabort this is the signal');
};
acEv.signal.addEventListener('abort', function(e) {
    seen.push('listener');
    assert(e.target === acEv.signal, 'event.target is the signal in a listener');
    assert(this === acEv.signal, 'listener this is the signal');
});
acEv.signal.addEventListener('abort', function() { seen.push('once'); }, { once: true });
acEv.abort();
assertEqual(seen.join(','), 'onabort,listener,once', 'onabort runs first, then listeners in order');
acEv.abort();
assertEqual(seen.length, 3, 'second abort fires nothing');
acEv.signal.addEventListener('abort', function() { seen.push('late'); });
assertEqual(seen.length, 3, 'a listener added after the abort never runs');

// AbortSignal.any's composite also dispatches a real event.
var anyA = new AbortController();
var anyTarget = null;
var anySig = AbortSignal.any([anyA.signal]);
anySig.addEventListener('abort', function(e) { anyTarget = e.target; });
anyA.abort('why');
assert(anyTarget === anySig, 'any() composite event target is the composite');
assertEqual(anySig.reason, 'why', 'any() composite reason');

// --- removeEventListener ---
var ac4 = new AbortController();
var count = 0;
function handler() { count++; }
ac4.signal.addEventListener('abort', handler);
ac4.signal.removeEventListener('abort', handler);
ac4.abort();
assertEqual(count, 0, 'removed listener not called');

// --- onabort property ---
var ac5 = new AbortController();
var onabortCalled = false;
ac5.signal.onabort = function() { onabortCalled = true; };
ac5.abort();
assertEqual(onabortCalled, true, 'onabort handler called');

// --- throwIfAborted ---
var ac6 = new AbortController();
// Should not throw when not aborted
ac6.signal.throwIfAborted();

ac6.abort();
var threw = false;
try {
    ac6.signal.throwIfAborted();
} catch(e) {
    threw = true;
    assertEqual(e.name, 'AbortError', 'throwIfAborted throws AbortError');
}
assertEqual(threw, true, 'throwIfAborted actually threw');

// --- AbortSignal.abort() static ---
var sig = AbortSignal.abort();
assertEqual(sig.aborted, true, 'AbortSignal.abort() is already aborted');
assertEqual(sig.reason.name, 'AbortError', 'AbortSignal.abort() default reason');

var sig2 = AbortSignal.abort('custom');
assertEqual(sig2.reason, 'custom', 'AbortSignal.abort() custom reason');

// --- AbortSignal.any() ---
var ac7 = new AbortController();
var ac8 = new AbortController();
var combined = AbortSignal.any([ac7.signal, ac8.signal]);
assertEqual(combined.aborted, false, 'combined signal starts not aborted');

ac7.abort('first');
assertEqual(combined.aborted, true, 'combined signal aborted when one source aborts');
assertEqual(combined.reason, 'first', 'combined signal has correct reason');

// any() with already-aborted signal
var preAborted = AbortSignal.abort('pre');
var combined2 = AbortSignal.any([preAborted, ac8.signal]);
assertEqual(combined2.aborted, true, 'any() with pre-aborted signal');
assertEqual(combined2.reason, 'pre', 'any() pre-aborted reason');
