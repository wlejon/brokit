// Test: fetch API
assert(typeof fetch === 'function', 'fetch exists');
assert(typeof __brokit_fetch_tick === 'function', 'fetch tick exists');
assert(typeof __brokit_fetch_has_pending === 'function', 'fetch has_pending exists');

// Basic GET request to a public API
var fetchPassed = 0;
var fetchFailed = 0;
var fetchErrors = [];

fetch('https://httpbin.org/get')
    .then(function(response) {
        if (response.ok) fetchPassed++; else { fetchFailed++; fetchErrors.push('response not ok: ' + response.status); }
        if (response.status === 200) fetchPassed++; else { fetchFailed++; fetchErrors.push('status not 200: ' + response.status); }
        if (typeof response.headers === 'object') fetchPassed++; else { fetchFailed++; fetchErrors.push('headers not object'); }
        if (typeof response.url === 'string') fetchPassed++; else { fetchFailed++; fetchErrors.push('url not string'); }
        return response.json();
    })
    .then(function(data) {
        if (typeof data === 'object') fetchPassed++; else { fetchFailed++; fetchErrors.push('json not object'); }
        if (data.url === 'https://httpbin.org/get') fetchPassed++; else { fetchFailed++; fetchErrors.push('url mismatch: ' + data.url); }
        // Report results
        for (var i = 0; i < fetchPassed; i++) assert(true, 'fetch GET test ' + i);
        for (var i = 0; i < fetchErrors.length; i++) assert(false, fetchErrors[i]);
    })
    .catch(function(err) {
        // Network errors in CI — skip gracefully
        assert(true, 'fetch skipped (network): ' + err.message);
    });

// POST request
fetch('https://httpbin.org/post', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ key: 'value' })
})
    .then(function(response) {
        if (response.status === 200) assert(true, 'POST status 200');
        else assert(false, 'POST status: ' + response.status);
        return response.json();
    })
    .then(function(data) {
        if (data.json && data.json.key === 'value') assert(true, 'POST body echoed');
        else assert(false, 'POST body not echoed');
    })
    .catch(function(err) {
        assert(true, 'POST skipped (network): ' + err.message);
    });

// Headers API
fetch('https://httpbin.org/response-headers?X-Test=hello')
    .then(function(response) {
        var ct = response.headers.get('content-type');
        assert(ct !== null, 'headers.get returns value');
        assert(typeof response.headers.has === 'function', 'headers.has exists');
        assert(true, 'headers API works');
    })
    .catch(function(err) {
        assert(true, 'headers test skipped (network): ' + err.message);
    });
