// Test: fs module
var fs = globalThis.__brokit_fs;
assert(typeof fs === 'object', 'fs exists');

// Use os.tmpdir for test directory
var tmpBase = globalThis.__brokit_os.tmpdir();
var testDir = tmpBase + '/brokit_fs_test_' + Date.now();

// ── mkdirSync ─────────────────────────────────────────────────────────────
fs.mkdirSync(testDir);
assert(fs.existsSync(testDir), 'mkdirSync created directory');

// mkdirSync recursive
fs.mkdirSync(testDir + '/a/b/c', { recursive: true });
assert(fs.existsSync(testDir + '/a/b/c'), 'mkdirSync recursive');

// mkdirSync on existing dir with recursive should not throw
fs.mkdirSync(testDir + '/a/b/c', { recursive: true });

// ── writeFileSync / readFileSync ──────────────────────────────────────────
var testFile = testDir + '/hello.txt';
fs.writeFileSync(testFile, 'Hello, brokit!');
assert(fs.existsSync(testFile), 'writeFileSync created file');

// readFileSync with encoding
var content = fs.readFileSync(testFile, 'utf8');
assertEqual(content, 'Hello, brokit!', 'readFileSync utf8');

// readFileSync with encoding as options object
var content2 = fs.readFileSync(testFile, { encoding: 'utf-8' });
assertEqual(content2, 'Hello, brokit!', 'readFileSync encoding option object');

// readFileSync without encoding returns Uint8Array
var buf = fs.readFileSync(testFile);
assert(buf instanceof Uint8Array, 'readFileSync no encoding returns Uint8Array');
assert(buf.length > 0, 'readFileSync buffer has content');

// readFileSync nonexistent file throws
var threw = false;
try { fs.readFileSync(testDir + '/nonexistent.txt'); } catch (e) {
    threw = true;
    assertEqual(e.code, 'ENOENT', 'readFileSync ENOENT code');
    assert(e.syscall === 'open', 'readFileSync ENOENT syscall');
}
assert(threw, 'readFileSync throws for missing file');

// ── writeFileSync with Uint8Array ─────────────────────────────────────────
var binFile = testDir + '/binary.dat';
var binData = new Uint8Array([0x48, 0x65, 0x6c, 0x6c, 0x6f]); // "Hello"
fs.writeFileSync(binFile, binData);
var binRead = fs.readFileSync(binFile, 'utf8');
assertEqual(binRead, 'Hello', 'writeFileSync Uint8Array');

// writeFileSync with subarray view (non-zero byteOffset) and ArrayBuffer
var fullAb = new ArrayBuffer(10);
var fullU8 = new Uint8Array(fullAb);
for (var bi = 0; bi < 10; bi++) fullU8[bi] = 65 + bi; // 'A'..'J'
var subView = fullU8.subarray(2, 7); // 'CDEFG'
var subFile = testDir + '/subview.dat';
fs.writeFileSync(subFile, subView);
assertEqual(fs.readFileSync(subFile, 'utf8'), 'CDEFG', 'writeFileSync subarray view');

var abFile = testDir + '/arraybuf.dat';
fs.writeFileSync(abFile, fullAb);
var abRead = fs.readFileSync(abFile);
assertEqual(abRead.length, 10, 'writeFileSync ArrayBuffer length');
assertEqual(abRead[0], 65, 'writeFileSync ArrayBuffer first byte');
assertEqual(abRead[9], 74, 'writeFileSync ArrayBuffer last byte');

// ── Positioned slice reads: readFileSync({ offset, length }) & readFileSliceSync ─
var sliceMid = fs.readFileSync(abFile, { offset: 3, length: 4 });
assert(sliceMid instanceof Uint8Array, 'readFileSync slice returns Uint8Array');
assertEqual(sliceMid.length, 4, 'readFileSync slice middle length');
assertEqual(sliceMid[0], 68, 'readFileSync slice middle[0] == D');
assertEqual(sliceMid[3], 71, 'readFileSync slice middle[3] == G');

var sliceText = fs.readFileSync(abFile, { encoding: 'utf8', offset: 2, length: 5 });
assertEqual(sliceText, 'CDEFG', 'readFileSync slice with encoding');

var sliceTail = fs.readFileSync(abFile, { offset: 7, length: 100 });
assertEqual(sliceTail.length, 3, 'readFileSync slice clamped at EOF');
assertEqual(sliceTail[0], 72, 'readFileSync slice tail[0] == H');
assertEqual(sliceTail[2], 74, 'readFileSync slice tail[2] == J');

