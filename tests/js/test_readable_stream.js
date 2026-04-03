// Test: ReadableStream

// ── Constructor and basic types ───────────────────────────────────────────
assert(typeof ReadableStream === 'function', 'ReadableStream exists');
assert(typeof ReadableStreamDefaultReader === 'function', 'Reader exists');
assert(typeof ReadableStreamDefaultController === 'function', 'Controller exists');
assert(typeof TextDecoderStream === 'function', 'TextDecoderStream exists');

// ── Basic ReadableStream from push source ─────────────────────────────────
var pushStream = new ReadableStream({
    start: function(controller) {
        controller.enqueue('a');
        controller.enqueue('b');
        controller.enqueue('c');
        controller.close();
    }
});
assert(pushStream instanceof ReadableStream, 'instanceof ReadableStream');
assertEqual(pushStream.locked, false, 'not locked initially');

var pushReader = pushStream.getReader();
assertEqual(pushStream.locked, true, 'locked after getReader');

pushReader.read()
    .then(function(r) {
        assertEqual(r.value, 'a', 'push stream: chunk a');
        assertEqual(r.done, false, 'push stream: not done');
        return pushReader.read();
    })
    .then(function(r) {
        assertEqual(r.value, 'b', 'push stream: chunk b');
        return pushReader.read();
    })
    .then(function(r) {
        assertEqual(r.value, 'c', 'push stream: chunk c');
        return pushReader.read();
    })
    .then(function(r) {
        assertEqual(r.done, true, 'push stream: done');
        assertEqual(r.value, undefined, 'push stream: done value undefined');
    });

// ── Pull-based ReadableStream ─────────────────────────────────────────────
var pullCount = 0;
var pullStream = new ReadableStream({
    pull: function(controller) {
        pullCount++;
        if (pullCount <= 3) {
            controller.enqueue('chunk' + pullCount);
        } else {
            controller.close();
        }
    }
});

var pullReader = pullStream.getReader();
pullReader.read()
    .then(function(r) {
        assertEqual(r.value, 'chunk1', 'pull stream: chunk1');
        return pullReader.read();
    })
    .then(function(r) {
        assertEqual(r.value, 'chunk2', 'pull stream: chunk2');
        return pullReader.read();
    })
    .then(function(r) {
        assertEqual(r.value, 'chunk3', 'pull stream: chunk3');
        return pullReader.read();
    })
    .then(function(r) {
        assertEqual(r.done, true, 'pull stream: done');
    });

// ── Async pull (returns Promise) ──────────────────────────────────────────
var asyncPullCount = 0;
var asyncStream = new ReadableStream({
    pull: function(controller) {
        asyncPullCount++;
        return new Promise(function(resolve) {
            if (asyncPullCount <= 2) {
                controller.enqueue('async' + asyncPullCount);
            } else {
                controller.close();
            }
            resolve();
        });
    }
});

var asyncReader = asyncStream.getReader();
asyncReader.read()
    .then(function(r) {
        assertEqual(r.value, 'async1', 'async pull: chunk1');
        return asyncReader.read();
    })
    .then(function(r) {
        assertEqual(r.value, 'async2', 'async pull: chunk2');
        return asyncReader.read();
    })
    .then(function(r) {
        assertEqual(r.done, true, 'async pull: done');
    });

// ── Reader.releaseLock ────────────────────────────────────────────────────
var lockStream = new ReadableStream({
    start: function(controller) { controller.enqueue('x'); controller.close(); }
});
var lockReader = lockStream.getReader();
assertEqual(lockStream.locked, true, 'locked');
lockReader.releaseLock();
assertEqual(lockStream.locked, false, 'unlocked after releaseLock');

// Can get a new reader
var lockReader2 = lockStream.getReader();
lockReader2.read().then(function(r) {
    assertEqual(r.value, 'x', 'new reader reads data');
});

// ── Controller.error ──────────────────────────────────────────────────────
var errStream = new ReadableStream({
    start: function(controller) {
        controller.error(new Error('test error'));
    }
});
var errReader = errStream.getReader();
errReader.read().then(
    function() { assert(false, 'error stream: should not resolve'); },
    function(e) {
        assert(e instanceof Error, 'error stream: rejected with Error');
        assertEqual(e.message, 'test error', 'error stream: message');
    }
);

// ── ReadableStream.cancel ─────────────────────────────────────────────────
var cancelCalled = false;
var cancelStream = new ReadableStream({
    cancel: function(reason) { cancelCalled = true; }
});
cancelStream.cancel('done').then(function() {
    assert(cancelCalled, 'cancel called on source');
});

