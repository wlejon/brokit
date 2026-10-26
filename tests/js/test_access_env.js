// process.env under the host's access gate (api.h, "Access"): a variable the
// gate refuses (EnvRead) reads as unset and is not listed; a change it
// refuses (EnvWrite) goes to the program's own view, which process.env shows
// and child processes are given; a refusal the host marks as a revoked grant
// throws an Error named PermissionRevoked.

var cp = require('child_process');
var fs = require('fs');

var isWin = process.platform === 'win32';
var secret = 'BROKIT_ENV_SECRET_' + process.pid;
var mine = 'BROKIT_ENV_MINE_' + process.pid;
var gone = 'BROKIT_ENV_GONE_' + process.pid;

// No policy: the real environment.
process.env[secret] = 's3cret';
process.env[gone] = 'here';
assertEqual(process.env[secret], 's3cret', 'no policy: a set variable reads back');

__test_setAccessPolicy({ envHide: [secret, mine, gone] });

assertEqual(process.env[secret], undefined, 'a refused variable reads as unset');
assertEqual(secret in process.env, false, 'and is not in process.env');
assertEqual(Object.keys(process.env).indexOf(secret), -1, 'and is not listed');
assert(Object.keys(process.env).length > 0, 'the variables the gate shows are still listed');
assertEqual(process.env.BROKIT_ENV_NEVER_SET_ANYWHERE, undefined, 'an unset variable is unset');
var log = __test_accessLog();
assert(log.indexOf('env:read:BROKIT_ENV_NEVER_SET_ANYWHERE') < 0, 'an unset variable is answered without asking');

process.env[mine] = 'mine';
assertEqual(process.env[mine], 'mine', 'a refused change shows in the program\'s own view');
assert(Object.keys(process.env).indexOf(mine) >= 0, 'and is listed');
delete process.env[gone];
assertEqual(process.env[gone], undefined, 'a refused delete hides the variable from the program');

var echo = isWin ? ['cmd', ['/c', 'echo %' + mine + '%']] : ['sh', ['-c', 'echo $' + mine]];
var child = cp.spawnSync(echo[0], echo[1], { encoding: 'utf8' });
assertEqual(String(child.stdout).trim(), 'mine', 'a child process is given the program\'s view');
var echoGone = isWin ? ['cmd', ['/c', 'echo [%' + gone + '%]']] : ['sh', ['-c', 'echo "[$' + gone + ']"']];
child = cp.spawnSync(echoGone[0], echoGone[1], { encoding: 'utf8' });
var goneOut = String(child.stdout).trim();
assert(goneOut === '[]' || goneOut === '[%' + gone + '%]', 'and not what it deleted: ' + goneOut);

// A revoked grant: the refusal names itself.
var root = 'brokit_access_env_' + process.pid;
fs.mkdirSync(root, { recursive: true });
var abs = fs.realpathSync(root);
__test_setAccessPolicy({ fsDenyUnder: [abs], revoked: true });
var err = null;
try { fs.readFileSync(abs + '/x.txt'); } catch (e) { err = e; }
assert(err !== null, 'a revoked read throws');
assertEqual(err && err.name, 'PermissionRevoked', 'named PermissionRevoked');
assertEqual(err && err.message, 'No file access to ' + (err && err.path), 'its message is the refusal without the marker');
assertEqual(err && err.code, 'EACCES', 'with the fs code');

__test_clearAccessPolicy();
assertEqual(process.env[secret], 's3cret', 'cleared: the real variable reads again');
// The program's own view is dropped by writing through to the real one.
process.env[mine] = 'x';
delete process.env[mine];
process.env[gone] = 'x';
delete process.env[gone];
delete process.env[secret];
assertEqual(process.env[mine], undefined, 'cleaned up');
fs.rmSync(root, { recursive: true, force: true });
