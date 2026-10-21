// Test: Node-compatible `http` (`node:http`) module over llhttp + net.
// Covers GET/POST/HEAD, chunked streaming, keep-alive pipelining, fetch()
// integration, http.get/http.request client, upgrade handoff, and bad request.

const http = require('http');
const net = require('net');

assertEqual(require('node:http'), http, 'require(node:http) === require(http)');
assert(typeof http.createServer === 'function', 'http.createServer is function');
assert(typeof http.request === 'function', 'http.request is function');
assert(typeof http.get === 'function', 'http.get is function');
assertEqual(http.STATUS_CODES[200], 'OK', 'STATUS_CODES[200] === OK');
assert(Array.isArray(http.METHODS) && http.METHODS.indexOf('POST') >= 0, 'METHODS includes POST');

let fetchFlowDone = false;
let clientReqFlowDone = false;
let keepAliveFlowDone = false;
let upgradeFlowDone = false;
let badReqFlowDone = false;
let finished = false;

const server = http.createServer((req, res) => {
    if (req.url === '/hello' && req.method === 'GET') {
        res.setHeader('X-Custom', 'bro-http');
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ ok: true, method: req.method, host: req.headers['host'] }));
        return;
    }

    if (req.url === '/head-check' && req.method === 'HEAD') {
        res.writeHead(200, { 'Content-Type': 'text/plain' });
        res.end('body-that-must-not-be-sent');
        return;
    }

    if (req.url === '/echo' && req.method === 'POST') {
        req.setEncoding('utf8');
        let body = '';
        req.on('data', (chunk) => { body += chunk; });
        req.on('end', () => {
            res.writeHead(201, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ echoed: JSON.parse(body), len: body.length }));
        });
        return;
    }

    if (req.url === '/stream' && req.method === 'GET') {
        res.writeHead(200, { 'Content-Type': 'text/plain' });
        res.write('part1-');
        res.write('part2-');
        res.end('part3');
        return;
    }

    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end('not found');
});

server.on('upgrade', (req, socket, head) => {
    assertEqual(req.url, '/up', 'upgrade url is /up');
    assertEqual(req.headers['upgrade'], 'custom-proto', 'upgrade header parsed');
    socket.write(
        'HTTP/1.1 101 Switching Protocols\r\n' +
        'Upgrade: custom-proto\r\n' +
        'Connection: Upgrade\r\n\r\n'
    );
    if (head && head.length > 0) {
        socket.end(head);
    } else {
        socket.on('data', (c) => {
            socket.end(c);
        });
    }
});

function maybeFinish() {
    if (finished) return;
    if (!fetchFlowDone || !clientReqFlowDone || !keepAliveFlowDone || !upgradeFlowDone || !badReqFlowDone) {
        return;
    }
    finished = true;
    server.close();
}

server.listen(0, '127.0.0.1');
const port = server.address().port;
assert(port > 0, 'http server listening on ephemeral port: ' + port);
const baseUrl = 'http://127.0.0.1:' + port;

// 1. Fetch GET + POST + chunked stream
(async () => {
    const r1 = await fetch(baseUrl + '/hello');
    assertEqual(r1.status, 200, 'GET /hello status 200');
    assertEqual(r1.headers.get('x-custom'), 'bro-http', 'custom header present');
    const j1 = await r1.json();
    assertEqual(j1.ok, true, 'GET /hello JSON ok');
    assertEqual(j1.method, 'GET', 'GET /hello method');

    const r2 = await fetch(baseUrl + '/echo', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ task: 'validate', step: 1 })
    });
    assertEqual(r2.status, 201, 'POST /echo status 201');
    const j2 = await r2.json();
    assertEqual(j2.echoed.task, 'validate', 'POST /echo payload task');
    assertEqual(j2.echoed.step, 1, 'POST /echo payload step');

    const r3 = await fetch(baseUrl + '/stream');
    assertEqual(r3.status, 200, 'GET /stream status 200');
    const t3 = await r3.text();
    assertEqual(t3, 'part1-part2-part3', 'chunked stream reassembled');

    fetchFlowDone = true;
    maybeFinish();
})().catch((err) => {
    assert(false, 'fetch flow failed: ' + (err && err.message ? err.message : err));
});

