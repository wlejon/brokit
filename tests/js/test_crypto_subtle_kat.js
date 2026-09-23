// Test: crypto.subtle known-answer vectors and the WebCrypto object model
// (CryptoKey / SubtleCrypto interfaces, key.algorithm / key.usages, JWK,
// AES-CBC / AES-CTR / AES-KW, PBKDF2 / HKDF, wrapKey / unwrapKey, error names).

function hex(buf) {
    return Array.from(new Uint8Array(buf))
        .map(function(b) { return b.toString(16).padStart(2, '0'); }).join('');
}
function unhex(s) {
    var out = new Uint8Array(s.length / 2);
    for (var i = 0; i < out.length; i++) out[i] = parseInt(s.substr(i * 2, 2), 16);
    return out;
}
var enc = new TextEncoder();

async function rejectsWith(p, name, label) {
    try {
        await p;
        assert(false, label + ' (resolved)');
    } catch (e) {
        assertEqual(e && e.name, name, label);
    }
}

async function testInterfaces() {
    assert(typeof CryptoKey === 'function', 'CryptoKey is a global interface');
    assert(typeof SubtleCrypto === 'function', 'SubtleCrypto is a global interface');
    assert(crypto.subtle instanceof SubtleCrypto, 'crypto.subtle instanceof SubtleCrypto');
    var threw = false;
    try { new CryptoKey(); } catch (e) { threw = true; }
    assert(threw, 'CryptoKey is not constructible');

    var key = await crypto.subtle.generateKey({ name: 'hmac', hash: { name: 'sha-384' } }, true, ['verify', 'sign']);
    assert(key instanceof CryptoKey, 'generated key instanceof CryptoKey');
    assertEqual(key.type, 'secret', 'key.type');
    assertEqual(key.algorithm.name, 'HMAC', 'algorithm name normalized');
    assertEqual(key.algorithm.hash.name, 'SHA-384', 'algorithm hash normalized');
    assertEqual(key.algorithm.length, 1024, 'HMAC default length is the block size');
    assertEqual(key.usages.join(','), 'sign,verify', 'usages in canonical order');

    var aes = await crypto.subtle.generateKey({ name: 'AES-CBC', length: 192 }, false, ['decrypt', 'encrypt']);
    assertEqual(aes.algorithm.name, 'AES-CBC', 'AES algorithm name');
    assertEqual(aes.algorithm.length, 192, 'AES algorithm length');
    assertEqual(aes.extractable, false, 'non-extractable');
    await rejectsWith(crypto.subtle.exportKey('raw', aes), 'InvalidAccessError', 'export of non-extractable key');

    // A host handle that is not a key is refused, not reinterpreted.
    var threwType = false;
    try { await crypto.subtle.sign('HMAC', new Blob(['x']), enc.encode('x')); }
    catch (e) { threwType = e instanceof TypeError; }
    assert(threwType, 'a Blob is not a CryptoKey');
}

async function testDigestVectors() {
    assertEqual(hex(await crypto.subtle.digest('SHA-1', enc.encode('abc'))),
        'a9993e364706816aba3e25717850c26c9cd0d89d', 'SHA-1("abc")');
    assertEqual(hex(await crypto.subtle.digest({ name: 'sha-256' }, enc.encode('abc'))),
        'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad', 'SHA-256("abc"), lower-case name');
    var dv = new DataView(enc.encode('xxabcxx').buffer, 2, 3);
    assertEqual(hex(await crypto.subtle.digest('SHA-256', dv)),
        'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad', 'digest of a DataView window');
    await rejectsWith(crypto.subtle.digest('MD5', enc.encode('x')), 'NotSupportedError', 'unknown digest');
}

async function testHmacVector() {
    // RFC 4231 test case 2.
    var key = await crypto.subtle.importKey('raw', enc.encode('Jefe'),
        { name: 'HMAC', hash: 'SHA-256' }, false, ['sign', 'verify']);
    var mac = await crypto.subtle.sign('HMAC', key, enc.encode('what do ya want for nothing?'));
    assertEqual(hex(mac), '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843', 'RFC 4231 #2');
    assertEqual(await crypto.subtle.verify('HMAC', key, mac, enc.encode('what do ya want for nothing?')), true,
        'verify RFC 4231 #2');
    await rejectsWith(crypto.subtle.sign('AES-GCM', key, enc.encode('x')), 'InvalidAccessError',
        'sign with mismatched algorithm');
}

