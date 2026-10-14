// Test: fs.utimesSync, fs.lutimesSync, fs.futimesSync, fs.utimes, fs.lutimes, fs.futimes, fs.promises.utimes, fs.promises.lutimes
var fs = globalThis.__brokit_fs;
var tmpBase = globalThis.__brokit_os.tmpdir();
var testDir = tmpBase + '/brokit_fs_utimes_' + Date.now() + '_' + Math.floor(Math.random() * 100000);

fs.mkdirSync(testDir, { recursive: true });

var file = testDir + '/sample.txt';
fs.writeFileSync(file, 'hello utimes');

// ── 1. utimesSync with Unix epoch seconds (including fractional ms) ────────
var atimeSec = 1700000123.456;
var mtimeSec = 1700000789.321;
fs.utimesSync(file, atimeSec, mtimeSec);

var st1 = fs.statSync(file);
assert(Math.abs(st1.atimeMs - 1700000123456) <= 5, 'utimesSync number atimeMs (got ' + st1.atimeMs + ')');
assert(Math.abs(st1.mtimeMs - 1700000789321) <= 5, 'utimesSync number mtimeMs (got ' + st1.mtimeMs + ')');
assert(st1.atime instanceof Date, 'stat.atime is a Date');
assert(st1.mtime instanceof Date, 'stat.mtime is a Date');
assert(Math.abs(st1.atime.getTime() - 1700000123456) <= 5, 'stat.atime Date value matches');
assert(Math.abs(st1.mtime.getTime() - 1700000789321) <= 5, 'stat.mtime Date value matches');

// ── 2. utimesSync with numeric strings ─────────────────────────────────────
fs.utimesSync(file, '1650000100.250', '1650000200.750');
var st2 = fs.statSync(file);
assert(Math.abs(st2.atimeMs - 1650000100250) <= 5, 'utimesSync numeric string atimeMs');
assert(Math.abs(st2.mtimeMs - 1650000200750) <= 5, 'utimesSync numeric string mtimeMs');

// ── 3. utimesSync with Date objects ────────────────────────────────────────
var dAtime = new Date('2023-06-15T10:20:30.500Z');
var dMtime = new Date('2023-06-16T11:22:33.250Z');
fs.utimesSync(file, dAtime, dMtime);
var st3 = fs.statSync(file);
assert(Math.abs(st3.atimeMs - dAtime.getTime()) <= 5, 'utimesSync Date atimeMs');
assert(Math.abs(st3.mtimeMs - dMtime.getTime()) <= 5, 'utimesSync Date mtimeMs');

// ── 4. utimesSync on a directory ───────────────────────────────────────────
var subDir = testDir + '/subdir';
fs.mkdirSync(subDir);
fs.utimesSync(subDir, 1680000000, 1680000500);
var dirSt = fs.statSync(subDir);
assert(Math.abs(dirSt.mtimeMs - 1680000500000) <= 5, 'utimesSync updates directory mtimeMs');

// ── 5. utimesSync error cases (ENOENT and TypeError) ───────────────────────
var enoentThrew = false;
try {
    fs.utimesSync(testDir + '/missing.txt', 1700000000, 1700000000);
} catch (e) {
    enoentThrew = true;
    assertEqual(e.code, 'ENOENT', 'utimesSync missing file throws ENOENT');
    assertEqual(e.syscall, 'utime', 'utimesSync missing file syscall is utime');
}
assert(enoentThrew, 'utimesSync threw on missing file');

var badInputs = [
    [NaN, 1700000000, 'NaN atime'],
    [1700000000, NaN, 'NaN mtime'],
    [Infinity, 1700000000, 'Infinity atime'],
    [1700000000, -Infinity, '-Infinity mtime'],
    ['not-a-number', 1700000000, 'non-numeric string'],
    ['', 1700000000, 'empty string'],
    [null, 1700000000, 'null atime'],
    [undefined, 1700000000, 'undefined atime'],
    [{}, 1700000000, 'object atime'],
    [new Date(NaN), 1700000000, 'Invalid Date atime']
];
for (var i = 0; i < badInputs.length; i++) {
    var item = badInputs[i];
    var threwType = false;
    try {
        fs.utimesSync(file, item[0], item[1]);
    } catch (e) {
        threwType = (e instanceof TypeError) && e.code === 'ERR_INVALID_ARG_TYPE';
    }
    assert(threwType, 'utimesSync rejects invalid timestamp: ' + item[2]);
}

// ── 6. futimesSync on an open file descriptor ──────────────────────────────
var fdFile = testDir + '/futimes.txt';
var fd = fs.openSync(fdFile, 'w+');
fs.writeSync(fd, 'written before futimes');
var fAtime = new Date('2022-01-02T03:04:05.000Z');
var fMtime = new Date('2022-05-06T07:08:09.000Z');
fs.futimesSync(fd, fAtime, fMtime);
var fst = fs.fstatSync(fd);
assert(Math.abs(fst.atimeMs - fAtime.getTime()) <= 5, 'futimesSync fstat atimeMs');
assert(Math.abs(fst.mtimeMs - fMtime.getTime()) <= 5, 'futimesSync fstat mtimeMs');
fs.closeSync(fd);

var fstAfterClose = fs.statSync(fdFile);
assert(Math.abs(fstAfterClose.mtimeMs - fMtime.getTime()) <= 5, 'futimesSync mtime preserved after closeSync');

var ebadfThrew = false;
try {
    fs.futimesSync(fd, 1700000000, 1700000000);
} catch (e) {
    ebadfThrew = (e.code === 'EBADF');
}
assert(ebadfThrew, 'futimesSync on closed fd throws EBADF');

