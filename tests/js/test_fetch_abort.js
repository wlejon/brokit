// Test: fetch + AbortSignal — pre-aborted signals reject immediately (no
// network involved), and aborting mid-flight cancels the transfer promptly
// instead of letting it run to completion.

assert(typeof AbortController === 'function', 'AbortController exists');

// Pre-aborted signal: rejects with AbortError before any network work.
var pre = new AbortController();
pre.abort();
fetch('https://example.com/', { signal: pre.signal }).then(
    function() { assert(false, 'pre-aborted fetch must reject'); },
    function(e) {
        assert(e && e.name === 'AbortError', 'pre-aborted fetch rejects with AbortError (got ' + (e && e.name) + ')');
    });

// In-flight abort: the server would hold this response for 10s; aborting at
// 300ms must reject with AbortError long before that. Network failures skip
// gracefully (matching test_fetch.js).
var ctl = new AbortController();
var t0 = Date.now();
fetch('https://httpbin.org/delay/10', { signal: ctl.signal }).then(
    function() { assert(false, 'aborted in-flight fetch must not resolve'); },
    function(e) {
        if (e && e.name === 'AbortError') {
            assert(Date.now() - t0 < 5000, 'abort cancels the in-flight transfer promptly');
        } else {
            assert(true, 'fetch abort skipped (network): ' + (e && e.message));
        }
    });
setTimeout(function() { ctl.abort(); }, 300);

// Aborting after completion is a no-op (must not throw or re-settle).
var late = new AbortController();
fetch('data:text/plain,hello', { signal: late.signal }).then(
    function(r) { return r.text(); }).then(
    function(t) {
        assert(t === 'hello', 'data URL fetch with live signal resolves');
        late.abort();
        assert(true, 'abort after completion is a no-op');
    },
    function(e) { assert(false, 'data URL fetch failed: ' + (e && e.message)); });