async function testJwk() {
    var key = await crypto.subtle.importKey('raw', unhex('000102030405060708090a0b0c0d0e0f'),
        { name: 'AES-GCM' }, true, ['encrypt', 'decrypt']);
    var jwk = await crypto.subtle.exportKey('jwk', key);
    assertEqual(jwk.kty, 'oct', 'jwk.kty');
    assertEqual(jwk.k, 'AAECAwQFBgcICQoLDA0ODw', 'jwk.k is unpadded base64url');
    assertEqual(jwk.alg, 'A128GCM', 'jwk.alg');
    assertEqual(jwk.ext, true, 'jwk.ext');
    assertEqual(jwk.key_ops.join(','), 'encrypt,decrypt', 'jwk.key_ops');

    var back = await crypto.subtle.importKey('jwk', jwk, 'AES-GCM', true, ['encrypt']);
    assertEqual(hex(await crypto.subtle.exportKey('raw', back)), '000102030405060708090a0b0c0d0e0f', 'jwk round trip');

    var hk = await crypto.subtle.importKey('raw', enc.encode('Jefe'), { name: 'HMAC', hash: 'SHA-512' }, true, ['sign']);
    assertEqual((await crypto.subtle.exportKey('jwk', hk)).alg, 'HS512', 'HMAC jwk alg');

    await rejectsWith(crypto.subtle.importKey('jwk', { kty: 'oct', k: jwk.k, alg: 'A256GCM' }, 'AES-GCM', true, ['encrypt']),
        'DataError', 'jwk alg mismatch');
    await rejectsWith(crypto.subtle.importKey('jwk', { kty: 'RSA', k: jwk.k }, 'AES-GCM', true, ['encrypt']),
        'DataError', 'jwk wrong kty');
    await rejectsWith(crypto.subtle.importKey('jwk', { kty: 'oct', k: jwk.k, ext: false }, 'AES-GCM', true, ['encrypt']),
        'DataError', 'jwk ext:false with extractable');
    await rejectsWith(crypto.subtle.importKey('raw', new Uint8Array(20), 'AES-GCM', true, ['encrypt']),
        'DataError', 'AES key of a bad length');
}

async function testAesCbc() {
    // NIST SP 800-38A F.2.1 (first block), then PKCS#7 padding adds a block.
    var key = await crypto.subtle.importKey('raw', unhex('2b7e151628aed2a6abf7158809cf4f3c'), 'AES-CBC', false,
        ['encrypt', 'decrypt']);
    var iv = unhex('000102030405060708090a0b0c0d0e0f');
    var ct = await crypto.subtle.encrypt({ name: 'AES-CBC', iv: iv }, key, unhex('6bc1bee22e409f96e93d7e117393172a'));
    assertEqual(ct.byteLength, 32, 'CBC output is padded to two blocks');
    assertEqual(hex(ct).slice(0, 32), '7649abac8119b246cee98e9b12e9197d', 'SP 800-38A CBC block 1');
    var pt = await crypto.subtle.decrypt({ name: 'AES-CBC', iv: iv }, key, ct);
    assertEqual(hex(pt), '6bc1bee22e409f96e93d7e117393172a', 'CBC round trip');

    var empty = await crypto.subtle.encrypt({ name: 'AES-CBC', iv: iv }, key, new Uint8Array(0));
    assertEqual(empty.byteLength, 16, 'CBC of empty input is one padding block');
    assertEqual((await crypto.subtle.decrypt({ name: 'AES-CBC', iv: iv }, key, empty)).byteLength, 0, 'CBC empty round trip');

    await rejectsWith(crypto.subtle.encrypt({ name: 'AES-CBC', iv: new Uint8Array(12) }, key, pt),
        'OperationError', 'CBC iv must be 16 bytes');
    await rejectsWith(crypto.subtle.decrypt({ name: 'AES-CBC', iv: iv }, key, new Uint8Array(15)),
        'OperationError', 'CBC ciphertext must be whole blocks');
}