var sliceOob = fs.readFileSync(abFile, { offset: 10, length: 5 });
assert(sliceOob instanceof Uint8Array && sliceOob.length === 0, 'readFileSync out-of-bounds offset == fileSize returns empty Uint8Array');

var sliceOob2 = fs.readFileSync(abFile, { offset: 50, length: 5 });
assert(sliceOob2 instanceof Uint8Array && sliceOob2.length === 0, 'readFileSync out-of-bounds offset > fileSize returns empty Uint8Array');

var sliceZeroLen = fs.readFileSync(abFile, { offset: 2, length: 0 });
assert(sliceZeroLen instanceof Uint8Array && sliceZeroLen.length === 0, 'readFileSync length 0 returns empty Uint8Array');

var fastSlice = fs.readFileSliceSync(abFile, 1, 3);
assert(fastSlice instanceof Uint8Array, 'readFileSliceSync returns Uint8Array');
assertEqual(fastSlice.length, 3, 'readFileSliceSync length');
assertEqual(fastSlice[0], 66, 'readFileSliceSync[0] == B');
assertEqual(fastSlice[2], 68, 'readFileSliceSync[2] == D');

var fastSliceTail = fs.readFileSliceSync(abFile, 8, 50);
assertEqual(fastSliceTail.length, 2, 'readFileSliceSync clamped at EOF');
assertEqual(fastSliceTail[0], 73, 'readFileSliceSync tail[0] == I');

var fastSliceOob = fs.readFileSliceSync(abFile, 100, 10);
assertEqual(fastSliceOob.length, 0, 'readFileSliceSync out-of-bounds returns empty Uint8Array');

// Negative offset / length throws RangeError
threw = false;
try { fs.readFileSync(abFile, { offset: -1, length: 4 }); } catch (e) {
    threw = e instanceof RangeError;
}
assert(threw, 'readFileSync negative offset throws RangeError');

threw = false;
try { fs.readFileSync(abFile, { offset: 0, length: -2 }); } catch (e) {
    threw = e instanceof RangeError;
}
assert(threw, 'readFileSync negative length throws RangeError');

threw = false;
try { fs.readFileSliceSync(abFile, -1, 4); } catch (e) {
    threw = e instanceof RangeError;
}
assert(threw, 'readFileSliceSync negative offset throws RangeError');

threw = false;
try { fs.readFileSliceSync(abFile, 0, -1); } catch (e) {
    threw = e instanceof RangeError;
}
assert(threw, 'readFileSliceSync negative length throws RangeError');

// Empty file read
var emptyFile = testDir + '/empty.dat';
fs.writeFileSync(emptyFile, new Uint8Array(0));
var emptyBuf = fs.readFileSync(emptyFile);
assert(emptyBuf instanceof Uint8Array && emptyBuf.length === 0, 'readFileSync empty file');
assertEqual(fs.readFileSync(emptyFile, 'utf8'), '', 'readFileSync empty file utf8');

// ── appendFileSync ────────────────────────────────────────────────────────
fs.appendFileSync(testFile, ' World!');
var appended = fs.readFileSync(testFile, 'utf8');
assertEqual(appended, 'Hello, brokit! World!', 'appendFileSync');

// ── statSync ──────────────────────────────────────────────────────────────
var stat = fs.statSync(testFile);
assert(stat.isFile(), 'statSync isFile');
assert(!stat.isDirectory(), 'statSync not isDirectory');
assert(stat.size > 0, 'statSync size > 0');
assertEqual(typeof stat.mtimeMs, 'number', 'statSync mtimeMs is number');
assert(stat.mtime instanceof Date, 'statSync mtime is Date');

var dirStat = fs.statSync(testDir);
assert(dirStat.isDirectory(), 'statSync directory isDirectory');
assert(!dirStat.isFile(), 'statSync directory not isFile');

// statSync nonexistent throws
threw = false;
try { fs.statSync(testDir + '/nope'); } catch (e) {
    threw = true;
    assertEqual(e.code, 'ENOENT', 'statSync ENOENT');
}
assert(threw, 'statSync throws for missing path');

// ── lstatSync ─────────────────────────────────────────────────────────────
var lstat = fs.lstatSync(testFile);
assert(lstat.isFile(), 'lstatSync isFile');

