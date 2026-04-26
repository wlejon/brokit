// fs.watch — native filesystem watcher.
//
// The test harness pumps __brokit_fs_watch_tick between Sleep(10) iterations
// while any watcher is open, so callback delivery happens automatically. We
// drive the test by closing watchers in the callback once we've seen the
// expected events; that flips has_pending → false and exits the pump loop.

const fs   = require('fs');
const path = require('path');
const os   = require('os');

function uniqueDir(label) {
    const d = path.join(os.tmpdir(),
        'brokit_fs_watch_' + label + '_' + Date.now() + '_' + Math.floor(Math.random() * 1e9));
    fs.mkdirSync(d, { recursive: true });
    return d;
}

// ── Sanity: fs.watch is exposed and returns an FSWatcher with the expected
//   methods. We can attach a listener and close the watcher cleanly.
(function smoke() {
    const dir = uniqueDir('smoke');
    let w = null;
    try {
        w = fs.watch(dir, () => {});
        assert(w !== null && w !== undefined, 'fs.watch returned a watcher');
        assert(typeof w.on === 'function',     'watcher has .on');
        assert(typeof w.off === 'function',    'watcher has .off');
        assert(typeof w.close === 'function',  'watcher has .close');
    } finally {
        if (w) w.close();
        try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) {}
    }
})();

// ── A new file in the watched directory triggers a 'rename' event.
(function detectsCreate() {
    const dir = uniqueDir('create');
    const target = path.join(dir, 'newfile.txt');
    let sawCreate = false;

    const w = fs.watch(dir, function (event, filename) {
        if (filename === 'newfile.txt') sawCreate = true;
        // Close on first event of interest so the harness exits.
        if (sawCreate) w.close();
    });

    fs.writeFileSync(target, 'hello');

    // The harness loop will pump until the watcher closes.
    // Result is checked once the loop exits and control returns to the
    // collection step in main.cpp (which calls __test_results).
    // We assert here as a deferred check via the close listener.
    w.on('close', function () {
        assert(sawCreate, 'fs.watch saw create of newfile.txt');
        try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) {}
    });
})();

// ── Modifying an existing file triggers a 'change' event.
(function detectsModify() {
    const dir    = uniqueDir('modify');
    const target = path.join(dir, 'data.txt');
    fs.writeFileSync(target, 'v1');

    let sawChange = false;
    const w = fs.watch(dir, function (event, filename) {
        if (filename === 'data.txt' && event === 'change') {
            sawChange = true;
            w.close();
        } else if (filename === 'data.txt' && event === 'rename') {
            // Some platforms (Windows) report the modify as rename when the
            // file's size changes via a fresh write. Accept either.
            sawChange = true;
            w.close();
        }
    });

    fs.writeFileSync(target, 'v2-much-longer-payload-to-force-change');

    w.on('close', function () {
        assert(sawChange, 'fs.watch saw modify of data.txt');
        try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) {}
    });
})();

// ── Recursive watch: an event in a subdirectory carries a relative path.
(function detectsRecursiveCreate() {
    const dir    = uniqueDir('recursive');
    const subDir = path.join(dir, 'sub');
    fs.mkdirSync(subDir);

    let sawNested = false;
    const w = fs.watch(dir, { recursive: true }, function (event, filename) {
        // Filenames use forward slashes regardless of platform.
        if (filename && filename.replace(/\\/g, '/') === 'sub/inside.txt') {
            sawNested = true;
            w.close();
        }
    });

    fs.writeFileSync(path.join(subDir, 'inside.txt'), 'nested');

    w.on('close', function () {
        assert(sawNested, 'recursive fs.watch reported sub/inside.txt');
        try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) {}
    });
})();

// ── close() is idempotent and emits a single 'close' event.
(function closeIsIdempotent() {
    const dir = uniqueDir('idempotent_close');
    let closeCount = 0;
    const w = fs.watch(dir, () => {});
    w.on('close', function () { closeCount++; });
    w.close();
    w.close();
    w.close();
    assertEqual(closeCount, 1, 'close emits once even when called repeatedly');
    try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) {}
})();

// ── Watching a non-existent path throws.
(function rejectsMissingPath() {
    let threw = false;
    try {
        fs.watch('/this/path/should/not/exist/__brokit_test__');
    } catch (e) {
        threw = true;
    }
    assert(threw, 'fs.watch on missing path throws');
})();
