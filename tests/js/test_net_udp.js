// Raw UDP: dgram round-trip over loopback, string + binary payloads, rinfo,
// setBroadcast opt-in, close events, udp6 loopback.
//
// Final assertions run inside completion callbacks (see test_net_tcp.js note);
// the watchdog timer only fires if a flow hangs.

const dgram = require('dgram');

assert(typeof dgram.createSocket === 'function', 'dgram.createSocket is fn');

let v4Done = false;
let v6Done = false;
let finished = false;

// ── udp4 round trip ─────────────────────────────────────────────────────────
let bRinfo = null;
let listeningFired = false;
let aClosed = false, bClosed = false;

const a = dgram.createSocket('udp4');
const b = dgram.createSocket('udp4');

a.on('close', () => { aClosed = true; });
b.on('close', () => {
    bClosed = true;
    v4Done = true;
    maybeFinish();
});

b.on('message', (msg, rinfo) => {
    bRinfo = rinfo;
    assert(msg.length === 5, 'b got 5 bytes');
    let text = '';
    for (let i = 0; i < msg.length; i++) text += String.fromCharCode(msg[i]);
    assert(text === 'ping!', 'string payload intact: ' + text);
    assert(rinfo.address === '127.0.0.1', 'rinfo address: ' + rinfo.address);
    assert(rinfo.family === 'IPv4', 'rinfo family IPv4');
    assert(rinfo.size === 5, 'rinfo size 5');
    // Reply with binary to complete the round trip.
    b.send(new Uint8Array([9, 8, 7]), rinfo.port, rinfo.address);
});
a.on('message', (msg, rinfo) => {
    assert(msg.length === 3 && msg[0] === 9 && msg[1] === 8 && msg[2] === 7,
           'binary reply intact');
    assert(rinfo.port === b.address().port, 'reply rinfo port');
    assert(bRinfo && bRinfo.port === a.address().port, 'rinfo port was sender port');
    a.close();
    b.close();
});

a.bind(0); // ephemeral, default host 127.0.0.1 (safe default)
b.bind(0, undefined, () => { listeningFired = true; });

assert(a.address().port > 0 && b.address().port > 0, 'both sockets got ephemeral ports');
assert(a.address().address === '127.0.0.1', 'udp default bind is loopback');

// setBroadcast is opt-in and must not throw.
a.setBroadcast(true);
a.setBroadcast(false);

a.send('ping!', b.address().port); // host omitted → 127.0.0.1

// ── udp6, same code path over ::1 ───────────────────────────────────────────
const s6a = dgram.createSocket('udp6');
const s6b = dgram.createSocket('udp6');
s6b.on('message', (msg, rinfo) => {
    assert(msg.length === 3, 'udp6 got 3 bytes');
    assert(rinfo.family === 'IPv6', 'udp6 rinfo family: ' + rinfo.family);
    s6a.close();
    s6b.close(() => {
        v6Done = true;
        maybeFinish();
    });
});
s6a.bind(0); // default ::1
s6b.bind(0);
assert(s6b.address().address === '::1', 'udp6 default bind is ::1: ' + s6b.address().address);
s6a.send('six', s6b.address().port); // host omitted → ::1

function maybeFinish() {
    if (finished || !v4Done || !v6Done) return;
    finished = true;
    assert(listeningFired, 'listening callback fired');
    assert(aClosed && bClosed, 'both udp4 sockets closed');
}

// Watchdog: only reachable if a flow hangs (open sockets keep the pump alive).
setTimeout(() => {
    if (!finished) {
        assert(false, 'udp flows did not complete (v4=' + v4Done + ' v6=' + v6Done + ')');
        a.close(); b.close(); s6a.close(); s6b.close();
    }
}, 10000);
