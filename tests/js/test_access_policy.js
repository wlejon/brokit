// The host's access policy (api.h, "Access policy") is asked where the power
// is exercised: every fs native, require() of a file, sqlite's file opens,
// every way to start a process, and every socket and request. A refusal
// throws (or rejects with) an Error carrying exactly the hook's sentence,
// code EACCES for fs and spawn and ERR_ACCESS_DENIED for net. The harness's
// __test_setAccessPolicy installs a policy refusing what it is given.

var fs = require('fs');
var net = require('net');
var dgram = require('dgram');
var http = require('http');
var sqlite = require('sqlite');
var cp = require('child_process');

// Per process: this test and its _gcstress twin run side by side under ctest -j.
var root = 'brokit_access_test_' + process.pid;
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(root + '/open', { recursive: true });
fs.mkdirSync(root + '/deny', { recursive: true });
fs.mkdirSync(root + '/ro', { recursive: true });
['open', 'deny', 'ro'].forEach(function(d) {
    fs.writeFileSync(root + '/' + d + '/a.txt', d + '-a');
    fs.writeFileSync(root + '/' + d + '/mod.js', 'module.exports = "' + d + '-module";');
});
var absRoot = fs.realpathSync(root);
var sep = absRoot.indexOf('\\') >= 0 ? '\\' : '/';
var absOpen = absRoot + sep + 'open';
var absDeny = absRoot + sep + 'deny';
function fileUrl(abs) {
    var p = abs.replace(/\\/g, '/');
    return p[0] === '/' ? 'file://' + p : 'file:///' + p;
}

// Two local http servers for the redirect check, listening before any policy:
// A answers /ok and redirects /hop to B, which must never be asked.
var bHits = 0;
var serverB = http.createServer(function(req, res) { bHits++; res.writeHead(200); res.end('b'); });
serverB.listen(0, '127.0.0.1');
var portB = serverB.address().port;
var serverA = http.createServer(function(req, res) {
    if (req.url === '/hop') {
        res.writeHead(302, { 'Location': 'http://127.0.0.1:' + portB + '/landed' });
        res.end();
        return;
    }
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('a-ok');
});
serverA.listen(0, '127.0.0.1');
var portA = serverA.address().port;

function thrown(fn) {
    try { fn(); } catch (e) { return e; }
    return null;
}

function isAbsolute(p) { return /^([A-Za-z]:[\\\/]|\/)/.test(p); }

// A refused fs call: the hook's sentence, EACCES, and the absolute path.
function checkFsRefusal(e, kind, tail, label) {
    assert(e !== null, label + ' throws');
    if (!e) return;
    assertEqual(e.code, 'EACCES', label + ' code');
    assert(typeof e.path === 'string' && isAbsolute(e.path), label + ' path is absolute: ' + e.path);
    assert(typeof e.path === 'string' && e.path.replace(/\\/g, '/').slice(-tail.length) === tail,
           label + ' path ends with ' + tail + ': ' + e.path);
    var sentence = (kind === 'write' ? 'No write access to ' : 'No file access to ') + e.path;
    assertEqual(e.message, sentence, label + ' message is the hook\'s sentence');
}

__test_setAccessPolicy({
    fsDenyUnder: [root + '/deny'],
    fsReadOnlyUnder: [root + '/ro'],
    spawn: true,
    net: true
});

