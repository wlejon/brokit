// Test: Event, CustomEvent, EventTarget

// --- Event ---
assert(typeof Event === 'function', 'Event exists');

var e = new Event('click');
assertEqual(e.type, 'click', 'Event type');
assertEqual(e.bubbles, false, 'Event bubbles default');
assertEqual(e.cancelable, false, 'Event cancelable default');
assertEqual(e.defaultPrevented, false, 'Event defaultPrevented default');
assert(typeof e.timeStamp === 'number', 'Event timeStamp');

var e2 = new Event('submit', { bubbles: true, cancelable: true });
assert(e2.bubbles, 'Event bubbles option');
assert(e2.cancelable, 'Event cancelable option');
e2.preventDefault();
assert(e2.defaultPrevented, 'preventDefault works');

// stopPropagation / stopImmediatePropagation
var e3 = new Event('test');
e3.stopPropagation();
assert(e3._stopPropagation, 'stopPropagation');
var e4 = new Event('test');
e4.stopImmediatePropagation();
assert(e4._stopImmediate, 'stopImmediatePropagation');

// --- CustomEvent ---
assert(typeof CustomEvent === 'function', 'CustomEvent exists');

var ce = new CustomEvent('myevent', { detail: { foo: 42 } });
assertEqual(ce.type, 'myevent', 'CustomEvent type');
assertEqual(ce.detail.foo, 42, 'CustomEvent detail');
assert(ce instanceof Event, 'CustomEvent instanceof Event');

var ce2 = new CustomEvent('bare');
assertEqual(ce2.detail, null, 'CustomEvent default detail is null');

// --- EventTarget ---
assert(typeof EventTarget === 'function', 'EventTarget exists');

var target = new EventTarget();

// addEventListener + dispatchEvent
var called = false;
target.addEventListener('test', function(e) {
    called = true;
    assertEqual(e.type, 'test', 'event type in listener');
    assertEqual(e.target, target, 'event.target is target');
});
target.dispatchEvent(new Event('test'));
assert(called, 'listener called');

// Multiple listeners
var count = 0;
target.addEventListener('multi', function() { count++; });
target.addEventListener('multi', function() { count++; });
target.dispatchEvent(new Event('multi'));
assertEqual(count, 2, 'multiple listeners called');

// removeEventListener
var handler = function() { count += 10; };
target.addEventListener('rm', handler);
target.removeEventListener('rm', handler);
target.dispatchEvent(new Event('rm'));
assertEqual(count, 2, 'removed listener not called');

// once option
var onceCount = 0;
target.addEventListener('once', function() { onceCount++; }, { once: true });
target.dispatchEvent(new Event('once'));
target.dispatchEvent(new Event('once'));
assertEqual(onceCount, 1, 'once listener called only once');

// Deduplication
var dedup = 0;
var dedupFn = function() { dedup++; };
target.addEventListener('dedup', dedupFn);
target.addEventListener('dedup', dedupFn); // should be ignored
target.dispatchEvent(new Event('dedup'));
assertEqual(dedup, 1, 'duplicate listener ignored');

// handleEvent interface
var objListener = { handleEvent: function(e) { this.called = true; } };
target.addEventListener('obj', objListener);
target.dispatchEvent(new Event('obj'));
assert(objListener.called, 'handleEvent called');

// stopImmediatePropagation prevents subsequent listeners
var order = [];
var t2 = new EventTarget();
t2.addEventListener('stop', function(e) { order.push(1); e.stopImmediatePropagation(); });
t2.addEventListener('stop', function() { order.push(2); });
t2.dispatchEvent(new Event('stop'));
assertEqual(order.length, 1, 'stopImmediate prevents second listener');
assertEqual(order[0], 1, 'first listener ran');

// dispatchEvent returns based on defaultPrevented
var t3 = new EventTarget();
t3.addEventListener('cancel', function(e) { e.preventDefault(); });
var result = t3.dispatchEvent(new Event('cancel', { cancelable: true }));
assertEqual(result, false, 'dispatchEvent returns false when defaultPrevented');

var result2 = t3.dispatchEvent(new Event('nocancal'));
assertEqual(result2, true, 'dispatchEvent returns true when not prevented');

// No listeners for event type
var t4 = new EventTarget();
var result3 = t4.dispatchEvent(new Event('missing'));
assertEqual(result3, true, 'dispatchEvent with no listeners returns true');

// CustomEvent with EventTarget
var t5 = new EventTarget();
var detailReceived;
t5.addEventListener('custom', function(e) { detailReceived = e.detail; });
t5.dispatchEvent(new CustomEvent('custom', { detail: 'hello' }));
assertEqual(detailReceived, 'hello', 'CustomEvent detail via EventTarget');
