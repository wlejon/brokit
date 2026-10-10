// Test: process.kill(pid, signal), as Node has it

var isWin = process.platform === 'win32';
var spawnAsync = globalThis.__brokit_cp_spawnAsync;
var childPoll = globalThis.__brokit_cp_childPoll;

assert(typeof process.kill === 'function', 'process.kill exists');

function codeOf(fn) {
    try { fn(); } catch (e) { return e && (e.code || e.name); }
    return null;
}
function waitExit(id) {
    for (var i = 0; i < 400; i++) {
        var r = childPoll(id);
        if (r !== null) return r;
        var t0 = Date.now();
        while (Date.now() - t0 < 10) {}
    }
    return null;
}

// This process exists.
assertEqual(process.kill(process.pid, 0), true, 'signal 0 on this process is true');

// A child: alive, then ended by SIGTERM, then gone.
var child = isWin ? spawnAsync('ping', ['-n', '30', '127.0.0.1']) : spawnAsync('sleep', ['30']);
assert(typeof child.pid === 'number' && child.pid > 0, 'a child to signal: ' + child.pid);
assertEqual(process.kill(child.pid, 0), true, 'signal 0: the child exists');
assertEqual(process.kill(child.pid), true, 'the default signal (SIGTERM) is sent');
var exit = waitExit(child.id);
assert(exit !== null, 'the child ended');
if (exit && isWin) assertEqual(exit.exitCode, 1, 'TerminateProcess ends it with code 1, as libuv');
if (exit && !isWin) assertEqual(exit.signal, 'SIGTERM', 'it ended on SIGTERM');
assertEqual(codeOf(function() { process.kill(child.pid, 0); }), 'ESRCH', 'signal 0 on an exited child: ESRCH');

// By number, SIGKILL.
var child2 = isWin ? spawnAsync('ping', ['-n', '30', '127.0.0.1']) : spawnAsync('sleep', ['30']);
assertEqual(process.kill(child2.pid, 9), true, 'SIGKILL by number');
assert(waitExit(child2.id) !== null, 'SIGKILL ended the child');

// The error has Node's shape.
var err = null;
try { process.kill(child2.pid, 'SIGTERM'); } catch (e) { err = e; }
assert(err instanceof Error, 'a failed kill throws an Error');
assertEqual(err && err.code, 'ESRCH', 'code ESRCH');
assertEqual(err && err.syscall, 'kill', 'syscall kill');
assert(err && typeof err.errno === 'number' && err.errno < 0, 'errno is negative, as Node');

// Bad arguments.
assertEqual(codeOf(function() { process.kill('123'); }), 'TypeError', 'a pid that is not a number');
assertEqual(codeOf(function() { process.kill(process.pid, 'SIGNOPE'); }), 'TypeError', 'an unknown signal name');
if (isWin) {
    assertEqual(codeOf(function() { process.kill(process.pid, 'SIGHUP'); }), 'ENOSYS',
                'Windows ends a process only for SIGINT, SIGQUIT, SIGTERM, SIGKILL');
}