// ── fs, synchronous ─────────────────────────────────────────────────────────
assertEqual(fs.readFileSync(root + '/open/a.txt', 'utf8'), 'open-a', 'allowed read');
assertEqual(fs.readFileSync(root + '/ro/a.txt', 'utf8'), 'ro-a', 'read of a read-only dir');
checkFsRefusal(thrown(function() { fs.readFileSync(root + '/deny/a.txt'); }), 'read', 'deny/a.txt', 'readFileSync denied');
checkFsRefusal(thrown(function() { fs.writeFileSync(root + '/ro/b.txt', 'x'); }), 'write', 'ro/b.txt', 'writeFileSync read-only');
checkFsRefusal(thrown(function() { fs.appendFileSync(root + '/ro/a.txt', 'x'); }), 'write', 'ro/a.txt', 'appendFileSync read-only');
checkFsRefusal(thrown(function() { fs.statSync(root + '/deny/a.txt'); }), 'read', 'deny/a.txt', 'statSync denied');
checkFsRefusal(thrown(function() { fs.lstatSync(root + '/deny/a.txt'); }), 'read', 'deny/a.txt', 'lstatSync denied');
checkFsRefusal(thrown(function() { fs.readdirSync(root + '/deny'); }), 'read', '/deny', 'readdirSync denied');
checkFsRefusal(thrown(function() { fs.realpathSync(root + '/deny/a.txt'); }), 'read', 'deny/a.txt', 'realpathSync denied');
checkFsRefusal(thrown(function() { fs.mkdirSync(root + '/ro/sub'); }), 'write', 'ro/sub', 'mkdirSync read-only');
checkFsRefusal(thrown(function() { fs.mkdtempSync(root + '/ro/t-'); }), 'write', 'ro/t-XXXXXX', 'mkdtempSync read-only');
checkFsRefusal(thrown(function() { fs.rmSync(root + '/ro/a.txt', { force: true }); }), 'write', 'ro/a.txt', 'rmSync read-only (force)');
checkFsRefusal(thrown(function() { fs.rmdirSync(root + '/ro'); }), 'write', '/ro', 'rmdirSync read-only');
checkFsRefusal(thrown(function() { fs.unlinkSync(root + '/ro/a.txt'); }), 'write', 'ro/a.txt', 'unlinkSync read-only');
checkFsRefusal(thrown(function() { fs.chmodSync(root + '/ro/a.txt', 420); }), 'write', 'ro/a.txt', 'chmodSync read-only');
checkFsRefusal(thrown(function() { fs.utimesSync(root + '/ro/a.txt', 1, 1); }), 'write', 'ro/a.txt', 'utimesSync read-only');
checkFsRefusal(thrown(function() { fs.truncateSync(root + '/ro/a.txt', 0); }), 'write', 'ro/a.txt', 'truncateSync read-only');
checkFsRefusal(thrown(function() { fs.symlinkSync('a.txt', root + '/ro/link'); }), 'write', 'ro/link', 'symlinkSync read-only');
assertEqual(fs.readFileSync(root + '/ro/a.txt', 'utf8'), 'ro-a', 'the read-only file is untouched');

// existsSync answers false for a refused path rather than throwing.
assertEqual(fs.existsSync(root + '/deny/a.txt'), false, 'existsSync of a denied path is false');
assertEqual(fs.existsSync(root + '/open/a.txt'), true, 'existsSync of an allowed path');

// rename writes both ends; copy reads its source and writes its destination.
checkFsRefusal(thrown(function() { fs.renameSync(root + '/ro/a.txt', root + '/open/moved.txt'); }), 'write', 'ro/a.txt', 'renameSync from read-only');
checkFsRefusal(thrown(function() { fs.renameSync(root + '/open/a.txt', root + '/ro/moved.txt'); }), 'write', 'ro/moved.txt', 'renameSync into read-only');
assertEqual(fs.existsSync(root + '/open/a.txt'), true, 'a refused rename leaves its source');
checkFsRefusal(thrown(function() { fs.copyFileSync(root + '/deny/a.txt', root + '/open/c.txt'); }), 'read', 'deny/a.txt', 'copyFileSync from denied');
checkFsRefusal(thrown(function() { fs.copyFileSync(root + '/open/a.txt', root + '/ro/c.txt'); }), 'write', 'ro/c.txt', 'copyFileSync into read-only');
fs.copyFileSync(root + '/ro/a.txt', root + '/open/c.txt');
assertEqual(fs.readFileSync(root + '/open/c.txt', 'utf8'), 'ro-a', 'copy out of a read-only dir');

// fd opens are asked for what the descriptor can do.
var fd = fs.openSync(root + '/ro/a.txt', 'r');
assert(typeof fd === 'number', 'openSync r of a read-only file');
fs.closeSync(fd);
checkFsRefusal(thrown(function() { fs.openSync(root + '/ro/a.txt', 'w'); }), 'write', 'ro/a.txt', 'openSync w read-only');
checkFsRefusal(thrown(function() { fs.openSync(root + '/ro/a.txt', 'a'); }), 'write', 'ro/a.txt', 'openSync a read-only');
checkFsRefusal(thrown(function() { fs.openSync(root + '/ro/a.txt', 'r+'); }), 'write', 'ro/a.txt', 'openSync r+ read-only');
checkFsRefusal(thrown(function() { fs.openSync(root + '/deny/a.txt', 'r'); }), 'read', 'deny/a.txt', 'openSync r denied');

