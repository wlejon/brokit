// writeSync / fs.write take a string as Node does (fd, string, position,
// encoding); fsyncSync / fdatasyncSync / fs.fsync flush an open fd; mkdtemp
// makes a fresh directory. writeSync used to throw on a string, and fsync and
// mkdtemp were missing.

var fs = globalThis.__brokit_fs;
// Per process: this test and its _gcstress twin run side by side under ctest -j.
var root = 'brokit_fs_fd_write_test_' + process.pid;
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(root);

function bytesOf(path) { return Array.prototype.slice.call(fs.readFileSync(path)).join(','); }

// ── writeSync(fd, string[, position[, encoding]]) ─────────────────────────
var f = root + '/s.txt';
var fd = fs.openSync(f, 'w');
var n = fs.writeSync(fd, 'héllo €');
assertEqual(n, 10, 'utf8 by default: bytes written, not characters');
assertEqual(fs.writeSync(fd, '!'), 1, 'appends at the current position');
fs.closeSync(fd);
assertEqual(fs.readFileSync(f, 'utf8'), 'héllo €!', 'utf8 multibyte text round-trips');

fd = fs.openSync(f, 'r+');
assertEqual(fs.writeSync(fd, 'J', 0), 1, 'a string written at a position');
assertEqual(fs.writeSync(fd, 'X', 3, 'utf8'), 1, 'a position and an encoding');
fs.closeSync(fd);
assertEqual(fs.readFileSync(f, 'utf8'), 'JéX' + 'lo €!', 'the positioned writes overwrote in place');

fd = fs.openSync(root + '/enc.bin', 'w');
assertEqual(fs.writeSync(fd, 'éÿ', null, 'latin1'), 2, 'latin1: one byte a character');
assertEqual(fs.writeSync(fd, 'A', null, 'binary'), 1, 'binary is latin1');
assertEqual(fs.writeSync(fd, 'deadBEEF', null, 'hex'), 4, 'hex');
assertEqual(fs.writeSync(fd, 'AQID', null, 'base64'), 3, 'base64');
assertEqual(fs.writeSync(fd, 'z', null, 'ascii'), 1, 'ascii');
assertEqual(fs.writeSync(fd, 'hi', null, 'ucs2'), 4, 'ucs2');
assertEqual(fs.writeSync(fd, 'k', null, 'utf16le'), 2, 'utf16le');
fs.closeSync(fd);
assertEqual(bytesOf(root + '/enc.bin'),
    '233,255,65,222,173,190,239,1,2,3,122,104,0,105,0,107,0', 'each encoding wrote its bytes');

var threw = null;
fd = fs.openSync(root + '/bad.txt', 'w');
try { fs.writeSync(fd, 'x', null, 'klingon'); } catch (e) { threw = e; }
assert(threw instanceof TypeError, 'an unknown encoding throws a TypeError');

// ── the buffer forms still work ──────────────────────────────────────────
var buf = new Uint8Array([10, 20, 30, 40, 50]);
assertEqual(fs.writeSync(fd, buf), 5, 'the whole buffer');
assertEqual(fs.writeSync(fd, buf, 3), 2, 'from an offset to the end');
assertEqual(fs.writeSync(fd, buf, 1, 2), 2, 'offset and length');
assertEqual(fs.writeSync(fd, buf, { offset: 4, length: 1 }), 1, 'the options form');
if (typeof Buffer === 'function') {
    assertEqual(fs.writeSync(fd, Buffer.from('ok')), 2, 'a Buffer');
}
assertEqual(fs.writeSync(fd, buf, 2, 1, 0), 1, 'offset, length and position');
fs.closeSync(fd);
assertEqual(bytesOf(root + '/bad.txt'),
    typeof Buffer === 'function' ? '30,20,30,40,50,40,50,20,30,50,111,107' : '30,20,30,40,50,40,50,20,30,50',
    'the buffer writes landed');

// ── fsyncSync / fdatasyncSync ────────────────────────────────────────────
fd = fs.openSync(root + '/sync.txt', 'w');
fs.writeSync(fd, 'durable');
assertEqual(fs.fsyncSync(fd), undefined, 'fsyncSync on an open fd answers undefined');
assertEqual(fs.fdatasyncSync(fd), undefined, 'fdatasyncSync on an open fd answers undefined');
assertEqual(fs.readFileSync(root + '/sync.txt', 'utf8'), 'durable', 'fsync pushed the buffered write to the file');
fs.closeSync(fd);

