// Test: EventSource (SSE)

// ── API existence ────────────────────────────────────────────────────────
assert(typeof EventSource === 'function', 'EventSource exists');
assertEqual(EventSource.CONNECTING, 0, 'CONNECTING constant');
assertEqual(EventSource.OPEN, 1, 'OPEN constant');
assertEqual(EventSource.CLOSED, 2, 'CLOSED constant');

// ── Constructor validation ───────────────────────────────────────────────
var threw = false;
try { EventSource('http://example.com'); } catch (e) { threw = true; }
assert(threw, 'throws without new');

threw = false;
try { new EventSource(); } catch (e) { threw = true; }
assert(threw, 'throws without URL');

// ── Prototype methods exist ──────────────────────────────────────────────
assert(typeof EventSource.prototype.addEventListener === 'function', 'addEventListener exists');
assert(typeof EventSource.prototype.removeEventListener === 'function', 'removeEventListener exists');
assert(typeof EventSource.prototype.dispatchEvent === 'function', 'dispatchEvent exists');
assert(typeof EventSource.prototype.close === 'function', 'close exists');

// ── SSE parsing via _processEvent (white-box test) ───────────────────────
// Create an EventSource and immediately close it to prevent any network activity,
// then test the parsing logic directly.
var es = new EventSource('http://127.0.0.1:1/nope');
es.close(); // immediately close — prevents reconnection and stops fetch

var received = [];
es.onmessage = function(e) { received.push(e); };

// Simple data event
es._processEvent('data: hello world');
assertEqual(received.length, 1, 'received one event');
assertEqual(received[0].data, 'hello world', 'data parsed');
assertEqual(received[0].type, 'message', 'default type is message');
assertEqual(received[0].lastEventId, '', 'no id');

// Multi-line data
received = [];
es._processEvent('data: line1\ndata: line2\ndata: line3');
assertEqual(received.length, 1, 'multi-line: one event');
assertEqual(received[0].data, 'line1\nline2\nline3', 'multi-line data joined with newlines');

// Named event
received = [];
var namedReceived = [];
es.addEventListener('custom', function(e) { namedReceived.push(e); });
es._processEvent('event: custom\ndata: payload');
assertEqual(received.length, 0, 'named event not sent to onmessage');
assertEqual(namedReceived.length, 1, 'named event dispatched');
assertEqual(namedReceived[0].type, 'custom', 'event type');
assertEqual(namedReceived[0].data, 'payload', 'event data');

// Event with id
received = [];
es._processEvent('id: 42\ndata: with-id');
assertEqual(received.length, 1, 'event with id received');
assertEqual(received[0].lastEventId, '42', 'lastEventId set');
assertEqual(es._lastEventId, '42', 'internal lastEventId updated');

// Id persists across events
received = [];
es._processEvent('data: no-id-field');
assertEqual(received[0].lastEventId, '42', 'lastEventId persists');

// Retry field
es._processEvent('retry: 5000\ndata: retry-test');
assertEqual(es._retryMs, 5000, 'retry updated');

// Invalid retry (non-numeric) — should not change
es._processEvent('retry: abc\ndata: retry-ignore');
assertEqual(es._retryMs, 5000, 'invalid retry ignored');

// Comment lines (starting with :)
received = [];
es._processEvent(': this is a comment\ndata: after-comment');
assertEqual(received.length, 1, 'comment line ignored');
assertEqual(received[0].data, 'after-comment', 'data after comment');

// Empty data value (field with no value after colon)
received = [];
es._processEvent('data:');
assertEqual(received.length, 1, 'empty data dispatched');
assertEqual(received[0].data, '', 'empty data value');

// Field without colon
received = [];
es._processEvent('data');
assertEqual(received.length, 1, 'field without colon');
assertEqual(received[0].data, '', 'data is empty string for bare field');

// No data lines — should not dispatch
received = [];
es._processEvent('event: noop');
assertEqual(received.length, 0, 'no dispatch without data');

// Data with colon in value
received = [];
es._processEvent('data: key: value: more');
assertEqual(received.length, 1, 'colon in value');
assertEqual(received[0].data, 'key: value: more', 'full value with colons');

// Id with null character — should be ignored per spec
es._lastEventId = 'prev';
es._processEvent('id: bad\0id\ndata: null-id');
assertEqual(es._lastEventId, 'prev', 'id with null ignored');

// ── addEventListener / removeEventListener ───────────────────────────────
var calls = [];
var listener1 = function(e) { calls.push('l1:' + e.type); };
var listener2 = function(e) { calls.push('l2:' + e.type); };
es.addEventListener('test', listener1);
es.addEventListener('test', listener2);
es.dispatchEvent({ type: 'test' });
assertEqual(calls.length, 2, 'both listeners called');
assertEqual(calls[0], 'l1:test', 'listener1 called');
assertEqual(calls[1], 'l2:test', 'listener2 called');

es.removeEventListener('test', listener1);
calls = [];
es.dispatchEvent({ type: 'test' });
assertEqual(calls.length, 1, 'only listener2 after remove');
assertEqual(calls[0], 'l2:test', 'listener2 still called');

// ── close() state ────────────────────────────────────────────────────────
assertEqual(es.readyState, EventSource.CLOSED, 'readyState is CLOSED');

// ── Event handler setters ────────────────────────────────────────────────
var handler = function() {};
es.onopen = handler;
assertEqual(es.onopen, handler, 'onopen setter');
es.onmessage = handler;
assertEqual(es.onmessage, handler, 'onmessage setter');
es.onerror = handler;
assertEqual(es.onerror, handler, 'onerror setter');

// Non-function values get set to null
es.onopen = 'not a function';
assertEqual(es.onopen, null, 'onopen rejects non-function');

// ── withCredentials option ───────────────────────────────────────────────
var es3 = new EventSource('http://127.0.0.1:1/cred', { withCredentials: true });
assertEqual(es3.withCredentials, true, 'withCredentials option');
es3.close();

// ── Multiple events in stream buffer ─────────────────────────────────────
// Test the _readStream parsing logic indirectly by simulating what happens
// when a stream delivers text with multiple events separated by blank lines
received = [];
es.onmessage = function(e) { received.push(e); };

// Simulate what _readStream does: split on \n\n and call _processEvent
var chunk = 'data: first\n\ndata: second\n\nevent: custom\ndata: third\n\n';
var parts = chunk.split('\n\n');
parts.pop(); // remove trailing empty
for (var i = 0; i < parts.length; i++) {
    if (parts[i].length > 0) es._processEvent(parts[i]);
}
assertEqual(received.length, 2, 'two message events from chunk');
assertEqual(received[0].data, 'first', 'first event data');
assertEqual(received[1].data, 'second', 'second event data');
