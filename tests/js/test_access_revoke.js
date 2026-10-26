// Resources held open under the host's access gate (api.h, "Access") close
// when the host takes the grant back: a listening server, its accepted
// connections, a connected socket, a streaming fetch, a running child process
// and an fs.watch watcher each end with an error named PermissionRevoked. What
// the new grants still allow stays open. __test_accessChanged() is the host's
// grant change; it returns how many held resources it closed.

var fs = require('fs');
var net = require('net');
var http = require('http');
var cp = require('child_process');

var isWin = process.platform === 'win32';
var root = 'brokit_access_revoke_' + process.pid;
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(root + '/watched', { recursive: true });
var absWatched = fs.realpathSync(root + '/watched');

// Everything allowed, but a gate installed: what opens now is held.
__test_setAccessPolicy({});
var heldBefore = __test_heldAccessCount();

var R = { serverErr: null, acceptedErr: null, clientErr: null, keptServer: null, fetchErr: null,
          childErr: null, childExit: null, watchErr: null, revoked: -1 };

// A server whose grant is kept, streaming a body that never ends.
var keep = http.createServer(function(req, res) {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.write('first chunk');
});
keep.listen(0, '127.0.0.1');
var keepPort = keep.address().port;
keep.on('error', function(e) { R.keptServer = e; });

// A raw server whose grant goes, with a connection to it from here.
var lost = net.createServer(function(sock) {
    sock.on('error', function(e) { R.acceptedErr = e; });
});
lost.listen(0, '127.0.0.1');
var lostPort = lost.address().port;
lost.on('error', function(e) { R.serverErr = e; });
var client = net.connect(lostPort, '127.0.0.1');
client.on('error', function(e) { R.clientErr = e; });

var child = isWin ? cp.spawn('cmd', ['/c', 'ping -n 30 127.0.0.1 >nul']) : cp.spawn('sleep', ['30']);
child.on('error', function(e) { R.childErr = e; });
child.on('exit', function(code) { R.childExit = code; });

var watcher = fs.watch(absWatched);
watcher.on('error', function(e) { R.watchErr = e; });

var bodyRead = null;
var finished = false;

fetch('http://127.0.0.1:' + keepPort + '/stream').then(function(resp) {
    var reader = resp.body.getReader();
    return reader.read().then(function(first) {
        bodyRead = first.value ? first.value.length : 0;
        // Connected and streaming: now the grants change.
        R.heldOpen = __test_heldAccessCount();
        __test_setAccessPolicy({
            net: ['/stream', 'tcp://', 'tcp-listen://127.0.0.1:' + lostPort],
            spawn: true,
            fsDenyUnder: [absWatched],
            revoked: true
        });
        R.revoked = __test_accessChanged();
        return reader.read();
    });
}).then(function() { R.fetchErr = 'read on'; }, function(e) { R.fetchErr = e; })
.then(function() {
    return new Promise(function(resolve) { setTimeout(resolve, 600); });
}).then(function() {
    R.heldAfter = __test_heldAccessCount();
    __test_clearAccessPolicy();
    try { child.kill(); } catch (e) {}
    try { watcher.close(); } catch (e) {}
    keep.close();
    lost.close();
    finished = true;
}, function(e) {
    __test_clearAccessPolicy();
    assert(false, 'the revocation flow threw: ' + e);
});

function isRevoked(e, label) {
    assert(e && typeof e === 'object', label + ' got an error: ' + e);
    if (!e || typeof e !== 'object') return;
    assertEqual(e.name, 'PermissionRevoked', label + ' is a PermissionRevoked');
}

globalThis.__test_onDone = function() {
    assert(finished, 'the revocation flow ran');
    assertEqual(heldBefore, 0, 'nothing was held before');
    assert(R.heldOpen >= 6, 'server, accepted and client sockets, fetch, child and watcher are held: ' + R.heldOpen);
    assert(R.revoked >= 5, 'the grant change closed what it refused: ' + R.revoked);
    isRevoked(R.serverErr, 'the listening server');
    isRevoked(R.clientErr, 'the connected socket');
    isRevoked(R.fetchErr, 'the streaming body');
    isRevoked(R.childErr, 'the child process');
    assert(R.childExit !== null, 'and the child exited');
    isRevoked(R.watchErr, 'the watcher');
    assertEqual(R.keptServer, null, 'a server whose grant was kept stays up');
    assert(bodyRead > 0, 'the body streamed before the change');
    // The kept server, and the connection it accepted for the fetch until it
    // reads the fetch's end.
    assert(R.heldAfter >= 1 && R.heldAfter <= 2, 'only the kept server is still held: ' + R.heldAfter);
    fs.rmSync(root, { recursive: true, force: true });
};
