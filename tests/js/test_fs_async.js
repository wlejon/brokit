// fs.promises and the callback functions are asynchronous for real: the I/O
// runs on brokit's fs threads and the promise settles on a later tick, so a
// page that awaits a slow disk keeps drawing. They used to run the
// synchronous call in place and hand back a settled promise, which froze a
// page walking a whole drive.

var fs = globalThis.__brokit_fs;
var root = 'brokit_fs_async_test';
fs.rmSync(root, { recursive: true, force: true });

// ── Nothing settles inside the call ───────────────────────────────────────
var settledInCall = false;
fs.promises.mkdir(root).then(function() { settledInCall = true; });
// Microtasks run when this script's turn ends; the I/O has not been
// delivered by then, so this still reads false even after a checkpoint.
assertEqual(settledInCall, false, 'fs.promises.mkdir has not settled inside the call');

var log = [];
var errors = {};
var bytes = null;
var dirents = null;
var stats = null;
var linkStats = null;
var real = null;
var copied = null;

function step(name, p) {
    return p.then(function(v) { log.push(name); return v; });
}

var chain = step('mkdir', fs.promises.mkdir(root + '/a/b', { recursive: true }))
    .then(function() { return step('writeFile', fs.promises.writeFile(root + '/a/one.txt', 'hello')); })
    .then(function() { return step('appendFile', fs.promises.appendFile(root + '/a/one.txt', ' world')); })
    .then(function() { return step('writeBytes', fs.promises.writeFile(root + '/a/b/two.bin', new Uint8Array([1, 2, 3, 250]))); })
    .then(function() { return step('readFile', fs.promises.readFile(root + '/a/b/two.bin')); })
    .then(function(b) { bytes = b; return step('readdir', fs.promises.readdir(root + '/a', { withFileTypes: true })); })
    .then(function(d) { dirents = d; return step('stat', fs.promises.stat(root + '/a/one.txt')); })
    .then(function(s) { stats = s; return step('lstat', fs.promises.lstat(root + '/a/b')); })
    .then(function(s) { linkStats = s; return step('copyFile', fs.promises.copyFile(root + '/a/one.txt', root + '/a/copy.txt')); })
    .then(function() { return step('readCopy', fs.promises.readFile(root + '/a/copy.txt', 'utf8')); })
    .then(function(t) { copied = t; return step('rename', fs.promises.rename(root + '/a/copy.txt', root + '/a/moved.txt')); })
    .then(function() { return step('realpath', fs.promises.realpath(root + '/a/moved.txt')); })
    .then(function(r) { real = r; return step('access', fs.promises.access(root + '/a/moved.txt')); })
    .then(function() { return fs.promises.access(root + '/nope').catch(function(e) { errors.access = e; }); })
    .then(function() { return fs.promises.readdir(root + '/nope').catch(function(e) { errors.readdir = e; }); })
    .then(function() { return fs.promises.unlink(root + '/nope').catch(function(e) { errors.unlink = e; }); })
    .then(function() { return step('rmForce', fs.promises.rm(root + '/nope', { force: true })); })
    .then(function() { return step('unlink', fs.promises.unlink(root + '/a/moved.txt')); })
    .then(function() { return step('rm', fs.promises.rm(root + '/a', { recursive: true })); })
    .catch(function(e) { errors.chain = e; });

// The callback form: later, with (err, value).
var cbOrder = [];
fs.stat('.', function(err) { cbOrder.push(err ? 'err' : 'stat'); });
cbOrder.push('after call');

globalThis.__test_onDone = function() {
    assertEqual(settledInCall, true, 'the mkdir settled later');
    assert(!errors.chain, 'no step failed: ' + (errors.chain && errors.chain.message));
    assertEqual(log.join(','),
        'mkdir,writeFile,appendFile,writeBytes,readFile,readdir,stat,lstat,copyFile,readCopy,rename,realpath,access,rmForce,unlink,rm',
        'every step ran, in order');
    assert(bytes instanceof Uint8Array && bytes.length === 4 && bytes[3] === 250, 'readFile without an encoding answers bytes');
    var names = (dirents || []).map(function(d) { return d.name + (d.isDirectory() ? '/' : ''); }).sort();
    assertEqual(names.join(' '), 'b/ one.txt', 'readdir withFileTypes answers dirents');
    assert(stats && stats.isFile() && stats.size === 11 && stats.mtimeMs > 0, 'stat: a file of 11 bytes, with an mtime');
    assert(linkStats && linkStats.isDirectory() && !linkStats.isSymbolicLink(), 'lstat of a directory');
    assertEqual(copied, 'hello world', 'copyFile copied');
    assert(typeof real === 'string' && /moved\.txt$/.test(real), 'realpath answers a path');
    assert(errors.access instanceof Error && errors.access.code === 'ENOENT', 'access of a missing path rejects ENOENT');
    assert(errors.readdir instanceof Error && errors.readdir.code === 'ENOENT' && errors.readdir.syscall === 'scandir',
        'readdir of a missing path rejects ENOENT, scandir');
    assert(errors.unlink && errors.unlink.code === 'ENOENT', 'unlink of a missing path rejects');
    assertEqual(cbOrder.join(','), 'after call,stat', 'a callback runs after the call returns');
    fs.rmSync(root, { recursive: true, force: true });
    assert(!fs.existsSync(root), 'cleaned up');
};