async function testAesCtr() {
    // NIST SP 800-38A F.5.1, two blocks (the counter's low byte wraps 0xff -> 0x00 and carries).
    var key = await crypto.subtle.importKey('raw', unhex('2b7e151628aed2a6abf7158809cf4f3c'), 'AES-CTR', false,
        ['encrypt', 'decrypt']);
    var counter = unhex('f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff');
    var pt = unhex('6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51');
    var ct = await crypto.subtle.encrypt({ name: 'AES-CTR', counter: counter, length: 64 }, key, pt);
    assertEqual(hex(ct), '874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff', 'SP 800-38A CTR');
    var back = await crypto.subtle.decrypt({ name: 'AES-CTR', counter: counter, length: 64 }, key, ct);
    assertEqual(hex(back), hex(pt), 'CTR round trip');
    var partial = await crypto.subtle.encrypt({ name: 'AES-CTR', counter: counter, length: 64 }, key, pt.subarray(0, 5));
    assertEqual(hex(partial), '874d6191b6', 'CTR on a partial block');
    await rejectsWith(crypto.subtle.encrypt({ name: 'AES-CTR', counter: counter, length: 1 }, key, new Uint8Array(48)),
        'OperationError', 'CTR counter space exhausted');
}

async function testAesKw() {
    // RFC 3394 4.1: 128-bit key data under a 128-bit KEK.
    var kek = await crypto.subtle.importKey('raw', unhex('000102030405060708090a0b0c0d0e0f'), 'AES-KW', false,
        ['wrapKey', 'unwrapKey']);
    var inner = await crypto.subtle.importKey('raw', unhex('00112233445566778899aabbccddeeff'), 'AES-GCM', true,
        ['encrypt', 'decrypt']);
    var wrapped = await crypto.subtle.wrapKey('raw', inner, kek, 'AES-KW');
    assertEqual(hex(wrapped), '1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5', 'RFC 3394 4.1');
    var unwrapped = await crypto.subtle.unwrapKey('raw', wrapped, kek, 'AES-KW', 'AES-GCM', true, ['encrypt']);
    assertEqual(hex(await crypto.subtle.exportKey('raw', unwrapped)), '00112233445566778899aabbccddeeff', 'KW unwrap');
    var bad = new Uint8Array(wrapped);
    bad[3] ^= 1;
    await rejectsWith(crypto.subtle.unwrapKey('raw', bad, kek, 'AES-KW', 'AES-GCM', true, ['encrypt']),
        'OperationError', 'KW integrity check');
    await rejectsWith(crypto.subtle.encrypt('AES-KW', kek, new Uint8Array(16)), 'NotSupportedError', 'KW is wrap-only');
}

async function testPbkdf2() {
    // RFC 7914 section 11 / widely published PBKDF2-HMAC-SHA256 vectors.
    var base = await crypto.subtle.importKey('raw', enc.encode('password'), 'PBKDF2', false, ['deriveBits', 'deriveKey']);
    var one = await crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt: enc.encode('salt'), iterations: 1 }, base, 256);
    assertEqual(hex(one), '120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b', 'PBKDF2 c=1');
    var two = await crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt: enc.encode('salt'), iterations: 2 }, base, 256);
    assertEqual(hex(two), 'ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43', 'PBKDF2 c=2');

    var aes = await crypto.subtle.deriveKey({ name: 'PBKDF2', hash: 'SHA-256', salt: enc.encode('salt'), iterations: 1 },
        base, { name: 'AES-GCM', length: 128 }, true, ['encrypt']);
    assertEqual(aes.algorithm.length, 128, 'deriveKey AES length');
    assertEqual(hex(await crypto.subtle.exportKey('raw', aes)), '120fb6cffcf8b32c43e7225256c4f837', 'deriveKey = leading derived bits');

    await rejectsWith(crypto.subtle.importKey('raw', enc.encode('pw'), 'PBKDF2', true, ['deriveBits']),
        'SyntaxError', 'PBKDF2 keys are never extractable');
    await rejectsWith(crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt: enc.encode('s'), iterations: 0 }, base, 256),
        'OperationError', 'PBKDF2 zero iterations');
    await rejectsWith(crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt: enc.encode('s'), iterations: 1 }, base, 12),
        'OperationError', 'deriveBits length not a multiple of 8');
}

