// Test: os module
var os = globalThis.__brokit_os;
assert(typeof os === 'object', 'os exists');

// platform
assert(typeof os.platform === 'function', 'os.platform is function');
var plat = os.platform();
assert(typeof plat === 'string', 'platform returns string');
assert(['win32', 'linux', 'darwin'].indexOf(plat) !== -1, 'platform is known: ' + plat);

// type
var t = os.type();
assert(typeof t === 'string', 'type returns string');

// arch
var a = os.arch();
assert(typeof a === 'string', 'arch returns string');
assert(['x64', 'arm64', 'ia32', 'arm'].indexOf(a) !== -1, 'arch is known: ' + a);

// homedir
var home = os.homedir();
assert(typeof home === 'string', 'homedir returns string');
assert(home.length > 0, 'homedir not empty');

// userInfo: Node's shape
assert(typeof os.userInfo === 'function', 'os.userInfo is function');
var ui = os.userInfo();
assert(typeof ui.username === 'string' && ui.username.length > 0, 'userInfo.username: ' + ui.username);
assert(ui.homedir === home || ui.homedir.length > 0, 'userInfo.homedir');
assert(typeof ui.uid === 'number' && typeof ui.gid === 'number', 'userInfo.uid / gid are numbers');
if (plat === 'win32') {
    assert(ui.shell === null, 'userInfo.shell is null on Windows');
    assert(ui.uid === -1, 'userInfo.uid is -1 on Windows');
} else {
    assert(ui.shell === null || (typeof ui.shell === 'string' && ui.shell.charAt(0) === '/'),
           'userInfo.shell is the passwd login shell: ' + ui.shell);
}

// tmpdir
var tmp = os.tmpdir();
assert(typeof tmp === 'string', 'tmpdir returns string');
assert(tmp.length > 0, 'tmpdir not empty');

// hostname
var host = os.hostname();
assert(typeof host === 'string', 'hostname returns string');

// EOL
assert(typeof os.EOL === 'string', 'EOL is string');
assert(os.EOL === '\n' || os.EOL === '\r\n', 'EOL is valid');
