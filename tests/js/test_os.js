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

// System information, Node's shapes
var cpus = os.cpus();
assert(Array.isArray(cpus) && cpus.length > 0, 'cpus() lists the processors: ' + cpus.length);
assertEqual(cpus.length, os.availableParallelism(), 'one entry per logical processor');
var c0 = cpus[0];
assert(typeof c0.model === 'string', 'cpu model is a string: ' + c0.model);
assert(typeof c0.speed === 'number' && c0.speed >= 0, 'cpu speed in MHz: ' + c0.speed);
['user', 'nice', 'sys', 'idle', 'irq'].forEach(function(k) {
    assert(typeof c0.times[k] === 'number' && c0.times[k] >= 0, 'cpu times.' + k + ': ' + c0.times[k]);
});
assert(c0.times.user + c0.times.sys + c0.times.idle > 0, 'the processor has run');

var total = os.totalmem(), free = os.freemem();
assert(total > 64 * 1024 * 1024, 'totalmem in bytes: ' + total);
assert(free > 0 && free <= total, 'freemem in bytes, under totalmem: ' + free);

var up = os.uptime();
assert(typeof up === 'number' && up > 0, 'uptime in seconds: ' + up);

var load = os.loadavg();
assert(Array.isArray(load) && load.length === 3, 'loadavg is three numbers');
load.forEach(function(v) { assert(typeof v === 'number' && v >= 0, 'load average: ' + v); });
if (plat === 'win32') assertEqual(load.join(','), '0,0,0', 'loadavg is zeros on Windows, as Node');

var ifaces = os.networkInterfaces();
assert(ifaces && typeof ifaces === 'object', 'networkInterfaces is an object');
var sawInternal = false, entries = 0;
Object.keys(ifaces).forEach(function(name) {
    ifaces[name].forEach(function(e) {
        entries++;
        assert(e.family === 'IPv4' || e.family === 'IPv6', name + ' family: ' + e.family);
        assert(typeof e.address === 'string' && e.address.length > 0, name + ' address');
        assert(typeof e.netmask === 'string' && e.netmask.length > 0, name + ' netmask');
        assert(/^([0-9a-f]{2}:){5}[0-9a-f]{2}$/.test(e.mac), name + ' mac: ' + e.mac);
        assert(e.cidr === e.address + '/' + e.cidr.split('/')[1], name + ' cidr: ' + e.cidr);
        assert(typeof e.internal === 'boolean', name + ' internal');
        if (e.family === 'IPv6') assert(typeof e.scopeid === 'number', name + ' scopeid');
        if (e.internal && e.family === 'IPv4') {
            sawInternal = true;
            assertEqual(e.address, '127.0.0.1', 'the loopback address');
            assertEqual(e.netmask, '255.0.0.0', 'the loopback netmask');
        }
    });
});
assert(entries > 0, 'at least one interface is up');
assert(sawInternal, 'the IPv4 loopback is listed and internal');

var rel = os.release();
assert(/^\d+\.\d+/.test(rel), 'release is the kernel version: ' + rel);
var ver = os.version();
assert(typeof ver === 'string' && ver.length > 0, 'version: ' + ver);
if (plat === 'win32') assert(/^Windows/.test(ver), 'version names the Windows product: ' + ver);
var mach = os.machine();
assert(['x86_64', 'arm64', 'aarch64', 'i686', 'arm', 'armv7l'].indexOf(mach) !== -1, 'machine: ' + mach);
assertEqual(os.endianness(), 'LE', 'endianness');
assertEqual(os.constants.signals.SIGTERM, 15, 'os.constants.signals.SIGTERM');
assertEqual(os.constants.signals.SIGKILL, 9, 'os.constants.signals.SIGKILL');
assert(typeof os.devNull === 'string' && os.devNull.length > 0, 'devNull');
