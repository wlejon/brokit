// Test: every native method refuses a handle of another class as its
// receiver or handle argument. The binding unwraps with the class brand
// (src/api/host_class.cpp), never by reinterpreting whatever payload a
// handle carries, so a Blob, File, FileReader, CryptoKey, FastNoise or
// codec in the wrong slot throws a TypeError (or is treated as "not one")
// instead of being read as the wrong struct.

function expectTypeError(fn, what) {
    var err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ': did not throw');
    assert(err instanceof TypeError, what + ': threw ' + err + ', not a TypeError');
}

var SIMPLEX_ENCODED = "E@BBZEG@BD8JFgIECArXIzwECiQIw/UoPwkuAAE@BJDQAH@BC@AIEAJBw@ABZEED0KV78YZmZmPwQDmpkZPwsAAIA/HAMAAHBCBA==";

var blob = new Blob(['wrong receiver'], { type: 'text/plain' });
var file = new File(['file bytes'], 'a.txt', { type: 'text/plain' });
var reader = new FileReader();
var noise = new FastNoise(SIMPLEX_ENCODED);
var codec = globalThis.__brokit_compression.create('compress', 'gzip');

// ---- FastNoise methods on other handles ----
var noiseMethods = ['genSingle2D', 'genSingle3D', 'genUniformGrid2D', 'genUniformGrid2DInto',
                    'genUniformGrid3D', 'genUniformGrid3DInto', 'genTileable2D',
                    'genPositionArray2D', 'genPositionArray3D', 'set', 'getMembers'];
var others = [blob, file, reader, codec];
for (var i = 0; i < noiseMethods.length; i++) {
    var m = FastNoise.prototype[noiseMethods[i]];
    if (typeof m !== 'function') continue;
    for (var j = 0; j < others.length; j++) {
        (function (m, r, name) {
            expectTypeError(function () { m.call(r, 0, 0, 0, 1337); },
                            'FastNoise.' + name + ' on a foreign handle');
        })(m, others[j], noiseMethods[i]);
    }
}
// A node input that is not a FastNoise.
var meta = noise.getMembers();
if (meta.nodes && meta.nodes.length > 0) {
    expectTypeError(function () { noise.set(meta.nodes[0].name, blob); },
                    'FastNoise node input given a Blob');
}

// ---- Blob methods on other handles ----
var blobMethods = ['slice', 'arrayBuffer', 'text', 'bytes'];
var notBlobs = [reader, noise, codec];
for (var i = 0; i < blobMethods.length; i++) {
    var m = Blob.prototype[blobMethods[i]];
    if (typeof m !== 'function') continue;
    for (var j = 0; j < notBlobs.length; j++) {
        (function (m, r, name) {
            var err = null;
            try {
                var p = m.call(r);
                if (p && typeof p.then === 'function') p.then(null, function () {});
            } catch (e) { err = e; }
            assert(err instanceof TypeError, 'Blob.' + name + ' on a foreign handle threw ' + err);
        })(m, notBlobs[j], blobMethods[i]);
    }
}
// A File is still a Blob.
assertEqual(Blob.prototype.slice.call(file, 0, 4).size, 4, 'Blob.slice on a File');
var sizeGet = Object.getOwnPropertyDescriptor(Blob.prototype, 'size').get;
assertEqual(sizeGet.call(file), 10, 'Blob size getter on a File');
assertEqual(sizeGet.call(noise), 0, 'Blob size getter on a FastNoise');
var nameGet = Object.getOwnPropertyDescriptor(File.prototype, 'name').get;
assertEqual(nameGet.call(blob), '', 'File name getter on a Blob');
assertEqual(nameGet.call(noise), '', 'File name getter on a FastNoise');

// ---- FileReader methods on other handles ----
var readerMethods = ['readAsArrayBuffer', 'readAsText', 'readAsBinaryString', 'readAsDataURL', 'abort'];
var notReaders = [blob, file, noise, codec];
for (var i = 0; i < readerMethods.length; i++) {
    var m = FileReader.prototype[readerMethods[i]];
    for (var j = 0; j < notReaders.length; j++) {
        (function (m, r, name) {
            expectTypeError(function () { m.call(r, blob); }, 'FileReader.' + name + ' on a foreign handle');
        })(m, notReaders[j], readerMethods[i]);
    }
}
var rsGet = Object.getOwnPropertyDescriptor(FileReader.prototype, 'readyState').get;
assertEqual(rsGet.call(noise), 0, 'readyState getter on a FastNoise');
var resultGet = Object.getOwnPropertyDescriptor(FileReader.prototype, 'result').get;
assertEqual(resultGet.call(blob), null, 'result getter on a Blob');

// A FileReader given a non-Blob reports it rather than reading the payload.
var badRead = new FileReader();
var badReadDone = new Promise(function (resolve) {
    badRead.onloadend = function () { resolve(badRead.error); };
});
badRead.readAsText(noise);

// ---- the compression codec on other handles ----
var codecProto = Object.getPrototypeOf(codec);
expectTypeError(function () { codecProto.push.call(blob, new Uint8Array(4)); }, 'codec.push on a Blob');
expectTypeError(function () { codecProto.finish.call(noise); }, 'codec.finish on a FastNoise');

// ---- crypto.subtle with a non-key where a key is expected ----
async function testSubtle() {
    var data = new TextEncoder().encode('x');
    var candidates = [blob, file, reader, noise, codec];
    for (var i = 0; i < candidates.length; i++) {
        var k = candidates[i];
        var ops = [
            function () { return crypto.subtle.exportKey('raw', k); },
            function () { return crypto.subtle.sign('HMAC', k, data); },
            function () { return crypto.subtle.encrypt({ name: 'AES-GCM', iv: new Uint8Array(12) }, k, data); },
        ];
        for (var j = 0; j < ops.length; j++) {
            var err = null;
            try { await ops[j](); } catch (e) { err = e; }
            assert(err !== null, 'subtle op ' + j + ' accepted a foreign handle as a key');
        }
    }
    var badReadErr = await badReadDone;
    assert(badReadErr !== null && badReadErr.name === 'NotFoundError',
           'readAsText of a FastNoise reports NotFoundError');
}

testSubtle().catch(function (e) {
    assert(false, 'test_wrong_receiver failed: ' + (e && e.stack || e));
});