// ── ReadableStream.tee ────────────────────────────────────────────────────
var teeStream = new ReadableStream({
    start: function(controller) {
        controller.enqueue(1);
        controller.enqueue(2);
        controller.close();
    }
});
var branches = teeStream.tee();
assertEqual(branches.length, 2, 'tee returns 2 branches');
assert(branches[0] instanceof ReadableStream, 'tee branch 1 is ReadableStream');
assert(branches[1] instanceof ReadableStream, 'tee branch 2 is ReadableStream');

var teeReader1 = branches[0].getReader();
teeReader1.read()
    .then(function(r) {
        assertEqual(r.value, 1, 'tee branch1: value 1');
        return teeReader1.read();
    })
    .then(function(r) {
        assertEqual(r.value, 2, 'tee branch1: value 2');
        return teeReader1.read();
    })
    .then(function(r) {
        assertEqual(r.done, true, 'tee branch1: done');
    });

// ── ReadableStream.from (array) ───────────────────────────────────────────
var fromStream = ReadableStream.from([10, 20, 30]);
var fromReader = fromStream.getReader();
fromReader.read()
    .then(function(r) {
        assertEqual(r.value, 10, 'from array: value 10');
        return fromReader.read();
    })
    .then(function(r) {
        assertEqual(r.value, 20, 'from array: value 20');
        return fromReader.read();
    })
    .then(function(r) {
        assertEqual(r.value, 30, 'from array: value 30');
        return fromReader.read();
    })
    .then(function(r) {
        assertEqual(r.done, true, 'from array: done');
    });

// ── Symbol.asyncIterator ──────────────────────────────────────────────────
var iterStream = new ReadableStream({
    start: function(controller) {
        controller.enqueue('x');
        controller.enqueue('y');
        controller.close();
    }
});
var iter = iterStream[Symbol.asyncIterator]();
assert(typeof iter.next === 'function', 'asyncIterator has next');
iter.next()
    .then(function(r) {
        assertEqual(r.value, 'x', 'asyncIterator: x');
        return iter.next();
    })
    .then(function(r) {
        assertEqual(r.value, 'y', 'asyncIterator: y');
        return iter.next();
    })
    .then(function(r) {
        assertEqual(r.done, true, 'asyncIterator: done');
    });

// ── Uint8Array chunks (binary data) ──────────────────────────────────────
var binStream = new ReadableStream({
    start: function(controller) {
        controller.enqueue(new Uint8Array([72, 101, 108, 108, 111])); // "Hello"
        controller.enqueue(new Uint8Array([32, 87, 111, 114, 108, 100])); // " World"
        controller.close();
    }
});
var binReader = binStream.getReader();
binReader.read()
    .then(function(r) {
        assert(r.value instanceof Uint8Array, 'binary: Uint8Array chunk');
        assertEqual(r.value.length, 5, 'binary: chunk 1 length');
        return binReader.read();
    })
    .then(function(r) {
        assertEqual(r.value.length, 6, 'binary: chunk 2 length');
        return binReader.read();
    })
    .then(function(r) {
        assertEqual(r.done, true, 'binary: done');
    });

// ── TextDecoderStream ─────────────────────────────────────────────────────
var tds = new TextDecoderStream();
assert(tds.readable instanceof ReadableStream, 'TDS has readable');
assert(typeof tds.writable === 'object', 'TDS has writable');

// ── Reader.closed promise ─────────────────────────────────────────────────
var closedStream = new ReadableStream({
    start: function(controller) { controller.close(); }
});
var closedReader = closedStream.getReader();
closedReader.closed.then(function() {
    assert(true, 'closed promise resolved for closed stream');
});

// ── desiredSize ───────────────────────────────────────────────────────────
var dsController;
var dsStream = new ReadableStream({
    start: function(c) { dsController = c; }
});
assertEqual(dsController.desiredSize, 1, 'desiredSize starts at 1');
dsController.enqueue('item');
assertEqual(dsController.desiredSize, 0, 'desiredSize after enqueue');
dsController.close();

// ── Empty stream ──────────────────────────────────────────────────────────
var emptyStream = new ReadableStream({
    start: function(controller) { controller.close(); }
});
var emptyReader = emptyStream.getReader();
emptyReader.read().then(function(r) {
    assertEqual(r.done, true, 'empty stream: immediately done');
    assertEqual(r.value, undefined, 'empty stream: no value');
});

// ── Double getReader throws ───────────────────────────────────────────────
var dblStream = new ReadableStream({
    start: function(c) { c.close(); }
});
dblStream.getReader();
var dblThrew = false;
try { dblStream.getReader(); } catch(e) { dblThrew = true; }
assert(dblThrew, 'double getReader throws TypeError');
