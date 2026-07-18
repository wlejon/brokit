// WebSocketServer: RFC 6455 handshake + framing over the net module's TCP.
//
// Two clients exercise it:
//   1. The real brokit WebSocket client (curl) — text echo, binary echo,
//      close handshake with a custom code.
//   2. A raw net socket — asserts the exact RFC 6455 example accept key,
//      zero-key masked fragmentation reassembly, and that an UNMASKED client
//      frame fails the connection with 1002 (masking is mandatory).
//
// Final assertions run in completion callbacks (see test_net_tcp.js note).

const net = require('net');

assert(typeof WebSocketServer === 'function', 'WebSocketServer global exists');
assert(typeof require('websocket-server').WebSocketServer === 'function',
       'websocket-server module exists');

let clientFlowDone = false;
let rawFlowDone = false;
let unmaskedFlowDone = false;
let serverClosed = false;
let finished = false;

let srvConnections = 0;
let srvGotText = null;
let srvGotBinary = null;
let srvCloseCodes = []; // {code, wasClean} per connection, any order
let srvGotFragmented = null;

const wss = new WebSocketServer({ port: 0 }); // default host 127.0.0.1
const port = wss.address().port;
assert(port > 0, 'wss got ephemeral port: ' + port);
assert(wss.address().address === '127.0.0.1', 'wss default bind is loopback');

wss.on('connection', (ws, request) => {
    srvConnections++;
    assert(ws.readyState === 1, 'server-side socket arrives OPEN');
    assert(typeof ws.send === 'function' && typeof ws.close === 'function',
           'server-side socket has client surface');
    assert(request && typeof request.url === 'string' && request.url[0] === '/',
           'request url present: ' + (request && request.url));
    ws.onmessage = (ev) => {
        if (typeof ev.data === 'string') {
            if (ev.data === 'Hello') { srvGotFragmented = ev.data; return; }
            srvGotText = ev.data;
            ws.send(ev.data.toUpperCase());
        } else {
            srvGotBinary = ev.data; // Uint8Array (binaryType default)
            ws.send(ev.data);       // echo binary
        }
    };
    ws.onclose = (ev) => {
        srvCloseCodes.push({ code: ev.code, wasClean: ev.wasClean });
        maybeFinish();
    };
});
wss.on('close', () => {
    serverClosed = true;
    maybeFinish();
});
// The listener's close can outrun the last per-connection TCP teardown, so
// finishing also waits for all three server-side close events (the curl
// client's 4001 lands a tick or two after wss.close()).

function maybeDone() {
    if (clientFlowDone && rawFlowDone && unmaskedFlowDone) wss.close();
}
function maybeFinish() {
    if (finished || !serverClosed || srvCloseCodes.length < 3) return;
    finished = true;
    assert(srvConnections === 3, 'server saw 3 connections: ' + srvConnections);
    assert(srvGotText === 'echo me', 'server got text: ' + srvGotText);
    assert(srvGotBinary && srvGotBinary.length === 3 && srvGotBinary[0] === 5 &&
           srvGotBinary[1] === 0 && srvGotBinary[2] === 200,
           'server got binary intact');
    assert(srvGotFragmented === 'Hello', 'fragmented message reassembled: ' + srvGotFragmented);
    // The curl client closed with 4001, the raw socket with 1000 — both must
    // arrive as CLEAN closes (full close handshake both directions).
    assert(srvCloseCodes.some((c) => c.code === 4001 && c.wasClean),
           'server saw clean close 4001: ' + JSON.stringify(srvCloseCodes));
    assert(srvCloseCodes.some((c) => c.code === 1000 && c.wasClean),
           'server saw clean close 1000: ' + JSON.stringify(srvCloseCodes));
}

// ── 1. Real WebSocket client against the server ─────────────────────────────
const ws = new WebSocket('ws://127.0.0.1:' + port + '/');
let step = 0;
let clientCloseCode = 0;
ws.onopen = () => { ws.send('echo me'); };
ws.onmessage = (ev) => {
    if (step === 0) {
        assert(ev.data === 'ECHO ME', 'client got uppercased echo: ' + ev.data);
        step = 1;
        ws.send(new Uint8Array([5, 0, 200]));
    } else if (step === 1) {
        assert(ev.data instanceof Uint8Array && ev.data.length === 3,
               'client got binary echo back');
        assert(ev.data[0] === 5 && ev.data[1] === 0 && ev.data[2] === 200,
               'binary echo bytes intact');
        step = 2;
        ws.close(4001, 'done testing');
    }
};
ws.onclose = (ev) => {
    clientCloseCode = ev.code;
    assert(step === 2, 'client closed after both echoes (step=' + step + ')');
    assert(clientCloseCode === 4001, 'client close code echoed: ' + clientCloseCode);
    clientFlowDone = true;
    maybeDone();
};

