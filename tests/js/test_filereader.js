// Test: FileReader constants, async read, and event lifecycle

assert(typeof FileReader === 'function', 'FileReader constructor exists');
assertEqual(FileReader.EMPTY, 0, 'FileReader.EMPTY');
assertEqual(FileReader.LOADING, 1, 'FileReader.LOADING');
assertEqual(FileReader.DONE, 2, 'FileReader.DONE');

var r0 = new FileReader();
assertEqual(r0.readyState, 0, 'initial readyState is EMPTY');
assertEqual(r0.result, null, 'initial result is null');
assertEqual(r0.error, null, 'initial error is null');
assertEqual(r0.EMPTY, 0, 'instance.EMPTY');
assertEqual(r0.LOADING, 1, 'instance.LOADING');
assertEqual(r0.DONE, 2, 'instance.DONE');

// Test asynchronous delivery & readAsText
var b1 = new Blob(['hello filereader'], { type: 'text/plain' });
var r1 = new FileReader();
var r1SyncDelivered = false;
var r1Done = false;

r1.readAsText(b1);
assertEqual(r1.readyState, 1, 'readyState is LOADING immediately after readAsText');

// Handlers assigned AFTER readAsText
r1.onload = function () {
    if (!syncCheckpoint) r1SyncDelivered = true;
    assertEqual(r1.result, 'hello filereader', 'readAsText result matches');
    assertEqual(r1.readyState, 2, 'readyState is DONE in onload');
    r1Done = true;
};

// Test readAsBinaryString
var binBlob = new Blob([new Uint8Array([104, 105, 0, 255])]);
var r2 = new FileReader();
var r2Done = false;
r2.readAsBinaryString(binBlob);
r2.onload = function () {
    assertEqual(r2.result.length, 4, 'readAsBinaryString length');
    assertEqual(r2.result.charCodeAt(0), 104, 'byte 0');
    assertEqual(r2.result.charCodeAt(1), 105, 'byte 1');
    assertEqual(r2.result.charCodeAt(2), 0, 'byte 2');
    assertEqual(r2.result.charCodeAt(3), 255, 'byte 3');
    r2Done = true;
};

// Test readAsDataURL
var r3 = new FileReader();
var r3Done = false;
r3.readAsDataURL(new Blob(['hi!'], { type: 'text/plain' }));
r3.onload = function () {
    assertEqual(r3.result, 'data:text/plain;base64,aGkh', 'readAsDataURL result');
    r3Done = true;
};

// Test readAsArrayBuffer & addEventListener
var r4 = new FileReader();
var r4Done = false;
r4.readAsArrayBuffer(new Blob([new Uint8Array([1, 2, 3])]));
r4.addEventListener('load', function (e) {
    assert(e.target === r4, 'event target is reader');
    assert(r4.result instanceof ArrayBuffer, 'result is ArrayBuffer');
    assertEqual(r4.result.byteLength, 3, 'ArrayBuffer byteLength');
    r4Done = true;
});

// Test abort()
var r5 = new FileReader();
var r5AbortedCalled = false;
r5.readAsText(b1);
r5.onload = function () {
    r5AbortedCalled = true;
};
r5.abort();
assertEqual(r5.readyState, 2, 'readyState is DONE after abort');
assertEqual(r5.result, null, 'result is null after abort');

var syncCheckpoint = true;
assert(!r1SyncDelivered, 'read was not delivered synchronously');
