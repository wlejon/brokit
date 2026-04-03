// Test: child_process module
var cp = globalThis.__brokit_child_process;
assert(typeof cp === 'object', 'child_process exists');

var isWin = process.platform === 'win32';

// ── execSync ──────────────────────────────────────────────────────────────

// Basic command
var echoResult = cp.execSync(isWin ? 'echo hello' : 'echo hello');
assert(typeof echoResult === 'string', 'execSync returns string');
assert(echoResult.trim() === 'hello', 'execSync echo');

// execSync with encoding option
var echoUtf8 = cp.execSync(isWin ? 'echo utf8test' : 'echo utf8test', { encoding: 'utf8' });
assert(echoUtf8.trim() === 'utf8test', 'execSync encoding utf8');

// execSync with null encoding returns Uint8Array
var echoBuf = cp.execSync(isWin ? 'echo buffer' : 'echo buffer', { encoding: null });
assert(echoBuf instanceof Uint8Array, 'execSync null encoding returns Uint8Array');
assert(echoBuf.length > 0, 'execSync buffer has content');

// execSync non-zero exit throws
var threw = false;
try {
    cp.execSync(isWin ? 'cmd /c exit 42' : 'exit 42');
} catch (e) {
    threw = true;
    assert(e.status === 42 || e.code === 42 || e.status !== undefined, 'execSync error has status');
    assert(typeof e.stdout !== 'undefined', 'execSync error has stdout');
    assert(typeof e.stderr !== 'undefined', 'execSync error has stderr');
}
assert(threw, 'execSync throws on non-zero exit');

// execSync with multiple commands (piping)
if (isWin) {
    var multiResult = cp.execSync('echo foo & echo bar');
    assert(multiResult.indexOf('foo') !== -1, 'execSync multi-command has foo');
    assert(multiResult.indexOf('bar') !== -1, 'execSync multi-command has bar');
} else {
    var multiResult = cp.execSync('echo foo && echo bar');
    assert(multiResult.indexOf('foo') !== -1, 'execSync multi-command has foo');
    assert(multiResult.indexOf('bar') !== -1, 'execSync multi-command has bar');
}

// ── exec (callback style) ────────────────────────────────────────────────

var cbStdout = null;
var cbStderr = null;
var cbErr = null;
cp.exec(isWin ? 'echo callback_test' : 'echo callback_test', function(err, stdout, stderr) {
    cbErr = err;
    cbStdout = stdout;
    cbStderr = stderr;
});
assertEqual(cbErr, null, 'exec callback no error');
assert(cbStdout.trim() === 'callback_test', 'exec callback stdout');
assert(typeof cbStderr === 'string', 'exec callback stderr is string');

// exec callback with error
var cbErrResult = null;
cp.exec(isWin ? 'cmd /c exit 1' : 'exit 1', function(err, stdout, stderr) {
    cbErrResult = err;
});
assert(cbErrResult !== null, 'exec callback error on non-zero exit');
assert(cbErrResult.code === 1, 'exec callback error code');

// ── exec (Promise style) ─────────────────────────────────────────────────

var promiseResult = null;
cp.exec(isWin ? 'echo promise_test' : 'echo promise_test').then(function(result) {
    promiseResult = result;
});
// Promise resolves synchronously in our implementation

// ── execFileSync ──────────────────────────────────────────────────────────

if (isWin) {
    var fileResult = cp.execFileSync('cmd', ['/c', 'echo', 'filecmd']);
    assert(fileResult.trim() === 'filecmd', 'execFileSync with args');
} else {
    var fileResult = cp.execFileSync('echo', ['filecmd']);
    assert(fileResult.trim() === 'filecmd', 'execFileSync with args');
}

// ── execFile (callback style) ────────────────────────────────────────────

var efStdout = null;
if (isWin) {
    cp.execFile('cmd', ['/c', 'echo', 'execfile_cb'], function(err, stdout, stderr) {
        efStdout = stdout;
    });
} else {
    cp.execFile('echo', ['execfile_cb'], function(err, stdout, stderr) {
        efStdout = stdout;
    });
}
assert(efStdout && efStdout.trim() === 'execfile_cb', 'execFile callback');

// ── spawnSync ─────────────────────────────────────────────────────────────

if (isWin) {
    var spResult = cp.spawnSync('cmd', ['/c', 'echo', 'spawn_test']);
} else {
    var spResult = cp.spawnSync('echo', ['spawn_test']);
}
assert(typeof spResult === 'object', 'spawnSync returns object');
assertEqual(spResult.status, 0, 'spawnSync status 0');
assert(spResult.stdout.trim() === 'spawn_test', 'spawnSync stdout');
assert(typeof spResult.stderr === 'string', 'spawnSync stderr is string');
assertEqual(spResult.signal, null, 'spawnSync no signal');

// spawnSync with non-zero exit
if (isWin) {
    var spFail = cp.spawnSync('cmd', ['/c', 'exit', '7']);
} else {
    var spFail = cp.spawnSync('sh', ['-c', 'exit 7']);
}
assertEqual(spFail.status, 7, 'spawnSync non-zero status');

// spawnSync with input
if (isWin) {
    var spInput = cp.spawnSync('findstr', ['.'], { input: 'hello from stdin' });
    assert(spInput.stdout.indexOf('hello from stdin') !== -1, 'spawnSync with input');
} else {
    var spInput = cp.spawnSync('cat', [], { input: 'hello from stdin' });
    assert(spInput.stdout.indexOf('hello from stdin') !== -1, 'spawnSync with input');
}

// ── cwd option ────────────────────────────────────────────────────────────

var tmpDir = globalThis.__brokit_os.tmpdir();
if (isWin) {
    var cwdResult = cp.execSync('cd', { cwd: tmpDir });
} else {
    var cwdResult = cp.execSync('pwd', { cwd: tmpDir });
}
assert(cwdResult.trim().length > 0, 'execSync cwd option works');

// ── Use fs + child_process together ───────────────────────────────────────

var fs = globalThis.__brokit_fs;
var testDir = tmpDir + '/brokit_cp_test_' + Date.now();
fs.mkdirSync(testDir);

// Write a file, then read it with a command
fs.writeFileSync(testDir + '/test.txt', 'hello from fs');
if (isWin) {
    var catResult = cp.execSync('type "' + testDir + '\\test.txt"');
} else {
    var catResult = cp.execSync('cat "' + testDir + '/test.txt"');
}
assert(catResult.indexOf('hello from fs') !== -1, 'fs + child_process integration');

// Cleanup
fs.rmSync(testDir, { recursive: true, force: true });