// ── 2. Raw socket: handshake accept key + zero-mask fragmentation ───────────
// RFC 6455 §1.3 example: key "dGhlIHNhbXBsZSBub25jZQ==" must yield accept
// "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=".
const raw = net.connect(port, '127.0.0.1', () => {
    raw.write('GET / HTTP/1.1\r\n' +
              'Host: 127.0.0.1:' + port + '\r\n' +
              'Upgrade: websocket\r\n' +
              'Connection: Upgrade\r\n' +
              'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n' +
              'Sec-WebSocket-Version: 13\r\n\r\n');
});
let rawBuf = [];
let rawUpgraded = false;
raw.on('data', (chunk) => {
    for (let i = 0; i < chunk.length; i++) rawBuf.push(chunk[i]);
    if (!rawUpgraded) {
        const text = String.fromCharCode.apply(null, rawBuf);
        const hdrEnd = text.indexOf('\r\n\r\n');
        if (hdrEnd < 0) return;
        assert(text.indexOf('HTTP/1.1 101') === 0, 'raw got 101');
        assert(text.indexOf('Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=') > 0,
               'RFC 6455 example accept key matches');
        rawUpgraded = true;
        rawBuf = rawBuf.slice(hdrEnd + 4);
        // Fragmented text "Hello" with an all-zero masking key (legal, and
        // XOR-transparent): "Hel" (text, FIN=0) + "lo" (cont, FIN=1).
        raw.write(new Uint8Array([0x01, 0x83, 0, 0, 0, 0, 0x48, 0x65, 0x6C]));
        raw.write(new Uint8Array([0x80, 0x82, 0, 0, 0, 0, 0x6C, 0x6F]));
        // Then a masked close (1000) to finish cleanly.
        raw.write(new Uint8Array([0x88, 0x82, 0, 0, 0, 0, 0x03, 0xE8]));
    }
});
raw.on('close', () => {
    // Server must have echoed the close frame: 0x88 0x02 0x03 0xE8.
    let sawCloseEcho = false;
    for (let i = 0; i + 3 < rawBuf.length; i++) {
        if (rawBuf[i] === 0x88 && rawBuf[i + 1] === 0x02 &&
            rawBuf[i + 2] === 0x03 && rawBuf[i + 3] === 0xE8) sawCloseEcho = true;
    }
    assert(sawCloseEcho, 'server echoed close frame with code 1000');
    rawFlowDone = true;
    maybeDone();
});

// ── 3. Raw socket: unmasked frame must fail the connection with 1002 ────────
const rawBad = net.connect(port, '127.0.0.1', () => {
    rawBad.write('GET /bad HTTP/1.1\r\n' +
                 'Host: 127.0.0.1:' + port + '\r\n' +
                 'Upgrade: websocket\r\n' +
                 'Connection: Upgrade\r\n' +
                 'Sec-WebSocket-Key: c29tZW90aGVya2V5MTIzNDU2Nzg=\r\n' +
                 'Sec-WebSocket-Version: 13\r\n\r\n');
});
let badBuf = [];
let badUpgraded = false;
rawBad.on('data', (chunk) => {
    for (let i = 0; i < chunk.length; i++) badBuf.push(chunk[i]);
    if (!badUpgraded) {
        const text = String.fromCharCode.apply(null, badBuf);
        const hdrEnd = text.indexOf('\r\n\r\n');
        if (hdrEnd < 0) return;
        badUpgraded = true;
        badBuf = badBuf.slice(hdrEnd + 4);
        // UNMASKED text frame "abc" — RFC 6455 §5.1 violation.
        rawBad.write(new Uint8Array([0x81, 0x03, 0x61, 0x62, 0x63]));
    }
});
rawBad.on('close', () => {
    // Server must have failed the connection with close code 1002.
    let saw1002 = false;
    for (let i = 0; i + 3 < badBuf.length; i++) {
        if (badBuf[i] === 0x88 && badBuf[i + 2] === 0x03 && badBuf[i + 3] === 0xEA)
            saw1002 = true;
    }
    assert(saw1002, 'unmasked frame failed connection with 1002');
    unmaskedFlowDone = true;
    maybeDone();
});

// Watchdog: only reachable if a flow hangs (open sockets keep the pump alive).
setTimeout(() => {
    if (!finished) {
        assert(false, 'ws flows did not complete (client=' + clientFlowDone +
                      ' raw=' + rawFlowDone + ' unmasked=' + unmaskedFlowDone +
                      ' serverClosed=' + serverClosed + ')');
        try { ws.close(); } catch (e) {}
        raw.destroy();
        rawBad.destroy();
        wss.close();
    }
}, 15000);