// fs.watch reads what changes under it.
checkFsRefusal(thrown(function() { fs.watch(root + '/deny', function() {}); }), 'read', '/deny', 'fs.watch denied');

// require() of a file is a read of that file.
assertEqual(require(absOpen + sep + 'mod.js'), 'open-module', 'require of an allowed file');
checkFsRefusal(thrown(function() { require(absDeny + sep + 'mod.js'); }), 'read', 'deny/mod.js', 'require of a denied file');

// sqlite: a file database is opened read-write; ATTACH and VACUUM INTO name files too.
checkFsRefusal(thrown(function() { new sqlite.DatabaseSync(root + '/ro/db.sqlite'); }), 'write', 'ro/db.sqlite', 'sqlite open read-only');
var mem = new sqlite.DatabaseSync(':memory:');
mem.exec('CREATE TABLE t (x)');
var attachErr = thrown(function() { mem.exec("ATTACH DATABASE '" + root + "/deny/x.db' AS other"); });
assert(attachErr !== null && attachErr.code === 'EACCES', 'ATTACH of a denied file is EACCES');
assert(attachErr !== null && /^No write access to |^No file access to /.test(attachErr.message), 'ATTACH carries the hook\'s sentence: ' + (attachErr && attachErr.message));
var vacuumErr = thrown(function() { mem.exec("VACUUM INTO '" + root + "/ro/copy.db'"); });
assert(vacuumErr !== null && vacuumErr.code === 'EACCES', 'VACUUM INTO a read-only dir is EACCES');
assertEqual(fs.existsSync(root + '/ro/copy.db'), false, 'VACUUM INTO wrote nothing');
mem.exec("ATTACH DATABASE ':memory:' AS scratch");
mem.close();

// ── child_process: every form asks, with the program and its arguments ─────
function checkSpawnRefusal(e, line, label) {
    assert(e !== null, label + ' throws');
    if (!e) return;
    assertEqual(e.code, 'EACCES', label + ' code');
    assertEqual(e.message, 'Not allowed to run ' + line, label + ' message');
}
checkSpawnRefusal(thrown(function() { cp.execSync('echo hi'); }), 'echo hi', 'execSync');
checkSpawnRefusal(thrown(function() { cp.spawnSync('node', ['-e', 'a b']); }), 'node -e "a b"', 'spawnSync');
checkSpawnRefusal(thrown(function() { cp.spawnSync('echo', ['x'], { shell: true }); }), 'echo x', 'spawnSync with a shell');
checkSpawnRefusal(thrown(function() { cp.execFileSync('node', ['--version']); }), 'node --version', 'execFileSync');
checkSpawnRefusal(thrown(function() { cp.spawn('node', ['--version']); }), 'node --version', 'spawn');
checkSpawnRefusal(thrown(function() { globalThis.__brokit_cp_exec('echo raw', {}); }), 'echo raw', 'the exec native');

var execErr = null, execFileErr = null;
var asyncSpawn = [
    new Promise(function(resolve) {
        cp.exec('echo later', function(err) { execErr = err; resolve(); });
    }),
    cp.execFile('node', ['--version']).then(function() {}, function(err) { execFileErr = err; })
];

// ── fs, asynchronous: asked on this thread, before anything is queued ──────
var asyncErrs = {};
function expectReject(name, p) {
    return p.then(function() { asyncErrs[name] = 'resolved'; },
                  function(e) { asyncErrs[name] = e; });
}
var asyncFs = [
    expectReject('readFile', fs.promises.readFile(root + '/deny/a.txt')),
    expectReject('writeFile', fs.promises.writeFile(root + '/ro/p.txt', 'x')),
    expectReject('stat', fs.promises.stat(root + '/deny/a.txt')),
    expectReject('readdir', fs.promises.readdir(root + '/deny')),
    expectReject('rm', fs.promises.rm(root + '/ro/a.txt', { force: true })),
    expectReject('renameFrom', fs.promises.rename(root + '/ro/a.txt', root + '/open/r.txt')),
    expectReject('renameTo', fs.promises.rename(root + '/open/a.txt', root + '/ro/r.txt')),
    expectReject('copyFrom', fs.promises.copyFile(root + '/deny/a.txt', root + '/open/r.txt')),
    expectReject('copyTo', fs.promises.copyFile(root + '/open/a.txt', root + '/ro/r.txt')),
    expectReject('access', fs.promises.access(root + '/deny/a.txt')),
    expectReject('mkdtemp', fs.promises.mkdtemp(root + '/ro/t-')),
    expectReject('fileHandle', fs.promises.open(root + '/ro/a.txt', 'w')),
    new Promise(function(resolve) {
        fs.readFile(root + '/deny/a.txt', function(err) { asyncErrs.callback = err || 'no error'; resolve(); });
    }),
    fs.promises.readFile(root + '/ro/a.txt', 'utf8').then(function(t) { asyncErrs.allowedRead = t; })
];