var rfd = fs.openSync(root + '/sync.txt', 'r');
assertEqual(fs.fsyncSync(rfd), undefined, 'fsyncSync on a read-only fd');
fs.closeSync(rfd);

threw = null;
try { fs.fsyncSync(fd); } catch (e) { threw = e; }
assert(threw && threw.code === 'EBADF' && threw.syscall === 'fsync', 'fsyncSync of a closed fd throws EBADF');
threw = null;
try { fs.fdatasyncSync(987654); } catch (e) { threw = e; }
assert(threw && threw.code === 'EBADF' && threw.syscall === 'fdatasync', 'fdatasyncSync of a bad fd throws EBADF');

// ── mkdtempSync ──────────────────────────────────────────────────────────
var t1 = fs.mkdtempSync(root + '/tmp-');
var t2 = fs.mkdtempSync(root + '/tmp-');
assert(/\/tmp-[A-Za-z0-9]{6}$/.test(t1), 'prefix + six characters: ' + t1);
assert(t1 !== t2, 'two calls make two directories');
assert(fs.statSync(t1).isDirectory() && fs.statSync(t2).isDirectory(), 'both exist as directories');
if (process.platform !== 'win32') {
    assertEqual(fs.statSync(t1).mode & 511, 448, 'made with mode 0700');
}
threw = null;
try { fs.mkdtempSync(root + '/no/such/dir/x-'); } catch (e) { threw = e; }
assert(threw && threw.code === 'ENOENT' && threw.syscall === 'mkdtemp', 'mkdtempSync under a missing directory throws ENOENT');

// ── the asynchronous forms ───────────────────────────────────────────────
var order = [];
var results = {};
var afd = fs.openSync(root + '/async.txt', 'w');
fs.write(afd, 'hé!', function(err, written, data) {
    order.push('write');
    results.write = { err: err, written: written, data: data };
    fs.write(afd, new Uint8Array([65, 66]), 0, 2, null, function(err2, written2) {
        results.writeBuf = { err: err2, written: written2 };
        fs.fsync(afd, function(err3) {
            results.fsync = { err: err3, text: fs.readFileSync(root + '/async.txt', 'utf8') };
            fs.fdatasync(afd, function(err4) {
                results.fdatasync = err4;
                fs.closeSync(afd);
                fs.fsync(afd, function(err5) { results.fsyncBad = err5; });
            });
        });
    });
});
order.push('after call');

fs.write(afd, 'x', null, 'klingon', function(err) { results.writeBadEnc = err; });

fs.mkdtemp(root + '/cb-', function(err, dir) { results.mkdtemp = { err: err, dir: dir }; });
fs.promises.mkdtemp(root + '/p-').then(
    function(dir) { results.pmkdtemp = dir; },
    function(err) { results.pmkdtemp = err; });

globalThis.__test_onDone = function() {
    assertEqual(order.join(','), 'after call,write', 'fs.write calls back after the call returns');
    assert(results.write && results.write.err === null, 'fs.write of a string: no error');
    assertEqual(results.write && results.write.written, 4, 'fs.write of a string: bytes written');
    assertEqual(results.write && results.write.data, 'hé!', 'fs.write hands the string back');
    assert(results.writeBuf && results.writeBuf.err === null && results.writeBuf.written === 2, 'fs.write of a buffer');
    assert(results.fsync && results.fsync.err === null, 'fs.fsync calls back without an error');
    assertEqual(results.fsync && results.fsync.text, 'hé!AB', 'the async writes reached the file');
    assertEqual(results.fdatasync, null, 'fs.fdatasync calls back without an error');
    assert(results.fsyncBad && results.fsyncBad.code === 'EBADF', 'fs.fsync of a closed fd calls back EBADF');
    assert(results.writeBadEnc instanceof TypeError, 'fs.write with an unknown encoding calls back the error');
    assert(results.mkdtemp && results.mkdtemp.err === null && /\/cb-[A-Za-z0-9]{6}$/.test(results.mkdtemp.dir) &&
           fs.statSync(results.mkdtemp.dir).isDirectory(), 'fs.mkdtemp makes a directory');
    assert(typeof results.pmkdtemp === 'string' && /\/p-[A-Za-z0-9]{6}$/.test(results.pmkdtemp) &&
           fs.existsSync(results.pmkdtemp), 'fs.promises.mkdtemp makes a directory');
    fs.rmSync(root, { recursive: true, force: true });
    assert(!fs.existsSync(root), 'cleaned up');
};
