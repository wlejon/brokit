// child_process spawn with stdio:'pipe' — streaming stdout/stderr, writable
// stdin, exit/close ordering, backpressure on bulk output.
//
// Structure note: the harness pump loop keeps firing timers while
// __brokit_cp_has_pending reports a live child, so final assertions run inside
// 'close' handlers (guaranteed to fire before the pump gives up).

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const isWin = process.platform === 'win32';

// Shell wrappers. Every child below is a stock system binary so the test has
// no fixture binary to build.
function sh(script) {
    return isWin ? { file: 'cmd', args: ['/c', script] }
                 : { file: 'sh',  args: ['-c', script] };
}

// ── default stdio is still 'ignore' (no pipes, no behaviour change) ─────────

const quiet = cp.spawn(sh('echo hidden').file, sh('echo hidden').args);
assert(quiet.stdout === null, 'default spawn has no stdout stream');
assert(quiet.stderr === null, 'default spawn has no stderr stream');
assert(quiet.stdin === null, 'default spawn has no stdin stream');

let quietExited = false, quietClosed = false;
quiet.on('exit', function () { quietExited = true; });
quiet.on('close', function () {
    quietClosed = true;
    assert(quietExited, 'non-piped: exit fires before close');
    assertEqual(quiet.exitCode, 0, 'non-piped child exits 0');
});

// ── stdio:'pipe' + encoding:'utf8' → decoded string chunks ─────────────────

const talker = sh('echo hello pipes');
const p1 = cp.spawn(talker.file, talker.args, { stdio: 'pipe', encoding: 'utf8' });

assert(p1.stdout !== null, 'piped spawn exposes stdout');
assert(p1.stderr !== null, 'piped spawn exposes stderr');
assert(p1.stdin !== null, 'piped spawn exposes stdin');

let p1Out = '';
let p1Exited = false;
let p1StdoutEnded = false;

p1.stdout.on('data', function (chunk) {
    assert(typeof chunk === 'string', 'encoding utf8 delivers string chunks');
    p1Out += chunk;
});
p1.stdout.on('end', function () { p1StdoutEnded = true; });
p1.on('exit', function (code) {
    p1Exited = true;
    assertEqual(code, 0, 'piped child exit code 0');
});
p1.on('close', function (code) {
    assert(p1Exited, 'piped: exit fires before close');
    assert(p1StdoutEnded, 'piped: stdout end fires before close');
    assertEqual(code, 0, 'close reports exit code');
    // All output must have been delivered by the time close fires.
    assert(p1Out.indexOf('hello pipes') !== -1,
           'stdout captured through pipe (got: ' + JSON.stringify(p1Out) + ')');
});

// ── binary is the default chunk type ───────────────────────────────────────

const bin = sh('echo bytes');
const p2 = cp.spawn(bin.file, bin.args, { stdio: 'pipe' });
let p2SawBinary = false;
let p2Bytes = 0;
p2.stdout.on('data', function (chunk) {
    if (chunk instanceof Uint8Array) p2SawBinary = true;
    p2Bytes += chunk.length;
});
p2.on('close', function () {
    assert(p2SawBinary, 'default piped chunks are Uint8Array');
    assert(p2Bytes > 0, 'binary chunk carried bytes');
});

// ── stderr is captured separately from stdout ──────────────────────────────

const noisy = isWin ? sh('echo OUT& echo ERR 1>&2')
                    : sh('echo OUT; echo ERR 1>&2');
const p3 = cp.spawn(noisy.file, noisy.args, { stdio: 'pipe', encoding: 'utf8' });
let p3Out = '', p3Err = '';
p3.stdout.on('data', function (c) { p3Out += c; });
p3.stderr.on('data', function (c) { p3Err += c; });
p3.on('close', function () {
    assert(p3Out.indexOf('OUT') !== -1, 'stdout stream got OUT');
    assert(p3Err.indexOf('ERR') !== -1, 'stderr stream got ERR');
    assert(p3Out.indexOf('ERR') === -1, 'stdout stream did not get stderr text');
});

// ── stdin: write + end, child echoes it back ───────────────────────────────
//
// `sort` exists on both platforms and reads stdin to EOF before writing, so it
// only completes if stdin.end() actually closes the pipe.

