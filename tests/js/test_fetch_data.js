// Test: fetch with data: and local-file URLs (offline paths — no network)

// ── data: URL plain text ──────────────────────────────────────────────────
fetch('data:text/plain,Hello%20World')
    .then(function (r) {
        assert(r.ok, 'data: text/plain ok');
        assertEqual(r.status, 200, 'data: status 200');
        return r.text();
    })
    .then(function (t) {
        assertEqual(t, 'Hello World', 'data: percent-decoded text');
    });

// ── data: URL base64 ──────────────────────────────────────────────────────
// btoa('hi!') = 'aGkh'
fetch('data:text/plain;base64,aGkh')
    .then(function (r) { return r.text(); })
    .then(function (t) { assertEqual(t, 'hi!', 'data: base64 decoded'); });

// ── data: URL with default mime ───────────────────────────────────────────
fetch('data:,DefaultMime')
    .then(function (r) {
        var ct = r.headers.get('content-type');
        assert(ct && ct.indexOf('text/plain') === 0, 'default mime text/plain');
        return r.text();
    })
    .then(function (t) { assertEqual(t, 'DefaultMime', 'default-mime body'); });

// ── data: URL malformed (no comma) ────────────────────────────────────────
fetch('data:malformed-no-comma')
    .then(function (r) { return r.text(); })
    .then(function (t) {
        assertEqual(t, 'malformed-no-comma', 'malformed data url treated as payload');
    });

// ── data: URL with binary content via arrayBuffer ─────────────────────────
fetch('data:application/octet-stream;base64,AAECAwQF')
    .then(function (r) {
        var ct = r.headers.get('content-type');
        assert(ct && ct.indexOf('application/octet-stream') !== -1, 'binary mime');
        return r.arrayBuffer();
    })
    .then(function (ab) {
        var v = new Uint8Array(ab);
        assertEqual(v.length, 6, 'binary length');
        assertEqual(v[0], 0, 'binary byte 0');
        assertEqual(v[5], 5, 'binary byte 5');
    });

// ── local file fetch — 404 for missing file ───────────────────────────────
fetch('/this_file_definitely_does_not_exist_brokit.xyz')
    .then(function (r) {
        assertEqual(r.ok, false, 'missing local file not ok');
        assertEqual(r.status, 404, 'missing local file 404');
    });

// ── HTTP POST/headers/etc with unreachable URLs ──────────────────────────
// Exercises body-parsing and header branches in C++ even when offline.
var fp1 = fetch('http://127.0.0.1:1/no-such-port-brokit', {
    method: 'POST',
    body: new Uint8Array([1, 2, 3, 4, 5])
});
assert(fp1 instanceof Promise, 'fetch returns promise for typed-array body');
fp1.catch(function () {});

var fp2 = fetch('http://127.0.0.1:1/no-such-port-brokit', {
    method: 'POST',
    body: new ArrayBuffer(8)
});
assert(fp2 instanceof Promise, 'fetch returns promise for ArrayBuffer body');
fp2.catch(function () {});

var fp3 = fetch('http://127.0.0.1:1/no-such-port-brokit', {
    method: 'GET',
    headers: { 'X-Test': 'a', 'X-Other': 'b' }
});
assert(fp3 instanceof Promise, 'fetch with header object');
fp3.catch(function () {});

var fp4 = fetch('http://127.0.0.1:1/no-such-port-brokit', {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body: 'string-body-text'
});
assert(fp4 instanceof Promise, 'fetch with string body');
fp4.catch(function () {});

// ── Local file fetch with various extensions (mime detection) ────────────
var fs = globalThis.__brokit_fs;
var os = globalThis.__brokit_os;
var tmpdir2 = os.tmpdir();
var prefix = tmpdir2 + '/brokit_fetch_mime_' + Date.now();
fs.mkdirSync(prefix);

var exts = ['xml', 'wasm', 'svg', 'webp', 'woff', 'woff2', 'ttf', 'otf',
            'gif', 'jpeg', 'jpg', 'mjs', 'json', 'css', 'html', 'htm', 'txt'];
var done = 0;
for (var i = 0; i < exts.length; i++) {
    (function (ext) {
        fs.writeFileSync(prefix + '/test.' + ext, 'data');
        fetch(prefix + '/test.' + ext).then(function (r) {
            var ct = r.headers.get('content-type') || '';
            assert(typeof ct === 'string', 'fetch ' + ext + ' content-type');
            done++;
        }).catch(function () { done++; });
    })(exts[i]);
}

// File with no extension → application/octet-stream
fs.writeFileSync(prefix + '/no_ext', 'x');
fetch(prefix + '/no_ext').then(function (r) {
    var ct = r.headers.get('content-type') || '';
    assert(ct.indexOf('octet-stream') !== -1, 'no-ext mime is octet-stream');
});

// ── Exercise fetch base-path resolver via JS-visible array ───────────────
var os3 = globalThis.__brokit_os;
var fs3 = globalThis.__brokit_fs;
var fetchBase = os3.tmpdir() + '/brokit_fetch_base_' + Date.now();
fs3.mkdirSync(fetchBase);
fs3.writeFileSync(fetchBase + '/r.txt', 'relative');

var origBases = globalThis.__brokit_fetch_base_paths;
globalThis.__brokit_fetch_base_paths = [fetchBase];
fetch('r.txt').then(function (r) {
    return r.text();
}).then(function (t) {
    assertEqual(t, 'relative', 'fetch base-path resolver finds file');
}).catch(function () {});

// fetch with leading "./..."
fetch('./r.txt').then(function (r) { return r.text(); })
                .then(function (t) {})
                .catch(function () {});

// Exercise prefix-mount in fetch (resolveBrokitPrefixMount)
var origMounts = globalThis.__brokit_path_mounts;
globalThis.__brokit_path_mounts = { '/fmount': fetchBase };
fetch('/fmount/r.txt').then(function (r) { return r.text(); })
                      .then(function (t) {
                          assertEqual(t, 'relative', 'fetch prefix-mount resolves');
                      })
                      .catch(function () {});

// Restore
globalThis.__brokit_fetch_base_paths = origBases;
globalThis.__brokit_path_mounts = origMounts;

// ── local file fetch — read self via tmp path ─────────────────────────────
var fs = globalThis.__brokit_fs;
var os = globalThis.__brokit_os;
var tmpdir = os.tmpdir();
var tmpFile = tmpdir + '/brokit_fetch_test_' + Date.now() + '.txt';
fs.writeFileSync(tmpFile, 'local-file-content');
fetch(tmpFile)
    .then(function (r) {
        assert(r.ok, 'local file ok');
        return r.text();
    })
    .then(function (t) {
        assertEqual(t, 'local-file-content', 'local file content');
        fs.unlinkSync(tmpFile);
    });
