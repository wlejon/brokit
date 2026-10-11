// A file's mtimeMs is the same number from every call. It used to be the
// file time shifted by (system_clock::now() - file_clock::now()), two clock
// reads a moment apart, then cut to the millisecond: a time near a
// millisecond boundary came back 1 ms off now and then, and a scanner that
// compares mtimes saw an untouched file as changed.

var fs = globalThis.__brokit_fs;
// Per process: this test and its _gcstress twin run side by side under ctest -j.
var root = 'brokit_fs_mtime_test_' + process.pid;
fs.rmSync(root, { recursive: true, force: true });
fs.mkdirSync(root);

var N = 400;
var names = [];
for (var i = 0; i < N; i++) {
    var p = root + '/f' + i + '.txt';
    fs.writeFileSync(p, 'x' + i);
    names.push(p);
}

var before = Date.now();
var first = names.map(function(p) { return fs.statSync(p).mtimeMs; });
for (var i = 0; i < N; i++) {
    assert(Number.isInteger(first[i]), 'mtimeMs is whole milliseconds: ' + first[i]);
    assert(Math.abs(first[i] - before) < 120000, 'mtimeMs is a Unix time near now: ' + first[i] + ' vs ' + before);
}
for (var round = 0; round < 25; round++) {
    for (var i = 0; i < N; i++) {
        var again = fs.statSync(names[i]).mtimeMs;
        if (again !== first[i]) assertEqual(again, first[i], 'statSync is stable for ' + names[i]);
    }
}

var asyncTimes = null;
var failed = null;
Promise.all(names.map(function(p) { return fs.promises.stat(p); }))
    .then(function(all) { asyncTimes = all.map(function(s) { return s.mtimeMs; }); },
          function(e) { failed = e; });

globalThis.__test_onDone = function() {
    assert(!failed, 'fs.promises.stat succeeded: ' + (failed && failed.message));
    assert(asyncTimes && asyncTimes.length === N, 'every file was stat-ed');
    var differ = [];
    for (var i = 0; i < N; i++) if (asyncTimes[i] !== first[i]) differ.push(names[i] + ' ' + asyncTimes[i] + ' vs ' + first[i]);
    assertEqual(differ.length, 0, 'fs.promises.stat agrees with statSync: ' + differ.slice(0, 3).join('; '));
    fs.rmSync(root, { recursive: true, force: true });
};