// ── existsSync ────────────────────────────────────────────────────────────
assert(fs.existsSync(testFile), 'existsSync true for file');
assert(fs.existsSync(testDir), 'existsSync true for dir');
assert(!fs.existsSync(testDir + '/nope'), 'existsSync false for missing');

// ── readdirSync ───────────────────────────────────────────────────────────
var entries = fs.readdirSync(testDir);
assert(Array.isArray(entries), 'readdirSync returns array');
assert(entries.indexOf('hello.txt') !== -1, 'readdirSync contains hello.txt');
assert(entries.indexOf('binary.dat') !== -1, 'readdirSync contains binary.dat');
assert(entries.indexOf('a') !== -1, 'readdirSync contains subdir a');

// readdirSync with withFileTypes
var dirents = fs.readdirSync(testDir, { withFileTypes: true });
assert(Array.isArray(dirents), 'readdirSync withFileTypes returns array');
var helloEntry = dirents.find(function(d) { return d.name === 'hello.txt'; });
assert(helloEntry, 'withFileTypes has hello.txt');
assert(helloEntry.isFile(), 'hello.txt dirent isFile');
assert(!helloEntry.isDirectory(), 'hello.txt dirent not isDirectory');
var aEntry = dirents.find(function(d) { return d.name === 'a'; });
assert(aEntry, 'withFileTypes has subdir a');
assert(aEntry.isDirectory(), 'a dirent isDirectory');

// readdirSync nonexistent throws
threw = false;
try { fs.readdirSync(testDir + '/nonexistent_dir'); } catch (e) {
    threw = true;
    assertEqual(e.code, 'ENOENT', 'readdirSync ENOENT');
}
assert(threw, 'readdirSync throws for missing dir');

// ── copyFileSync ──────────────────────────────────────────────────────────
var copyDest = testDir + '/hello_copy.txt';
fs.copyFileSync(testFile, copyDest);
var copied = fs.readFileSync(copyDest, 'utf8');
assertEqual(copied, 'Hello, brokit! World!', 'copyFileSync');

// ── cpSync ────────────────────────────────────────────────────────────────
fs.mkdirSync(testDir + '/tree/sub', { recursive: true });
fs.writeFileSync(testDir + '/tree/a.txt', 'a');
fs.writeFileSync(testDir + '/tree/sub/b.txt', 'b');
fs.cpSync(testDir + '/tree', testDir + '/tree2/deeper', { recursive: true });
assertEqual(fs.readFileSync(testDir + '/tree2/deeper/a.txt', 'utf8'), 'a', 'cpSync copies a tree');
assertEqual(fs.readFileSync(testDir + '/tree2/deeper/sub/b.txt', 'utf8'), 'b', 'cpSync copies nested directories');
threw = false;
try { fs.cpSync(testDir + '/tree', testDir + '/tree3'); } catch (e) { threw = e.code === 'ERR_FS_EISDIR'; }
assert(threw, 'cpSync refuses a directory without recursive');
fs.cpSync(testDir + '/tree/a.txt', testDir + '/one/two/a.txt');
assertEqual(fs.readFileSync(testDir + '/one/two/a.txt', 'utf8'), 'a', 'cpSync makes a file\'s parent directories');
fs.writeFileSync(testDir + '/one/two/a.txt', 'kept');
fs.cpSync(testDir + '/tree/a.txt', testDir + '/one/two/a.txt', { force: false });
assertEqual(fs.readFileSync(testDir + '/one/two/a.txt', 'utf8'), 'kept', 'cpSync force: false leaves an existing file');
fs.cpSync(testDir + '/tree/a.txt', testDir + '/one/two/a.txt');
assertEqual(fs.readFileSync(testDir + '/one/two/a.txt', 'utf8'), 'a', 'cpSync overwrites by default');

// ── renameSync ────────────────────────────────────────────────────────────
var renamed = testDir + '/renamed.txt';
fs.renameSync(copyDest, renamed);
assert(!fs.existsSync(copyDest), 'renameSync removes old path');
assert(fs.existsSync(renamed), 'renameSync creates new path');
assertEqual(fs.readFileSync(renamed, 'utf8'), 'Hello, brokit! World!', 'renameSync content preserved');

// ── chmodSync ─────────────────────────────────────────────────────────────
// Just verify it doesn't throw
fs.chmodSync(testFile, 0o644);
var statAfterChmod = fs.statSync(testFile);
assert(statAfterChmod.mode !== undefined, 'chmodSync mode available');