const p4 = cp.spawn('sort', [], { stdio: 'pipe', encoding: 'utf8' });
let p4Out = '';
p4.stdout.on('data', function (c) { p4Out += c; });
p4.on('close', function () {
    assert(p4Out.indexOf('brokit-stdin-roundtrip') !== -1,
           'stdin write reached the child and came back (got: ' + JSON.stringify(p4Out) + ')');
});
const wrote = p4.stdin.write('brokit-stdin-roundtrip\n');
assert(wrote > 0, 'stdin.write reports bytes written');
p4.stdin.end();
assertEqual(p4.stdin.write('ignored'), -1, 'write after end returns -1');

// stdin also accepts binary — the branch a rawvideo/PCM feed uses. Writing a
// TypedArray must not leave a stale pending exception behind from the type
// probe, so a plain string write immediately after still has to work.
const p7 = cp.spawn('sort', [], { stdio: 'pipe', encoding: 'utf8' });
let p7Out = '';
p7.stdout.on('data', function (c) { p7Out += c; });
p7.on('close', function () {
    assert(p7Out.indexOf('binary-then-text') !== -1,
           'binary stdin write round-tripped (got: ' + JSON.stringify(p7Out) + ')');
});
const binPayload = new TextEncoder().encode('binary-');
assertEqual(p7.stdin.write(binPayload), binPayload.length,
            'stdin.write accepts a Uint8Array and reports its length');
assert(p7.stdin.write('then-text\n') > 0,
       'string write still works right after a binary write');
p7.stdin.end();

// ── bulk output: every byte survives the backpressure path ─────────────────
//
// highWaterMark is set below the payload so the reader actually parks on the
// full-buffer wait and the child blocks in write() — the path that would
// deadlock or drop bytes if the drain/notify handshake were wrong.

const bulkPath = path.join(os.tmpdir(), 'brokit_cp_bulk.txt');
const LINE = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde\n'; // 64 B
const LINES = 4096;                                                  // 256 KB
fs.writeFileSync(bulkPath, LINE.repeat(LINES));
const expectedBulk = LINE.length * LINES;

// No shell here: spawn quotes each arg individually, so a path handed to a
// shell string would end up double-quoted. Passing it as its own argv entry is
// both correct and what a real caller does.
const dump = isWin ? { file: 'cmd', args: ['/c', 'type', bulkPath] }
                   : { file: 'cat', args: [bulkPath] };
const p5 = cp.spawn(dump.file, dump.args, { stdio: 'pipe', highWaterMark: 16 * 1024 });
let p5Bytes = 0;
let p5Chunks = 0;
p5.stdout.on('data', function (chunk) { p5Bytes += chunk.length; p5Chunks++; });
p5.on('close', function () {
    // Windows `type` rewrites \n as \r\n, so compare against both.
    assert(p5Bytes === expectedBulk || p5Bytes === expectedBulk + LINES,
           'bulk stdout delivered every byte (got ' + p5Bytes + ', expected ' +
           expectedBulk + ' or ' + (expectedBulk + LINES) + ')');
    assert(p5Chunks > 1, 'bulk output arrived as multiple chunks');
    try { fs.unlinkSync(bulkPath); } catch (e) { /* best effort */ }
});

// ── kill a long-running piped child → close still fires ────────────────────

const sleeper = isWin ? sh('ping -n 30 127.0.0.1 >nul') : sh('sleep 30');
const p6 = cp.spawn(sleeper.file, sleeper.args, { stdio: 'pipe' });
let p6Closed = false;
p6.on('close', function () {
    p6Closed = true;
    assert(p6.killed, 'killed flag set on the killed child');
});
assertEqual(p6.kill('SIGKILL'), true, 'kill returns true for a live piped child');

// ── native guards ──────────────────────────────────────────────────────────

let readThrew = false;
try {
    globalThis.__brokit_cp_childRead(quiet._id);
} catch (e) {
    readThrew = true;
}
assert(readThrew, 'childRead on a non-piped child throws');

let unknownThrew = false;
try {
    globalThis.__brokit_cp_childRead(987654);
} catch (e) {
    unknownThrew = true;
}
assert(unknownThrew, 'childRead on an unknown id throws');

assertEqual(globalThis.__brokit_cp_childRelease(987654), false,
            'childRelease on an unknown id returns false');
assertEqual(globalThis.__brokit_cp_childCloseStdin(987654), false,
            'childCloseStdin on an unknown id returns false');

assert(typeof globalThis.__brokit_cp_has_pending === 'function',
       '__brokit_cp_has_pending installed');
assert(globalThis.__brokit_cp_has_pending() === true,
       'has_pending true while children are live');