// ── 7. lutimesSync on a symlink ────────────────────────────────────────────
var targetFile = testDir + '/target.txt';
var linkFile = testDir + '/link.txt';
fs.writeFileSync(targetFile, 'target content');
var targetOrigAtime = 1600000000;
var targetOrigMtime = 1600000100;
fs.utimesSync(targetFile, targetOrigAtime, targetOrigMtime);

var symlinkSupported = true;
try {
    fs.symlinkSync(targetFile, linkFile);
} catch (e) {
    // On Windows without Developer Mode / admin privileges, symlink creation can fail with EPERM.
    symlinkSupported = false;
}

if (symlinkSupported) {
    var linkMtime = 1710000900;
    var linkAtime = 1710000800;
    fs.lutimesSync(linkFile, linkAtime, linkMtime);

    var targetSt = fs.statSync(targetFile);
    var linkLst = fs.lstatSync(linkFile);
    assert(linkLst.isSymbolicLink(), 'lstatSync identifies symlink');
    assert(Math.abs(targetSt.mtimeMs - targetOrigMtime * 1000) <= 5,
           'lutimesSync does not alter target file mtimeMs (got ' + targetSt.mtimeMs + ')');
    assert(Math.abs(linkLst.mtimeMs - linkMtime * 1000) <= 5,
           'lutimesSync updates symlink mtimeMs (got ' + linkLst.mtimeMs + ')');
}

// ── 8. Async callback & promises.utimes / promises.lutimes / fs.futimes ────
var asyncFile = testDir + '/async_utimes.txt';
fs.writeFileSync(asyncFile, 'async test');

var cbCalledSync = true;
var cbErr = 'pending';
fs.utimes(asyncFile, 1690000111, 1690000222, function(err) {
    cbErr = err;
});
cbCalledSync = (cbErr !== 'pending');
assert(!cbCalledSync, 'fs.utimes callback is invoked asynchronously');

var asyncDone = false;
var asyncError = null;

(async function() {
    try {
        // Wait for callback test file or test promises.utimes on a separate file
        var pFile = testDir + '/p_utimes.txt';
        fs.writeFileSync(pFile, 'promises utimes');

        var pDateA = new Date('2024-07-08T09:10:11.600Z');
        var pDateM = new Date('2024-07-09T10:11:12.400Z');
        await fs.promises.utimes(pFile, pDateA, pDateM);

        var pst = await fs.promises.stat(pFile);
        assert(Math.abs(pst.atimeMs - pDateA.getTime()) <= 5, 'fs.promises.utimes Date atimeMs');
        assert(Math.abs(pst.mtimeMs - pDateM.getTime()) <= 5, 'fs.promises.utimes Date mtimeMs');

        // Numeric strings with fs.promises.utimes
        await fs.promises.utimes(pFile, '1660000300.5', '1660000400.5');
        var pst2 = await fs.promises.stat(pFile);
        assert(Math.abs(pst2.atimeMs - 1660000300500) <= 5, 'fs.promises.utimes string atimeMs');
        assert(Math.abs(pst2.mtimeMs - 1660000400500) <= 5, 'fs.promises.utimes string mtimeMs');

        // fs.futimes (Promise / callback)
        var afd = fs.openSync(pFile, 'r+');
        await fs.futimes(afd, 1670000100, 1670000200);
        var afst = await fs.fstat(afd);
        assert(Math.abs(afst.mtimeMs - 1670000200000) <= 5, 'fs.futimes updates mtimeMs');
        fs.closeSync(afd);

        // fs.promises.lutimes on symlink
        if (symlinkSupported) {
            await fs.promises.lutimes(linkFile, 1720000100, 1720000200);
            var llst = await fs.promises.lstat(linkFile);
            var tst = await fs.promises.stat(targetFile);
            assert(Math.abs(llst.mtimeMs - 1720000200000) <= 5, 'fs.promises.lutimes updates symlink mtimeMs');
            assert(Math.abs(tst.mtimeMs - targetOrigMtime * 1000) <= 5, 'fs.promises.lutimes leaves target mtimeMs untouched');
        }

        // Error cases in fs.promises.utimes
        var missingRejected = false;
        try {
            await fs.promises.utimes(testDir + '/no_such_file.txt', 1700000000, 1700000000);
        } catch (e) {
            missingRejected = (e.code === 'ENOENT');
        }
        assert(missingRejected, 'fs.promises.utimes rejects ENOENT on missing path');

        var nanRejected = false;
        try {
            await fs.promises.utimes(pFile, NaN, 1700000000);
        } catch (e) {
            nanRejected = (e instanceof TypeError) && e.code === 'ERR_INVALID_ARG_TYPE';
        }
        assert(nanRejected, 'fs.promises.utimes rejects NaN with TypeError');

        asyncDone = true;
    } catch (err) {
        asyncError = err;
    }
})();

globalThis.__test_onDone = function() {
    if (asyncError) {
        assert(false, 'async utimes test failed: ' + (asyncError.stack || asyncError.message || asyncError));
    }
    assert(asyncDone, 'async utimes block completed');
    assertEqual(cbErr, null, 'fs.utimes callback succeeded');
    var cbSt = fs.statSync(asyncFile);
    assert(Math.abs(cbSt.mtimeMs - 1690000222000) <= 5, 'fs.utimes callback updated mtimeMs');

    fs.rmSync(testDir, { recursive: true, force: true });
};
