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
