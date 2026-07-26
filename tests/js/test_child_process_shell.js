// execFile / spawnSync / spawn take literal argv — no shell in between — while
// exec and execSync still take a shell command string.
//
// The distinction is not cosmetic. These APIs get handed user-chosen file
// paths, and media filenames are full of `&`, `(`, `)` and quotes. Joining
// argv into a shell line (what this used to do) meant such a path was parsed
// as syntax: at best the command failed, at worst the filename ran code. Every
// path case below is named so that a shell would visibly execute an extra
// `echo PWNED`, and every assertion checks it did not.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const isWin = process.platform === 'win32';

// A filename carrying shell metacharacters. `&` is a command separator in both
// cmd and sh, and the parenthesised tail trips cmd's block parser. All of these
// are legal in a Windows filename, so a real user really can produce this.
const HOSTILE_NAME = 'brokit & echo PWNED (test).txt';
const TOKEN = 'brokit-argv-token';
const hostilePath = path.join(os.tmpdir(), HOSTILE_NAME);
fs.writeFileSync(hostilePath, TOKEN + '\n');

// A stock binary (not a shell builtin) that takes a path as its own argv entry
// and prints a matching line. Being a real executable is the point: it can only
// be reached with the path intact if nothing re-parsed the argument.
function grepArgs(p) {
    return isWin ? { file: 'findstr', args: [TOKEN, p] }
                 : { file: 'grep',    args: [TOKEN, p] };
}
const g = grepArgs(hostilePath);

function assertClean(out, what) {
    assert(out.indexOf(TOKEN) !== -1, what + ' reached the file (got: ' + JSON.stringify(out) + ')');
    assert(out.indexOf('PWNED') === -1, what + ' did not let the filename run a command');
}

// ── execFileSync ───────────────────────────────────────────────────────────

assertClean(cp.execFileSync(g.file, g.args), 'execFileSync');

// Non-zero exit still throws, and the error carries the child's output.
let threw = null;
try {
    cp.execFileSync(g.file, ['no-such-token-anywhere', hostilePath]);
} catch (e) {
    threw = e;
}
assert(threw !== null, 'execFileSync throws on non-zero exit');
assert(typeof threw.status === 'number', 'execFileSync error carries status');

// ── spawnSync ──────────────────────────────────────────────────────────────

const sres = cp.spawnSync(g.file, g.args);
assertEqual(sres.status, 0, 'spawnSync found the metacharacter path');
assertClean(sres.stdout, 'spawnSync');

// ── spawn (async, default no shell) ────────────────────────────────────────

const sp = cp.spawn(g.file, g.args, { stdio: 'pipe', encoding: 'utf8' });
let spOut = '';
sp.stdout.on('data', function (c) { spOut += c; });
sp.on('close', function (code) {
    assertEqual(code, 0, 'spawn exited 0 on the metacharacter path');
    assertClean(spOut, 'spawn');
});

// ── execFile (async) ───────────────────────────────────────────────────────

let efFiredSync = true;
cp.execFile(g.file, g.args, function (err, stdout) {
    assertEqual(err, null, 'execFile no error');
    assertClean(stdout, 'execFile');
});
efFiredSync = false;   // runs before any callback can, if execFile is async

cp.execFile(g.file, g.args).then(function (r) {
    assertClean(r.stdout, 'execFile promise');
});

// ── exec keeps shell semantics ─────────────────────────────────────────────
//
// exec takes a command STRING, so `&` must still separate commands — that is
// the whole reason the two APIs are different.

cp.exec(isWin ? 'echo one& echo two' : 'echo one; echo two', function (err, stdout) {
    assertEqual(err, null, 'exec ran the shell line');
    assert(stdout.indexOf('one') !== -1 && stdout.indexOf('two') !== -1,
           'exec still separates commands on the shell operator');
});

// ── shell:true opts back in ────────────────────────────────────────────────

const shelled = cp.spawnSync(isWin ? 'echo shell_opt_in' : 'echo shell_opt_in', [],
                             { shell: true });
assert(shelled.stdout.indexOf('shell_opt_in') !== -1,
       'spawnSync shell:true runs a shell builtin');

const shellSpawn = cp.spawn(isWin ? 'echo spawn_shell_opt_in' : 'echo spawn_shell_opt_in', [],
                            { stdio: 'pipe', encoding: 'utf8', shell: true });
let shellOut = '';
shellSpawn.stdout.on('data', function (c) { shellOut += c; });
shellSpawn.on('close', function () {
    assert(shellOut.indexOf('spawn_shell_opt_in') !== -1,
           'spawn shell:true runs a shell builtin');
    try { fs.unlinkSync(hostilePath); } catch (e) { /* best effort */ }
});