// ── realpathSync ──────────────────────────────────────────────────────────
var real = fs.realpathSync(testDir);
assert(typeof real === 'string', 'realpathSync returns string');
assert(real.length > 0, 'realpathSync non-empty');

// ── unlinkSync ────────────────────────────────────────────────────────────
fs.unlinkSync(renamed);
assert(!fs.existsSync(renamed), 'unlinkSync removes file');

// ── rmSync ────────────────────────────────────────────────────────────────
// rmSync with force on nonexistent should not throw
fs.rmSync(testDir + '/doesnotexist', { force: true });

// rmSync single file
var rmFile = testDir + '/to_rm.txt';
fs.writeFileSync(rmFile, 'delete me');
fs.rmSync(rmFile);
assert(!fs.existsSync(rmFile), 'rmSync removes file');

// rmSync recursive
fs.writeFileSync(testDir + '/a/b/c/deep.txt', 'deep');
fs.rmSync(testDir + '/a', { recursive: true });
assert(!fs.existsSync(testDir + '/a'), 'rmSync recursive');

// ── rmdirSync ─────────────────────────────────────────────────────────────
var emptyDir = testDir + '/emptydir';
fs.mkdirSync(emptyDir);
assert(fs.existsSync(emptyDir), 'mkdirSync for rmdirSync test');
fs.rmdirSync(emptyDir);
assert(!fs.existsSync(emptyDir), 'rmdirSync removes empty dir');

// ── Async (callback style) ───────────────────────────────────────────────
var cbFile = testDir + '/cb_test.txt';
fs.writeFileSync(cbFile, 'callback');

// The callback runs later, as in Node: never inside the call.
var cbResult = null;
var cbErr = 'not called';
fs.readFile(cbFile, 'utf8', function(err, data) {
    cbErr = err;
    cbResult = data;
});
assertEqual(cbErr, 'not called', 'readFile callback does not run inside the call');

// ── Async (Promise style) ─────────────────────────────────────────────────
var promiseFile = testDir + '/promise_test.txt';
fs.writeFileSync(promiseFile, 'promise data');

var promiseResult = null;
fs.readFile(promiseFile, 'utf8').then(function(data) {
    promiseResult = data;
});

// ── fs.promises ───────────────────────────────────────────────────────────
var promisesFile = testDir + '/promises_test.txt';
fs.writeFileSync(promisesFile, 'promises api');

var pResult = null;
fs.promises.readFile(promisesFile, 'utf8').then(function(data) {
    pResult = data;
});

var pStatResult = null;
fs.promises.stat(promisesFile).then(function(s) {
    pStatResult = s;
});

// fs.promises.readdir
var pReaddirResult = null;
fs.promises.readdir(testDir).then(function(entries) {
    pReaddirResult = entries;
});

// fs.promises error case
var pError = null;
fs.promises.readFile(testDir + '/nope', 'utf8').catch(function(err) {
    pError = err;
});

// ── fs.constants ──────────────────────────────────────────────────────────
assertEqual(fs.constants.F_OK, 0, 'fs.constants.F_OK');
assertEqual(fs.constants.R_OK, 4, 'fs.constants.R_OK');
assertEqual(fs.constants.W_OK, 2, 'fs.constants.W_OK');

// ── writeFileSync overwrite ───────────────────────────────────────────────
fs.writeFileSync(testFile, 'overwritten');
assertEqual(fs.readFileSync(testFile, 'utf8'), 'overwritten', 'writeFileSync overwrites');

// ── The asynchronous results, once they have all settled ─────────────────
globalThis.__test_onDone = function() {
    assertEqual(cbErr, null, 'readFile callback no error');
    assertEqual(cbResult, 'callback', 'readFile callback result');
    assertEqual(promiseResult, 'promise data', 'fs.readFile without a callback answers a promise');
    assertEqual(pResult, 'promises api', 'fs.promises.readFile utf8');
    assert(pStatResult && pStatResult.isFile() && pStatResult.size === 12, 'fs.promises.stat');
    assert(pReaddirResult && pReaddirResult.indexOf('promises_test.txt') >= 0, 'fs.promises.readdir');
    assert(pError && pError.code === 'ENOENT' && pError instanceof Error, 'fs.promises.readFile rejects ENOENT with an Error');

    // ── Cleanup ───────────────────────────────────────────────────────────
    fs.rmSync(testDir, { recursive: true, force: true });
    assert(!fs.existsSync(testDir), 'cleanup: test directory removed');
};
