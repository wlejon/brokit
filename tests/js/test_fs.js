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

var cbResult = null;
var cbErr = null;
fs.readFile(cbFile, 'utf8', function(err, data) {
    cbErr = err;
    cbResult = data;
});
assertEqual(cbErr, null, 'readFile callback no error');
assertEqual(cbResult, 'callback', 'readFile callback result');

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

// ── Cleanup ───────────────────────────────────────────────────────────────
fs.rmSync(testDir, { recursive: true, force: true });
assert(!fs.existsSync(testDir), 'cleanup: test directory removed');