async function testHkdf() {
    // RFC 5869 A.1.
    var ikm = new Uint8Array(22).fill(0x0b);
    var base = await crypto.subtle.importKey('raw', ikm, 'HKDF', false, ['deriveBits']);
    var okm = await crypto.subtle.deriveBits({
        name: 'HKDF', hash: 'SHA-256',
        salt: unhex('000102030405060708090a0b0c'), info: unhex('f0f1f2f3f4f5f6f7f8f9'),
    }, base, 42 * 8);
    assertEqual(hex(okm), '3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865', 'RFC 5869 A.1');
    await rejectsWith(crypto.subtle.deriveKey({ name: 'HKDF', hash: 'SHA-256', salt: new Uint8Array(0), info: new Uint8Array(0) },
        base, { name: 'AES-GCM', length: 128 }, false, ['encrypt']), 'InvalidAccessError', 'deriveKey needs deriveKey usage');
}

async function testWrapJwk() {
    var wrapper = await crypto.subtle.generateKey({ name: 'AES-GCM', length: 256 }, false, ['wrapKey', 'unwrapKey']);
    var hk = await crypto.subtle.importKey('raw', enc.encode('Jefe'), { name: 'HMAC', hash: 'SHA-256' }, true, ['sign']);
    var iv = crypto.getRandomValues(new Uint8Array(12));
    var wrapped = await crypto.subtle.wrapKey('jwk', hk, wrapper, { name: 'AES-GCM', iv: iv });
    var back = await crypto.subtle.unwrapKey('jwk', wrapped, wrapper, { name: 'AES-GCM', iv: iv },
        { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
    assertEqual(back.extractable, false, 'unwrapped key extractable flag');
    var mac = await crypto.subtle.sign('HMAC', back, enc.encode('what do ya want for nothing?'));
    assertEqual(hex(mac), '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843', 'jwk wrap/unwrap preserves the key');
    var enc2 = await crypto.subtle.generateKey({ name: 'AES-GCM', length: 128 }, false, ['encrypt']);
    await rejectsWith(crypto.subtle.wrapKey('raw', hk, enc2, { name: 'AES-GCM', iv: iv }),
        'InvalidAccessError', 'wrapKey needs wrapKey usage');
}

async function testGcmTagLength() {
    var key = await crypto.subtle.generateKey({ name: 'AES-GCM', length: 128 }, false, ['encrypt', 'decrypt']);
    var iv = new Uint8Array(12);
    await rejectsWith(crypto.subtle.encrypt({ name: 'AES-GCM', iv: iv, tagLength: 100 }, key, new Uint8Array(4)),
        'OperationError', 'invalid tagLength');
    var ct = await crypto.subtle.encrypt({ name: 'AES-GCM', iv: iv, tagLength: 96 }, key, new Uint8Array(4));
    assertEqual(ct.byteLength, 4 + 12, '96-bit tag');
    var pt = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: iv, tagLength: 96 }, key, ct);
    assertEqual(pt.byteLength, 4, '96-bit tag round trip');

    // Every spec tag length round-trips, and a short tag still authenticates.
    var msg = enc.encode('short tags authenticate too');
    var tags = [32, 64, 96, 104, 112, 120, 128];
    for (var i = 0; i < tags.length; i++) {
        var c = await crypto.subtle.encrypt({ name: 'AES-GCM', iv: iv, tagLength: tags[i] }, key, msg);
        assertEqual(c.byteLength, msg.length + tags[i] / 8, 'tagLength ' + tags[i] + ' size');
        var p = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: iv, tagLength: tags[i] }, key, c);
        assertEqual(new TextDecoder().decode(p), 'short tags authenticate too', 'tagLength ' + tags[i] + ' round trip');
        var bad = new Uint8Array(c);
        bad[bad.length - 1] ^= 1;
        await rejectsWith(crypto.subtle.decrypt({ name: 'AES-GCM', iv: iv, tagLength: tags[i] }, key, bad),
            'OperationError', 'tagLength ' + tags[i] + ' tamper');
    }
    // A 32-bit tag is the leading bytes of the 128-bit one.
    var full = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv: iv }, key, msg));
    var tag32 = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv: iv, tagLength: 32 }, key, msg));
    assertEqual(hex(tag32), hex(full.subarray(0, msg.length + 4)), 'truncated tag is a prefix');
}

