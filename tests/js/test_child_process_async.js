// Test: child_process async / advanced options

var cp = globalThis.__brokit_child_process;
var isWin = process.platform === 'win32';

// ── spawnAsync / childPoll / childKill ───────────────────────────────────
var spawnAsync = globalThis.__brokit_cp_spawnAsync;
var childPoll = globalThis.__brokit_cp_childPoll;
var childKill = globalThis.__brokit_cp_childKill;

assert(typeof spawnAsync === 'function', '__brokit_cp_spawnAsync exists');
assert(typeof childPoll === 'function', '__brokit_cp_childPoll exists');
assert(typeof childKill === 'function', '__brokit_cp_childKill exists');

// Quick-exit child — spawn then poll to completion.
var handle;
if (isWin) {
    handle = spawnAsync('cmd', ['/c', 'exit', '0']);
} else {
    handle = spawnAsync('true', []);
}
assert(typeof handle === 'object', 'spawnAsync returns handle');
assert(typeof handle.id === 'number', 'handle.id is number');
assert(typeof handle.pid === 'number', 'handle.pid is number');

// Poll until exit (busy-loop, short-lived child)
var pollResult = null;
for (var i = 0; i < 200 && pollResult === null; i++) {
    pollResult = childPoll(handle.id);
    if (pollResult === null) {
        // tiny sleep via spinning in-place — busy wait but bounded
        var t0 = Date.now();
        while (Date.now() - t0 < 25) {}
    }
}
assert(pollResult !== null, 'childPoll eventually returns exit info');
if (pollResult) {
    assertEqual(pollResult.exitCode, 0, 'spawnAsync child exitCode 0');
    assertEqual(pollResult.signal, null, 'spawnAsync child signal null');
}

// childPoll on unknown id throws RangeError
var pollThrew = false;
try { childPoll(handle.id); } catch (e) { pollThrew = true; }
assert(pollThrew, 'childPoll on stale id throws');

// childKill on unknown id returns false (no throw)
assertEqual(childKill(999999), false, 'childKill on unknown id returns false');

// spawnAsync + childKill on a longer-running child
var longHandle;
if (isWin) {
    longHandle = spawnAsync('cmd', ['/c', 'ping', '-n', '20', '127.0.0.1']);
} else {
    longHandle = spawnAsync('sleep', ['10']);
}
assert(typeof longHandle.id === 'number', 'long-running spawnAsync handle');
var killed = childKill(longHandle.id);
assertEqual(killed, true, 'childKill returns true for live id');

// After kill, poll should eventually report exit
var killPoll = null;
for (var i = 0; i < 200 && killPoll === null; i++) {
    killPoll = childPoll(longHandle.id);
    if (killPoll === null) {
        var t0 = Date.now();
        while (Date.now() - t0 < 25) {}
    }
}
assert(killPoll !== null, 'childPoll observes kill exit');

// spawnAsync without args array — third arg position shifts
var noArgHandle;
if (isWin) {
    noArgHandle = spawnAsync('cmd', ['/c', 'exit', '0']);
} else {
    noArgHandle = spawnAsync('true');
}
assert(typeof noArgHandle.id === 'number', 'spawnAsync without args array');
// drain
for (var i = 0; i < 200; i++) {
    var r = childPoll(noArgHandle.id);
    if (r !== null) break;
    var t0 = Date.now();
    while (Date.now() - t0 < 25) {}
}

// ── execSync timeout option ──────────────────────────────────────────────
// On Windows, `ping -n 5` takes ~4s; 200ms timeout should kill it.
var timedOut = false;
try {
    if (isWin) {
        cp.execSync('ping -n 5 127.0.0.1', { timeout: 200 });
    } else {
        cp.execSync('sleep 5', { timeout: 200 });
    }
} catch (e) {
    timedOut = true;
}
assert(timedOut, 'execSync honors timeout');

// ── execSync maxBuffer option ─────────────────────────────────────────────
// Generate output larger than maxBuffer; output gets truncated.
var truncResult;
if (isWin) {
    truncResult = cp.execSync('echo aaaaaaaaaaaaaaaaaaaaaa', { maxBuffer: 4 });
} else {
    truncResult = cp.execSync('echo aaaaaaaaaaaaaaaaaaaaaa', { maxBuffer: 4 });
}
assert(typeof truncResult === 'string', 'maxBuffer execSync returns string');
assert(truncResult.length <= 16, 'maxBuffer truncates output');

// ── spawnAsync of nonexistent binary throws ──────────────────────────────
var badSpawnThrew = false;
try {
    spawnAsync('this_binary_does_not_exist_brokit_12345', []);
} catch (e) {
    badSpawnThrew = true;
}
assert(badSpawnThrew, 'spawnAsync of missing binary throws');