// ── network ─────────────────────────────────────────────────────────────────
function checkNetRefusal(e, url, label) {
    assert(e !== null && e !== undefined, label + ' refused');
    if (!e) return;
    assertEqual(e.code, 'ERR_ACCESS_DENIED', label + ' code');
    assertEqual(e.message, 'No network access to ' + url, label + ' message');
}
checkNetRefusal(thrown(function() { new WebSocket('ws://127.0.0.1:1/socket'); }), 'ws://127.0.0.1:1/socket', 'WebSocket');
checkNetRefusal(thrown(function() { net.connect(1, '127.0.0.1'); }), 'tcp://127.0.0.1:1', 'net.connect');
checkNetRefusal(thrown(function() { net.createServer().listen(0, '127.0.0.1'); }), 'tcp-listen://127.0.0.1:0', 'net listen');
checkNetRefusal(thrown(function() { http.createServer().listen(0, '127.0.0.1'); }), 'tcp-listen://127.0.0.1:0', 'http server listen');
var udp = dgram.createSocket('udp4');
checkNetRefusal(thrown(function() { udp.bind(0, '127.0.0.1'); }), 'udp-bind://127.0.0.1:0', 'udp bind');
checkNetRefusal(thrown(function() { udp.send('x', 9, '127.0.0.1'); }), 'udp://127.0.0.1:9', 'udp send');
udp.close();

var netResults = {};
var blobUrl = URL.createObjectURL(new Blob(['from a blob']));
var asyncNet = [
    fetch('http://127.0.0.1:' + portA + '/ok').then(function() { netResults.http = 'resolved'; },
                                                   function(e) { netResults.http = e; }),
    fetch('data:text/plain,hello').then(function(r) { return r.text(); })
        .then(function(t) { netResults.data = t; }, function(e) { netResults.data = e; }),
    fetch(blobUrl).then(function(r) { return r.text(); })
        .then(function(t) { netResults.blob = t; }, function(e) { netResults.blob = e; }),
    fetch(fileUrl(absOpen + sep + 'a.txt')).then(function(r) { return r.text(); })
        .then(function(t) { netResults.fileAllowed = t; }, function(e) { netResults.fileAllowed = e; }),
    fetch(fileUrl(absDeny + sep + 'a.txt')).then(function() { netResults.fileDenied = 'resolved'; },
                                                 function(e) { netResults.fileDenied = e; }),
    fetch(root + '/deny/a.txt').then(function() { netResults.pathDenied = 'resolved'; },
                                     function(e) { netResults.pathDenied = e; }),
    new Promise(function(resolve) {
        var x = new XMLHttpRequest();
        x.open('GET', 'http://127.0.0.1:' + portA + '/ok');
        x.onerror = function() { netResults.xhr = 'error'; resolve(); };
        x.onload = function() { netResults.xhr = 'loaded'; resolve(); };
        x.send();
    })
];

// ── then a policy that allows A but not B: a redirect is asked about too ───
var phase2 = {};
var finished = false;
Promise.all(asyncSpawn.concat(asyncFs, asyncNet)).then(function() {
    __test_setAccessPolicy({ net: ['http://127.0.0.1:' + portB] });
    return Promise.all([
        fetch('http://127.0.0.1:' + portA + '/ok').then(function(r) { return r.text(); })
            .then(function(t) { phase2.ok = t; }, function(e) { phase2.ok = e; }),
        fetch('http://127.0.0.1:' + portA + '/hop').then(function() { phase2.hop = 'resolved'; },
                                                        function(e) { phase2.hop = e; })
    ]);
}).then(function() {
    phase2.log = __test_accessLog();
    __test_clearAccessPolicy();
    serverA.close();
    serverB.close();
    finished = true;
}, function(e) {
    __test_clearAccessPolicy();
    serverA.close();
    serverB.close();
    assert(false, 'the access policy flow threw: ' + e);
});

