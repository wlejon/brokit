// Raw TCP: net.createServer + net.connect loopback echo, string + binary,
// graceful end, close events, safe default bind.
//
// Structure note: the harness pump loop exits once no socket is pending, so
// final assertions run inside completion callbacks (guaranteed to fire while
// sockets still pump). The late watchdog timer only fires if a flow hangs —
// sockets then stay open, which keeps the pump (and timers) alive to reach it.

const net = require('net');

assert(typeof net.createServer === 'function', 'net.createServer is fn');
assert(typeof net.connect === 'function', 'net.connect is fn');
assert(typeof net.Socket === 'function', 'net.Socket is fn');
assert(net.isIP('127.0.0.1') === 4, 'isIP v4');
assert(net.isIP('::1') === 6, 'isIP v6');
assert(net.isIP('nope') === 0, 'isIP invalid');

let serverGotConnection = false;
let serverGotData = null;
let serverSawEnd = false;
let clientConnected = false;
let clientChunks = [];
let listeningFired = false;

let echoFlowDone = false;
let refusedFlowDone = false;
let finished = false;

function maybeFinish() {
    if (finished || !echoFlowDone || !refusedFlowDone) return;
    finished = true;
    assert(listeningFired, 'listening event fired');
    assert(serverGotConnection, 'server got connection');
    assert(serverGotData && serverGotData.length === 4, 'server got 4 bytes');
    assert(serverSawEnd, 'server saw end (FIN)');
    assert(clientConnected, 'client connect fired');
}

const server = net.createServer((sock) => {
    serverGotConnection = true;
    assert(sock.remoteAddress === '127.0.0.1',
           'server sees loopback peer: ' + sock.remoteAddress);
    sock.on('data', (chunk) => {
        serverGotData = chunk;
        sock.write(chunk);  // echo bytes straight back
        sock.write('tail'); // and a string write
    });
    sock.on('end', () => { serverSawEnd = true; });
    sock.on('close', () => { server.close(); });
});
server.on('listening', () => { listeningFired = true; });
server.on('close', () => {
    echoFlowDone = true;
    maybeFinish();
});

server.listen(0); // ephemeral port, default host = 127.0.0.1 (safe default)
const addr = server.address();
assert(addr && addr.port > 0, 'server got ephemeral port: ' + (addr && addr.port));
assert(addr.address === '127.0.0.1', 'default bind is loopback: ' + addr.address);

const client = net.connect(addr.port, '127.0.0.1', () => {
    clientConnected = true;
    client.write(new Uint8Array([1, 2, 250, 255]));
});
client.on('data', (chunk) => {
    clientChunks.push(chunk);
    let total = 0;
    for (const c of clientChunks) total += c.length;
    if (total >= 8) client.end(); // 4 echoed bytes + 'tail'
});
client.on('close', (hadError) => {
    assert(hadError === false, 'client close without error');

    // All data made the round trip intact.
    let all = [];
    for (const c of clientChunks) all = all.concat(Array.from(c));
    assert(all.length === 8, 'client received 8 bytes: ' + all.length);
    assert(all[0] === 1 && all[1] === 2 && all[2] === 250 && all[3] === 255,
           'binary bytes echoed intact');
    assert(String.fromCharCode(all[4], all[5], all[6], all[7]) === 'tail',
           'string write delivered');
});

// Connection-refused path: error + close(hadError) fire, no throw.
const bad = net.connect(1, '127.0.0.1'); // port 1: nothing listens there
let refusedError = false;
bad.on('error', () => { refusedError = true; });
bad.on('close', (hadError) => {
    assert(hadError === true, 'refused connect closes with hadError');
    assert(refusedError, 'refused connect emitted error before close');
    refusedFlowDone = true;
    maybeFinish();
});

// Watchdog: only reachable if a flow hangs (open sockets keep the pump alive).
setTimeout(() => {
    if (!finished) {
        assert(false, 'tcp flows did not complete (echo=' + echoFlowDone +
                      ' refused=' + refusedFlowDone + ')');
        server.close();
        client.destroy();
        bad.destroy();
    }
}, 10000);
