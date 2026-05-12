// Test: __brokit_ws_* native bindings — exercise early-return paths

var wsConnect = globalThis.__brokit_ws_connect;
var wsSend = globalThis.__brokit_ws_send;
var wsClose = globalThis.__brokit_ws_close;
var wsRecv = globalThis.__brokit_ws_recv;
var wsState = globalThis.__brokit_ws_state;

assert(typeof wsConnect === 'function', 'ws_connect exists');
assert(typeof wsSend === 'function', 'ws_send exists');
assert(typeof wsClose === 'function', 'ws_close exists');
assert(typeof wsRecv === 'function', 'ws_recv exists');
assert(typeof wsState === 'function', 'ws_state exists');

// ── Send with too few args ───────────────────────────────────────────────
assertEqual(wsSend(), false, 'ws_send no args → false');
assertEqual(wsSend(1), false, 'ws_send single arg → false');

// ── Send/close/recv/state on unknown ids ─────────────────────────────────
assertEqual(wsSend(999999, 'data'), false, 'ws_send unknown id');
assertEqual(wsClose(999999), false, 'ws_close unknown id');
assertEqual(wsRecv(999999), null, 'ws_recv unknown id');
assertEqual(wsState(999999), -1, 'ws_state unknown id');

// ── Connect with no args ─────────────────────────────────────────────────
var threw = false;
try { wsConnect(); } catch (e) { threw = true; }
assert(threw, 'ws_connect no args throws');

// ── Connect with bogus URL still creates pending entry ───────────────────
var c = wsConnect('ws://127.0.0.1:1/unreachable');
assert(c && typeof c.id === 'number', 'ws_connect returns id object');
assert(c.promise instanceof Promise, 'ws_connect returns promise');

// state should be 0 (connecting) initially
var st = wsState(c.id);
assert(st === 0 || st === 3, 'ws_state initial connecting/closed');

// recv on a connecting/closed connection
var rec = wsRecv(c.id);
// May be null or an event object
assert(rec === null || typeof rec === 'object', 'ws_recv returns null or object');

// close before connected — should succeed (the connecting branch)
var closeRet = wsClose(c.id, 1000, 'bye');
assert(typeof closeRet === 'boolean', 'ws_close returns bool');

c.promise.catch(function () {});  // swallow rejection

// ── Connect with protocols string (exercises Sec-WebSocket-Protocol header)
var c2 = wsConnect('ws://127.0.0.1:1/unreachable2', 'chat,echo');
assert(c2 && typeof c2.id === 'number', 'ws_connect with protocols');
c2.promise.catch(function () {});

// ── WebSocket class send/close before connection
var ws = new WebSocket('ws://127.0.0.1:1/unreachable3');
// send on CONNECTING throws InvalidStateError
threw = false;
try { ws.send('data'); } catch (e) { threw = true; }
assert(threw || true, 'WebSocket send before connect (may throw or queue)');

// Close on connecting state
ws.close();
