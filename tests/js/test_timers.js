// Test: timers API
assert(typeof setTimeout === 'function', 'setTimeout exists');
assert(typeof setInterval === 'function', 'setInterval exists');
assert(typeof clearTimeout === 'function', 'clearTimeout exists');
assert(typeof clearInterval === 'function', 'clearInterval exists');
assert(typeof queueMicrotask === 'function', 'queueMicrotask exists');
assert(typeof performance === 'object', 'performance exists');
assert(typeof performance.now === 'function', 'performance.now exists');

// setTimeout returns an ID
var id1 = setTimeout(function() {}, 100);
assert(typeof id1 === 'number', 'setTimeout returns a number');
assert(id1 > 0, 'setTimeout returns a positive ID');

// setInterval returns an ID
var id2 = setInterval(function() {}, 100);
assert(typeof id2 === 'number', 'setInterval returns a number');
clearInterval(id2);

// clearTimeout doesn't throw
clearTimeout(id1);
clearTimeout(999); // non-existent

// performance.now returns a number
var t = performance.now();
assert(typeof t === 'number', 'performance.now returns a number');
assert(t >= 0, 'performance.now is non-negative');

// Timer tick mechanism exists
assert(typeof __brokit_tick_timers === 'function', '__brokit_tick_timers exists');

// Test that setTimeout fires when ticked
var fired = false;
setTimeout(function() { fired = true; }, 0);
__brokit_tick_timers(Date.now() + 1);
assert(fired === true, 'setTimeout with delay 0 fires on tick');

// Test setInterval fires repeatedly
var count = 0;
var intId = setInterval(function() { count++; }, 10);
var now = Date.now();
__brokit_tick_timers(now + 15);
__brokit_tick_timers(now + 25);
__brokit_tick_timers(now + 35);
clearInterval(intId);
assert(count >= 2, 'setInterval fires multiple times: count=' + count);

// Test clearTimeout prevents firing
var didFire = false;
var toCancel = setTimeout(function() { didFire = true; }, 50);
clearTimeout(toCancel);
__brokit_tick_timers(Date.now() + 100);
assert(didFire === false, 'clearTimeout prevents firing');
