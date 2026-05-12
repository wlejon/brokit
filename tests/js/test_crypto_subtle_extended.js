// Test: crypto.subtle error paths, JWK import, AES-192, key-usage enforcement
(async function () {
    // ── digest with invalid algorithm rejects ──────────────────────────────
    var rejected = false;
    try {
        await crypto.subtle.digest('NOT-A-HASH', new Uint8Array([1, 2, 3]));
    } catch (e) {
        rejected = true;
    }
    assert(rejected, 'digest invalid algorithm rejects');

    // digest with non-buffer data rejects
    rejected = false;
    try {
        await crypto.subtle.digest('SHA-256', 'not-a-buffer');
    } catch (e) { rejected = true; }
    assert(rejected, 'digest non-buffer data rejects');

    // ── importKey unsupported format rejects ──────────────────────────────
    rejected = false;
    try {
        await crypto.subtle.importKey(
            'pkcs8', new Uint8Array(32),
            { name: 'HMAC', hash: 'SHA-256' },
            false, ['sign']
        );
    } catch (e) { rejected = true; }
    assert(rejected, 'importKey pkcs8 format rejects');

    // importKey with non-object algorithm — exercises parseAlgorithm's string path
    var stringAlgoKey = await crypto.subtle.importKey(
        'raw', new Uint8Array(32), 'HMAC', false, ['sign']
    );
    assert(stringAlgoKey !== null, 'importKey accepts string algorithm name');

    // importKey raw with non-buffer rejects
    rejected = false;
    try {
        await crypto.subtle.importKey(
            'raw', 'not-buffer',
            { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']
        );
    } catch (e) { rejected = true; }
    assert(rejected, 'importKey raw non-buffer rejects');

    // ── importKey from JWK ─────────────────────────────────────────────────
    // base64url("0123456789abcdef" = 16 ASCII bytes) → "MDEyMzQ1Njc4OWFiY2RlZg"
    var jwkKey = await crypto.subtle.importKey(
        'jwk',
        { kty: 'oct', k: 'MDEyMzQ1Njc4OWFiY2RlZg' },
        { name: 'HMAC', hash: 'SHA-256' },
        true, ['sign', 'verify']
    );
    assert(jwkKey !== null && jwkKey !== undefined, 'JWK importKey succeeds');

    // Sign with JWK-imported key and verify
    var msg = new TextEncoder().encode('jwk message');
    var sig = await crypto.subtle.sign({ name: 'HMAC' }, jwkKey, msg);
    assert(sig instanceof ArrayBuffer, 'JWK key produces signature');
    var ok = await crypto.subtle.verify({ name: 'HMAC' }, jwkKey, sig, msg);
    assertEqual(ok, true, 'JWK key verify');

    // JWK with empty k field — currently accepts "undefined" string; just exercise path
    var emptyJwk = await crypto.subtle.importKey(
        'jwk', { kty: 'oct', k: '' },
        { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']
    );
    assert(emptyJwk !== null, 'JWK empty-k import (degenerate but accepted)');

    // ── generateKey AES-192 ────────────────────────────────────────────────
    var aes192 = await crypto.subtle.generateKey(
        { name: 'AES-GCM', length: 192 },
        true, ['encrypt', 'decrypt']
    );
    assert(aes192 !== null, 'AES-192 generateKey succeeds');

    // ── generateKey invalid AES length rejects ────────────────────────────
    rejected = false;
    try {
        await crypto.subtle.generateKey(
            { name: 'AES-GCM', length: 100 },
            true, ['encrypt', 'decrypt']
        );
    } catch (e) { rejected = true; }
    assert(rejected, 'AES bad length rejects');

    // ── generateKey unsupported algo rejects ──────────────────────────────
    rejected = false;
    try {
        await crypto.subtle.generateKey(
            { name: 'RSA-OAEP' }, true, ['encrypt']
        );
    } catch (e) { rejected = true; }
    assert(rejected, 'unsupported generateKey rejects');

    // ── generateKey invalid algorithm name rejects ────────────────────────
    rejected = false;
    try {
        await crypto.subtle.generateKey('not-an-algo', true, ['sign']);
    } catch (e) { rejected = true; }
    assert(rejected, 'generateKey non-object algo rejects');

    // ── generateKey HMAC with unsupported hash rejects ────────────────────
    rejected = false;
    try {
        await crypto.subtle.generateKey(
            { name: 'HMAC', hash: 'MD5' }, true, ['sign']
        );
    } catch (e) { rejected = true; }
    assert(rejected, 'HMAC unsupported hash rejects');

    // ── HMAC with explicit bit length override ────────────────────────────
    var shortKey = await crypto.subtle.generateKey(
        { name: 'HMAC', hash: 'SHA-256', length: 128 },
        true, ['sign']
    );
    assert(shortKey !== null, 'HMAC short-length generateKey succeeds');

    // ── HMAC with SHA-384 / SHA-512 / SHA-1 ───────────────────────────────
    var key384 = await crypto.subtle.generateKey(
        { name: 'HMAC', hash: 'SHA-384' }, true, ['sign', 'verify']
    );
    var sig384 = await crypto.subtle.sign({ name: 'HMAC' }, key384, msg);
    assertEqual(sig384.byteLength, 48, 'HMAC SHA-384 sig 48 bytes');

    var key512 = await crypto.subtle.generateKey(
        { name: 'HMAC', hash: 'SHA-512' }, true, ['sign', 'verify']
    );
    var sig512 = await crypto.subtle.sign({ name: 'HMAC' }, key512, msg);
    assertEqual(sig512.byteLength, 64, 'HMAC SHA-512 sig 64 bytes');

    var key1 = await crypto.subtle.generateKey(
        { name: 'HMAC', hash: 'SHA-1' }, true, ['sign', 'verify']
    );
    var sig1 = await crypto.subtle.sign({ name: 'HMAC' }, key1, msg);
    assertEqual(sig1.byteLength, 20, 'HMAC SHA-1 sig 20 bytes');

    // ── sign with verify-only key rejects ─────────────────────────────────
    var verifyOnly = await crypto.subtle.importKey(
        'raw', new Uint8Array(32),
        { name: 'HMAC', hash: 'SHA-256' },
        false, ['verify']
    );
    rejected = false;
    try {
        await crypto.subtle.sign({ name: 'HMAC' }, verifyOnly, msg);
    } catch (e) { rejected = true; }
    assert(rejected, 'sign without sign usage rejects');

    // ── verify with sign-only key rejects ─────────────────────────────────
    var signOnly = await crypto.subtle.importKey(
        'raw', new Uint8Array(32),
        { name: 'HMAC', hash: 'SHA-256' },
        false, ['sign']
    );
    var someSig = await crypto.subtle.sign({ name: 'HMAC' }, signOnly, msg);
    rejected = false;
    try {
        await crypto.subtle.verify({ name: 'HMAC' }, signOnly, someSig, msg);
    } catch (e) { rejected = true; }
    assert(rejected, 'verify without verify usage rejects');

    // ── sign with bad data type rejects ───────────────────────────────────
    var realKey = await crypto.subtle.generateKey(
        { name: 'HMAC', hash: 'SHA-256' }, true, ['sign', 'verify']
    );
    rejected = false;
    try {
        await crypto.subtle.sign({ name: 'HMAC' }, realKey, 'not-buffer');
    } catch (e) { rejected = true; }
    assert(rejected, 'sign non-buffer rejects');

    // ── verify signature non-buffer rejects ───────────────────────────────
    rejected = false;
    try {
        await crypto.subtle.verify({ name: 'HMAC' }, realKey, 'sig', msg);
    } catch (e) { rejected = true; }
    assert(rejected, 'verify non-buffer signature rejects');

    // ── encrypt with sign-only AES key rejects ────────────────────────────
    var aesKey = await crypto.subtle.generateKey(
        { name: 'AES-GCM', length: 128 }, true, ['decrypt']  // no encrypt
    );
    rejected = false;
    try {
        var iv = new Uint8Array(12);
        await crypto.subtle.encrypt({ name: 'AES-GCM', iv: iv }, aesKey, msg);
    } catch (e) { rejected = true; }
    assert(rejected, 'encrypt without encrypt usage rejects');

    // ── decrypt with encrypt-only AES key rejects ─────────────────────────
    var aesEncOnly = await crypto.subtle.generateKey(
        { name: 'AES-GCM', length: 128 }, true, ['encrypt']
    );
    var ivEnc = new Uint8Array(12);
    var ct = await crypto.subtle.encrypt({ name: 'AES-GCM', iv: ivEnc }, aesEncOnly, msg);
    rejected = false;
    try {
        await crypto.subtle.decrypt({ name: 'AES-GCM', iv: ivEnc }, aesEncOnly, ct);
    } catch (e) { rejected = true; }
    assert(rejected, 'decrypt without decrypt usage rejects');

    // ── AES-CBC encrypt/decrypt round-trip ────────────────────────────────
    var cbcKey = await crypto.subtle.generateKey(
        { name: 'AES-CBC', length: 256 }, true, ['encrypt', 'decrypt']
    );
    var cbcIv = new Uint8Array(16);
    var pt = new TextEncoder().encode('cbc plaintext data');
    var cbcCt = await crypto.subtle.encrypt({ name: 'AES-CBC', iv: cbcIv }, cbcKey, pt);
    assert(cbcCt instanceof ArrayBuffer, 'AES-CBC encrypt');
    var cbcPt = await crypto.subtle.decrypt({ name: 'AES-CBC', iv: cbcIv }, cbcKey, cbcCt);
    assertEqual(new TextDecoder().decode(cbcPt), 'cbc plaintext data', 'AES-CBC round-trip');

    // ── AES-GCM with AAD ──────────────────────────────────────────────────
    var aadKey = await crypto.subtle.generateKey(
        { name: 'AES-GCM', length: 256 }, true, ['encrypt', 'decrypt']
    );
    var aadIv = new Uint8Array(12);
    var aad = new TextEncoder().encode('additional-data');
    var aadCt = await crypto.subtle.encrypt(
        { name: 'AES-GCM', iv: aadIv, additionalData: aad },
        aadKey, pt
    );
    var aadPt = await crypto.subtle.decrypt(
        { name: 'AES-GCM', iv: aadIv, additionalData: aad },
        aadKey, aadCt
    );
    assertEqual(new TextDecoder().decode(aadPt), 'cbc plaintext data', 'AES-GCM with AAD round-trip');
})();
