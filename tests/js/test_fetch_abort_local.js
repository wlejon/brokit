// Test: fetch + AbortSignal on the schemes that never touch the network —
// local files, data: and blob: URLs. The signal is read before any I/O, so an
// already-aborted signal rejects for every scheme, and an abort in the same
// turn as the call beats the read, which settles on the next tick.

var fs = globalThis.__brokit_fs;
var os = globalThis.__brokit_os;
var tmpFile = os.tmpdir() + '/brokit_fetch_abort_local_' + Date.now() + '.txt';
fs.writeFileSync(tmpFile, 'ok');

var blobUrl = URL.createObjectURL(new Blob(['ok'], { type: 'text/plain' }));

var got = {};
function record(name, value) { got[name] = value; }

// The control: a live signal that never aborts must not disturb the fetch.
fetch(tmpFile, { signal: new AbortController().signal })
    .then(function (r) { return r.text(); })
    .then(function (t) { record('file.control', t); });

// Already aborted before the call: rejects for every scheme.
fetch(tmpFile, { signal: AbortSignal.abort() })
    .then(function () { record('file.pre', 'RESOLVED'); },
          function (e) { record('file.pre', e.name); });
fetch('data:text/plain,ok', { signal: AbortSignal.abort() })
    .then(function () { record('data.pre', 'RESOLVED'); },
          function (e) { record('data.pre', e.name); });
fetch(blobUrl, { signal: AbortSignal.abort() })
    .then(function () { record('blob.pre', 'RESOLVED'); },
          function (e) { record('blob.pre', e.name); });
fetch('/this_file_definitely_does_not_exist_brokit.xyz', { signal: AbortSignal.abort() })
    .then(function () { record('missing.pre', 'RESOLVED'); },
          function (e) { record('missing.pre', e.name); });

// Aborted after the call, in the same turn: the read has not settled yet.
var c1 = new AbortController();
fetch(tmpFile, { signal: c1.signal })
    .then(function () { record('file.late', 'RESOLVED'); },
          function (e) { record('file.late', e.name); });
c1.abort();

var c2 = new AbortController();
fetch('data:text/plain,ok', { signal: c2.signal })
    .then(function () { record('data.late', 'RESOLVED'); },
          function (e) { record('data.late', e.name); });
c2.abort();

var c3 = new AbortController();
fetch(blobUrl, { signal: c3.signal })
    .then(function () { record('blob.late', 'RESOLVED'); },
          function (e) { record('blob.late', e.name); });
c3.abort();

// Aborting a signal whose fetch has already settled changes nothing.
var c4 = new AbortController();
fetch(tmpFile, { signal: c4.signal })
    .then(function (r) { return r.text(); })
    .then(function (t) {
        record('file.settled', t);
        c4.abort();
        record('file.abortAfterSettle', 'harmless');
    }, function (e) { record('file.settled', 'REJECTED:' + e.name); });

// No signal at all still works.
fetch(tmpFile).then(function (r) { return r.text(); })
              .then(function (t) { record('file.noSignal', t); });

// The rejection is an AbortError, and the abort event that caused it carried
// the signal as its target.
var c5 = new AbortController();
var c5Target = null;
c5.signal.addEventListener('abort', function (e) { c5Target = e.target; });
fetch(tmpFile, { signal: c5.signal })
    .then(function () { record('file.reason', 'RESOLVED'); },
          function (e) { record('file.reason', e.name + ':' + (e.message.indexOf('aborted') !== -1)); });
c5.abort();
assert(c5Target === c5.signal, 'abort event target is the signal');

// Everything above settles on the fetch tick the harness pumps; the checks
// run from a fetch that is queued after all of them and so settles last.
fetch('data:text/plain,done').then(function () {
    return Promise.resolve();
}).then(function () {
    assertEqual(got['file.control'], 'ok', 'live signal leaves a file fetch alone');
    assertEqual(got['file.pre'], 'AbortError', 'pre-aborted file fetch rejects');
    assertEqual(got['data.pre'], 'AbortError', 'pre-aborted data: fetch rejects');
    assertEqual(got['blob.pre'], 'AbortError', 'pre-aborted blob: fetch rejects');
    assertEqual(got['missing.pre'], 'AbortError', 'pre-aborted missing-file fetch rejects before the 404');
    assertEqual(got['file.late'], 'AbortError', 'same-turn abort rejects a file fetch');
    assertEqual(got['data.late'], 'AbortError', 'same-turn abort rejects a data: fetch');
    assertEqual(got['blob.late'], 'AbortError', 'same-turn abort rejects a blob: fetch');
    assertEqual(got['file.settled'], 'ok', 'settled fetch keeps its value');
    assertEqual(got['file.abortAfterSettle'], 'harmless', 'abort after settle is harmless');
    assertEqual(got['file.noSignal'], 'ok', 'fetch without a signal works');
    assertEqual(got['file.reason'], 'AbortError:true', 'rejection is an AbortError with the abort message');
    URL.revokeObjectURL(blobUrl);
    fs.unlinkSync(tmpFile);
});