// IVs of 8 and 60 bytes, which take the GHASH-derived J0 rather than
// IV || 0^31 || 1. The 8-byte case is McGrew & Viega test case 5; the 60-byte
// expectation is OpenSSL's output for the same key/plaintext/AAD.
async function testGcmIvLengths() {
    var key = await crypto.subtle.importKey('raw', unhex('feffe9928665731c6d6a8f9467308308'),
        'AES-GCM', false, ['encrypt', 'decrypt']);
    var pt = unhex('d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72' +
                   '1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39');
    var aad = unhex('feedfacedeadbeeffeedfacedeadbeefabaddad2');
    var iv60 = unhex('9313225df88406e5a55909c5aff5269aa6a7a9538534f7da1e4c303d2a318a72' +
                     '8c3c0c95156809539fcf0e2429a6b525416aedf9aa0de657ba637b39');
    var ct = await crypto.subtle.encrypt({ name: 'AES-GCM', iv: iv60, additionalData: aad }, key, pt);
    assertEqual(hex(ct),
        '5b52133b0eeac3ddf640d04230452a941dbba658c55c04c774163626ef425a11' +
        'd419091877f4f94ac4ac3062debf80652e26ae9edf280b4e48def1cc' +
        '2b1e70367316110ad70ade0fc44d9418', 'GCM 60-byte IV');
    var back = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: iv60, additionalData: aad }, key, ct);
    assertEqual(hex(back), hex(pt), 'GCM 60-byte IV decrypt');

    var iv8 = unhex('cafebabefacedbad');
    var ct8 = await crypto.subtle.encrypt({ name: 'AES-GCM', iv: iv8, additionalData: aad }, key, pt);
    assertEqual(hex(ct8),
        '61353b4c2806934a777ff51fa22a4755699b2a714fcdc6f83766e5f97b6c7423' +
        '73806900e49f24b22b097544d4896b424989b5e1ebac0f07c23f4598' +
        '3612d2e79e3b0785561be14aaca2fccb', 'GCM test case 5 (8-byte IV)');
    var back8 = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: iv8, additionalData: aad, tagLength: 128 }, key, ct8);
    assertEqual(hex(back8), hex(pt), 'GCM test case 5 decrypt');

    var iv1 = new Uint8Array([7]);
    var c1 = await crypto.subtle.encrypt({ name: 'AES-GCM', iv: iv1, tagLength: 64 }, key, enc.encode('one'));
    var p1 = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: iv1, tagLength: 64 }, key, c1);
    assertEqual(new TextDecoder().decode(p1), 'one', '1-byte IV round trip');
    await rejectsWith(crypto.subtle.decrypt({ name: 'AES-GCM', iv: new Uint8Array([8]), tagLength: 64 }, key, c1),
        'OperationError', 'wrong IV fails authentication');
}

(async function() {
    await testInterfaces();
    await testDigestVectors();
    await testHmacVector();
    await testJwk();
    await testAesCbc();
    await testAesCtr();
    await testAesKw();
    await testPbkdf2();
    await testHkdf();
    await testWrapJwk();
    await testGcmTagLength();
    await testGcmIvLengths();
})().catch(function(e) {
    assert(false, 'test_crypto_subtle_kat failed: ' + (e && e.stack || e));
});
