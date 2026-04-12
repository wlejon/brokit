// Test: crypto.subtle API

assert(typeof crypto.subtle === 'object', 'crypto.subtle exists');
assert(typeof crypto.subtle.digest === 'function', 'subtle.digest exists');
assert(typeof crypto.subtle.importKey === 'function', 'subtle.importKey exists');
assert(typeof crypto.subtle.sign === 'function', 'subtle.sign exists');
assert(typeof crypto.subtle.verify === 'function', 'subtle.verify exists');
assert(typeof crypto.subtle.generateKey === 'function', 'subtle.generateKey exists');
assert(typeof crypto.subtle.encrypt === 'function', 'subtle.encrypt exists');
assert(typeof crypto.subtle.decrypt === 'function', 'subtle.decrypt exists');
assert(typeof crypto.subtle.exportKey === 'function', 'subtle.exportKey exists');

// ---- digest ----
async function testDigest() {
    var data = new TextEncoder().encode('hello');
    var hash = await crypto.subtle.digest('SHA-256', data);
    assert(hash instanceof ArrayBuffer, 'digest returns ArrayBuffer');
    assertEqual(hash.byteLength, 32, 'SHA-256 produces 32 bytes');

    var hex = Array.from(new Uint8Array(hash))
        .map(function(b) { return b.toString(16).padStart(2, '0'); }).join('');
    assertEqual(hex, '2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824',
        'SHA-256 of "hello" matches known value');

    var hash384 = await crypto.subtle.digest('SHA-384', new TextEncoder().encode('hello'));
    assertEqual(hash384.byteLength, 48, 'SHA-384 produces 48 bytes');

    var hash512 = await crypto.subtle.digest('SHA-512', new TextEncoder().encode('hello'));
    assertEqual(hash512.byteLength, 64, 'SHA-512 produces 64 bytes');

    // Empty input
    var hashEmpty = await crypto.subtle.digest('SHA-256', new Uint8Array(0));
    assertEqual(hashEmpty.byteLength, 32, 'SHA-256 of empty input is 32 bytes');
}

// ---- importKey + sign + verify (HMAC-SHA256) ----
async function testHMAC() {
    var rawKey = new TextEncoder().encode('my-secret-key');
    var key = await crypto.subtle.importKey(
        'raw', rawKey,
        { name: 'HMAC', hash: 'SHA-256' },
        false, ['sign', 'verify']
    );
    assert(key !== null && key !== undefined, 'importKey returns CryptoKey');
    assertEqual(key.type, 'secret', 'key type is secret');
    assertEqual(key.extractable, false, 'key is not extractable');

    var data = new TextEncoder().encode('message to authenticate');
    var signature = await crypto.subtle.sign({ name: 'HMAC' }, key, data);
    assert(signature instanceof ArrayBuffer, 'sign returns ArrayBuffer');
    assertEqual(signature.byteLength, 32, 'HMAC-SHA256 signature is 32 bytes');

    var valid = await crypto.subtle.verify({ name: 'HMAC' }, key, signature, data);
    assertEqual(valid, true, 'verify returns true for valid signature');

    var badData = new TextEncoder().encode('tampered message');
    var invalid = await crypto.subtle.verify({ name: 'HMAC' }, key, signature, badData);
    assertEqual(invalid, false, 'verify returns false for tampered data');
}

// ---- generateKey ----
async function testGenerateKey() {
    var key = await crypto.subtle.generateKey(
        { name: 'HMAC', hash: 'SHA-256' },
        true, ['sign', 'verify']
    );
    assert(key !== null, 'generateKey returns CryptoKey');
    assertEqual(key.type, 'secret', 'generated key type is secret');
    assertEqual(key.extractable, true, 'generated key is extractable');

    var exported = await crypto.subtle.exportKey('raw', key);
    assert(exported instanceof ArrayBuffer, 'exportKey returns ArrayBuffer');
    assertEqual(exported.byteLength, 32, 'HMAC-SHA256 key is 32 bytes');

    var sig = await crypto.subtle.sign({ name: 'HMAC' }, key, new TextEncoder().encode('test'));
    assertEqual(sig.byteLength, 32, 'can sign with generated key');
}

// ---- AES-GCM encrypt/decrypt ----
async function testAESGCM() {
    var key = await crypto.subtle.generateKey(
        { name: 'AES-GCM', length: 256 },
        false, ['encrypt', 'decrypt']
    );
    assert(key !== null, 'AES-GCM generateKey works');

    var iv = new Uint8Array(12);
    crypto.getRandomValues(iv);

    var plaintext = new TextEncoder().encode('secret data to encrypt');
    var ciphertext = await crypto.subtle.encrypt(
        { name: 'AES-GCM', iv: iv }, key, plaintext
    );
    assert(ciphertext instanceof ArrayBuffer, 'encrypt returns ArrayBuffer');
    assert(ciphertext.byteLength > plaintext.byteLength, 'ciphertext includes tag');

    var decrypted = await crypto.subtle.decrypt(
        { name: 'AES-GCM', iv: iv }, key, ciphertext
    );
    var decryptedText = new TextDecoder().decode(decrypted);
    assertEqual(decryptedText, 'secret data to encrypt', 'decrypt recovers original plaintext');
}

// ---- Raw key export/reimport roundtrip ----
async function testKeyRoundtrip() {
    var rawBytes = new TextEncoder().encode('test-key-data-32-bytes-long!!!!!');
    var key1 = await crypto.subtle.importKey(
        'raw', rawBytes,
        { name: 'HMAC', hash: 'SHA-256' },
        true, ['sign']
    );
    var sig1 = await crypto.subtle.sign({ name: 'HMAC' }, key1, new TextEncoder().encode('x'));

    var exported = await crypto.subtle.exportKey('raw', key1);
    var key2 = await crypto.subtle.importKey(
        'raw', exported,
        { name: 'HMAC', hash: 'SHA-256' },
        false, ['sign']
    );
    var sig2 = await crypto.subtle.sign({ name: 'HMAC' }, key2, new TextEncoder().encode('x'));

    var hex1 = Array.from(new Uint8Array(sig1)).map(function(b) { return b.toString(16).padStart(2, '0'); }).join('');
    var hex2 = Array.from(new Uint8Array(sig2)).map(function(b) { return b.toString(16).padStart(2, '0'); }).join('');
    assertEqual(hex1, hex2, 'raw export/re-import produces same HMAC signature');
}

// Run all async tests
testDigest();
testHMAC();
testGenerateKey();
testAESGCM();
testKeyRoundtrip();
