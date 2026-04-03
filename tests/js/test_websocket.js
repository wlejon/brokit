// Test: WebSocket

// ── API existence ────────────────────────────────────────────────────────
assert(typeof WebSocket === 'function', 'WebSocket exists');
assertEqual(WebSocket.CONNECTING, 0, 'CONNECTING constant');
assertEqual(WebSocket.OPEN, 1, 'OPEN constant');
assertEqual(WebSocket.CLOSING, 2, 'CLOSING constant');
assertEqual(WebSocket.CLOSED, 3, 'CLOSED constant');

// ── Constructor validation ───────────────────────────────────────────────
var threw = false;
try { WebSocket('ws://example.com'); } catch (e) { threw = true; }
assert(threw, 'throws without new');

threw = false;
try { new WebSocket(); } catch (e) { threw = true; }
assert(threw, 'throws without URL');

threw = false;
try { new WebSocket('http://example.com'); } catch (e) { threw = true; }
assert(threw, 'throws with http URL');

threw = false;
try { new WebSocket('ftp://example.com'); } catch (e) { threw = true; }
assert(threw, 'throws with ftp URL');

// ── Prototype methods exist ──────────────────────────────────────────────
assert(typeof WebSocket.prototype.send === 'function', 'send exists');
assert(typeof WebSocket.prototype.close === 'function', 'close exists');
assert(typeof WebSocket.prototype.addEventListener === 'function', 'addEventListener exists');
assert(typeof WebSocket.prototype.removeEventListener === 'function', 'removeEventListener exists');

// ── Instance properties ──────────────────────────────────────────────────
var ws = new WebSocket('ws://127.0.0.1:1/nope');
assertEqual(ws.url, 'ws://127.0.0.1:1/nope', 'url property');
assertEqual(ws.readyState, WebSocket.CONNECTING, 'initial readyState');
assertEqual(ws.bufferedAmount, 0, 'bufferedAmount');
assertEqual(ws.extensions, '', 'extensions');
assertEqual(ws.protocol, '', 'protocol');
assertEqual(ws.binaryType, 'blob', 'binaryType default');
assertEqual(ws.onopen, null, 'onopen default');
assertEqual(ws.onclose, null, 'onclose default');
assertEqual(ws.onmessage, null, 'onmessage default');
assertEqual(ws.onerror, null, 'onerror default');

// ── Event handler setters ────────────────────────────────────────────────
var handler = function() {};
ws.onopen = handler;
assertEqual(ws.onopen, handler, 'onopen setter');
ws.onclose = handler;
assertEqual(ws.onclose, handler, 'onclose setter');
ws.onmessage = handler;
assertEqual(ws.onmessage, handler, 'onmessage setter');
ws.onerror = handler;
assertEqual(ws.onerror, handler, 'onerror setter');

ws.onopen = 'not a function';
assertEqual(ws.onopen, null, 'onopen rejects non-function');

// ── binaryType ───────────────────────────────────────────────────────────
ws.binaryType = 'arraybuffer';
assertEqual(ws.binaryType, 'arraybuffer', 'binaryType setter');

// ── send before open throws ──────────────────────────────────────────────
threw = false;
try { ws.send('hello'); } catch (e) { threw = true; }
assert(threw, 'send while CONNECTING throws');

// Close the test socket to prevent it hanging in curl_multi
ws.close();

// ── close() validation ──────────────────────────────────────────────────
threw = false;
var _ws1 = new WebSocket('ws://127.0.0.1:1/x');
try { _ws1.close(999); } catch (e) { threw = true; }
assert(threw, 'close with invalid code throws');
_ws1.close(); // clean up

threw = false;
var _ws2 = new WebSocket('ws://127.0.0.1:1/x');
try { _ws2.close(3000); } catch (e) { threw = true; }
assert(!threw, 'close with code 3000 is valid');

threw = false;
var _ws3 = new WebSocket('ws://127.0.0.1:1/x');
try { _ws3.close(1000); } catch (e) { threw = true; }
assert(!threw, 'close with code 1000 is valid');

// ── addEventListener / removeEventListener ───────────────────────────────
var ws2 = new WebSocket('ws://127.0.0.1:1/events');
var calls = [];
var l1 = function(e) { calls.push('l1'); };
var l2 = function(e) { calls.push('l2'); };
ws2.addEventListener('test', l1);
ws2.addEventListener('test', l2);
ws2._dispatch({ type: 'test' });
assertEqual(calls.length, 2, 'both listeners called');

ws2.removeEventListener('test', l1);
calls = [];
ws2._dispatch({ type: 'test' });
assertEqual(calls.length, 1, 'only l2 after remove');
ws2.close();

// ── Close during CONNECTING cleans up ────────────────────────────────────
var ws3 = new WebSocket('ws://127.0.0.1:1/will-fail');
assertEqual(ws3.readyState, WebSocket.CONNECTING, 'ws3 is connecting');
ws3.close();
assertEqual(ws3.readyState, WebSocket.CLOSED, 'ws3 closed immediately');

// Double close is harmless
ws3.close();
assertEqual(ws3.readyState, WebSocket.CLOSED, 'ws3 still closed');

// ── Echo server integration test ─────────────────────────────────────────
// Commented out for CI — uncomment to test with a real WebSocket echo server.
// To run: uncomment below, build, and run with network access.
/*
var echoWs = new WebSocket('wss://echo.websocket.events');
echoWs.onopen = function() {
    assert(true, 'echo: connected');
    echoWs.send('hello brokit');
};
echoWs.onmessage = function(e) {
    if (e.data === 'hello brokit') assert(true, 'echo: received echo');
    echoWs.close(1000, 'done');
};
echoWs.onerror = function(e) {
    assert(true, 'echo: skipped (network)');
    echoWs.close();
};
echoWs.onclose = function(e) { assert(true, 'echo: closed'); };
*/