globalThis.__test_onDone = function() {
    assert(finished, 'the asynchronous checks all ran');

    checkSpawnRefusal(execErr, 'echo later', 'exec (callback)');
    checkSpawnRefusal(execFileErr, 'node --version', 'execFile (promise)');

    checkFsRefusal(asyncErrs.readFile, 'read', 'deny/a.txt', 'promises.readFile denied');
    checkFsRefusal(asyncErrs.writeFile, 'write', 'ro/p.txt', 'promises.writeFile read-only');
    checkFsRefusal(asyncErrs.stat, 'read', 'deny/a.txt', 'promises.stat denied');
    checkFsRefusal(asyncErrs.readdir, 'read', '/deny', 'promises.readdir denied');
    checkFsRefusal(asyncErrs.rm, 'write', 'ro/a.txt', 'promises.rm read-only');
    checkFsRefusal(asyncErrs.renameFrom, 'write', 'ro/a.txt', 'promises.rename from read-only');
    checkFsRefusal(asyncErrs.renameTo, 'write', 'ro/r.txt', 'promises.rename into read-only');
    checkFsRefusal(asyncErrs.copyFrom, 'read', 'deny/a.txt', 'promises.copyFile from denied');
    checkFsRefusal(asyncErrs.copyTo, 'write', 'ro/r.txt', 'promises.copyFile into read-only');
    checkFsRefusal(asyncErrs.access, 'read', 'deny/a.txt', 'promises.access denied');
    checkFsRefusal(asyncErrs.mkdtemp, 'write', 'ro/t-XXXXXX', 'promises.mkdtemp read-only');
    checkFsRefusal(asyncErrs.fileHandle, 'write', 'ro/a.txt', 'promises.open w read-only');
    checkFsRefusal(asyncErrs.callback, 'read', 'deny/a.txt', 'fs.readFile callback denied');
    assertEqual(asyncErrs.allowedRead, 'ro-a', 'promises.readFile allowed');

    checkNetRefusal(netResults.http, 'http://127.0.0.1:' + portA + '/ok', 'fetch http');
    assertEqual(netResults.data, 'hello', 'fetch data: is not network');
    assertEqual(netResults.blob, 'from a blob', 'fetch blob: is not network');
    assertEqual(netResults.fileAllowed, 'open-a', 'fetch file: of an allowed file');
    checkFsRefusal(netResults.fileDenied, 'read', 'deny/a.txt', 'fetch file: of a denied file');
    checkFsRefusal(netResults.pathDenied, 'read', 'deny/a.txt', 'fetch of a denied relative path');
    assertEqual(netResults.xhr, 'error', 'XMLHttpRequest to http is refused');

    assertEqual(phase2.ok, 'a-ok', 'an allowed fetch goes through');
    checkNetRefusal(phase2.hop, 'http://127.0.0.1:' + portB + '/landed', 'a redirect to a refused URL');
    assertEqual(bHits, 0, 'the refused redirect target was never asked');
    var log = phase2.log || [];
    assert(log.indexOf('net:http://127.0.0.1:' + portA + '/hop') >= 0, 'the redirect\'s first URL was asked');
    assert(log.indexOf('net:http://127.0.0.1:' + portB + '/landed') >= 0, 'the redirect target was asked');

    // Cleared: everything is allowed again.
    assertEqual(fs.readFileSync(root + '/deny/a.txt', 'utf8'), 'deny-a', 'no policy: a read goes through');
    assertEqual(fs.existsSync(root + '/deny/a.txt'), true, 'no policy: existsSync sees the file');
    fs.writeFileSync(root + '/ro/after.txt', 'ok');
    assertEqual(fs.readFileSync(root + '/ro/after.txt', 'utf8'), 'ok', 'no policy: a write goes through');
    assertEqual(String(cp.execSync('echo cleared')).trim(), 'cleared', 'no policy: a process starts');
    fs.rmSync(root, { recursive: true, force: true });
};