// 2. http.request / http.get client + HEAD check
http.get(baseUrl + '/hello', (res) => {
    assertEqual(res.statusCode, 200, 'http.get statusCode 200');
    res.setEncoding('utf8');
    let data = '';
    res.on('data', (c) => { data += c; });
    res.on('end', () => {
        const parsed = JSON.parse(data);
        assertEqual(parsed.ok, true, 'http.get parsed body');

        const headReq = http.request({
            hostname: '127.0.0.1',
            port: port,
            path: '/head-check',
            method: 'HEAD'
        }, (headRes) => {
            assertEqual(headRes.statusCode, 200, 'HEAD statusCode 200');
            assertEqual(headRes.headers['content-length'], '26', 'HEAD preserves Content-Length');
            let headBytes = 0;
            headRes.on('data', (c) => { headBytes += c.length; });
            headRes.on('end', () => {
                assertEqual(headBytes, 0, 'HEAD response carries zero body bytes');
                clientReqFlowDone = true;
                maybeFinish();
            });
        });
        headReq.end();
    });
});

// 3. Keep-alive pipelining: two back-to-back requests over one TCP socket
const kaSock = net.connect(port, '127.0.0.1', () => {
    kaSock.write(
        'GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n' +
        'GET /not-here HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n'
    );
});
let kaBuf = '';
kaSock.on('data', (c) => {
    kaBuf += new TextDecoder().decode(c);
});
kaSock.on('close', () => {
    assert(kaBuf.indexOf('HTTP/1.1 200 OK') >= 0, 'keep-alive first response 200');
    assert(kaBuf.indexOf('HTTP/1.1 404 Not Found') >= 0, 'keep-alive second response 404');
    keepAliveFlowDone = true;
    maybeFinish();
});

// 4. Upgrade handoff with trailing head bytes
const upSock = net.connect(port, '127.0.0.1', () => {
    upSock.write(
        'GET /up HTTP/1.1\r\n' +
        'Host: 127.0.0.1\r\n' +
        'Connection: Upgrade\r\n' +
        'Upgrade: custom-proto\r\n\r\n' +
        'ping-after-upgrade'
    );
});
let upBuf = '';
upSock.on('data', (c) => {
    upBuf += new TextDecoder().decode(c);
});
upSock.on('close', () => {
    assert(upBuf.indexOf('HTTP/1.1 101 Switching Protocols') === 0, 'upgrade got 101');
    assert(upBuf.indexOf('ping-after-upgrade') > 0, 'upgrade echoed trailing bytes');
    upgradeFlowDone = true;
    maybeFinish();
});

// 5. Malformed HTTP request returns 400 Bad Request
const badSock = net.connect(port, '127.0.0.1', () => {
    badSock.write('NOT_AN_HTTP_METHOD /bad HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n');
});
let badBuf = '';
badSock.on('data', (c) => {
    badBuf += new TextDecoder().decode(c);
});
badSock.on('close', () => {
    assert(badBuf.indexOf('HTTP/1.1 400 Bad Request') === 0, 'malformed request got 400');
    badReqFlowDone = true;
    maybeFinish();
});

const GC_STRESS = !!(process.env.BRONZE_GC_STRESS && process.env.BRONZE_GC_STRESS !== '0');
setTimeout(() => {
    if (!finished) {
        assert(false, 'http flows timed out (fetch=' + fetchFlowDone +
                      ' clientReq=' + clientReqFlowDone +
                      ' ka=' + keepAliveFlowDone +
                      ' up=' + upgradeFlowDone +
                      ' bad=' + badReqFlowDone + ')');
        server.close();
    }
}, 15000 * (GC_STRESS ? 8 : 1));
