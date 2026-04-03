// Test: WritableStream, TransformStream, TextEncoderStream

// ── API existence ────────────────────────────────────────────────────────
assert(typeof WritableStream === 'function', 'WritableStream exists');
assert(typeof WritableStreamDefaultWriter === 'function', 'WritableStreamDefaultWriter exists');
assert(typeof WritableStreamDefaultController === 'function', 'WritableStreamDefaultController exists');
assert(typeof TransformStream === 'function', 'TransformStream exists');
assert(typeof TransformStreamDefaultController === 'function', 'TransformStreamDefaultController exists');
assert(typeof TextEncoderStream === 'function', 'TextEncoderStream exists');
assert(typeof TextDecoderStream === 'function', 'TextDecoderStream still exists');

// ── WritableStream basic ─────────────────────────────────────────────────
var ws1 = new WritableStream();
assertEqual(ws1.locked, false, 'not locked initially');

// ── getWriter / locking ──────────────────────────────────────────────────
var writer1 = ws1.getWriter();
assertEqual(ws1.locked, true, 'locked after getWriter');

var threw = false;
try { ws1.getWriter(); } catch (e) { threw = true; }
assert(threw, 'cannot get second writer');

writer1.releaseLock();
assertEqual(ws1.locked, false, 'unlocked after releaseLock');

// Re-acquire
var writer2 = ws1.getWriter();
assertEqual(ws1.locked, true, 'locked again after re-acquire');
writer2.releaseLock();

// ── WritableStream with underlying sink ──────────────────────────────────
var chunks = [];
var ws2 = new WritableStream({
    write: function(chunk) {
        chunks.push(chunk);
    }
});

var w2 = ws2.getWriter();
w2.write('hello').then(function() {
    assertEqual(chunks.length, 1, 'one chunk written');
    assertEqual(chunks[0], 'hello', 'chunk value');
    return w2.write('world');
}).then(function() {
    assertEqual(chunks.length, 2, 'two chunks written');
    assertEqual(chunks[1], 'world', 'second chunk value');
    return w2.close();
}).then(function() {
    assert(true, 'writer closed successfully');
});

// ── WritableStream with async write ──────────────────────────────────────
var asyncChunks = [];
var ws3 = new WritableStream({
    write: function(chunk) {
        return new Promise(function(resolve) {
            asyncChunks.push(chunk);
            resolve();
        });
    }
});

var w3 = ws3.getWriter();
w3.write('a').then(function() {
    return w3.write('b');
}).then(function() {
    return w3.write('c');
}).then(function() {
    assertEqual(asyncChunks.length, 3, 'async: three chunks');
    assertEqual(asyncChunks[0], 'a', 'async chunk a');
    assertEqual(asyncChunks[1], 'b', 'async chunk b');
    assertEqual(asyncChunks[2], 'c', 'async chunk c');
});

// ── WritableStream close callback ────────────────────────────────────────
var closeCalled = false;
var ws4 = new WritableStream({
    close: function() {
        closeCalled = true;
    }
});

var w4 = ws4.getWriter();
w4.close().then(function() {
    assert(closeCalled, 'close callback invoked');
});

// ── WritableStream start callback ────────────────────────────────────────
var startController = null;
var ws5 = new WritableStream({
    start: function(controller) {
        startController = controller;
    }
});
assert(startController !== null, 'start callback received controller');

// ── Writer.closed promise ────────────────────────────────────────────────
var closeChunks = [];
var ws6 = new WritableStream({
    write: function(chunk) { closeChunks.push(chunk); }
});
var w6 = ws6.getWriter();
var closedResolved = false;
w6.closed.then(function() { closedResolved = true; });
w6.write('x').then(function() {
    return w6.close();
}).then(function() {
    assert(closedResolved, 'closed promise resolved after close');
});

// ── Writer.ready promise ─────────────────────────────────────────────────
var ws7 = new WritableStream();
var w7 = ws7.getWriter();
w7.ready.then(function() {
    assert(true, 'ready promise resolved for writable stream');
});

// ── Writer.desiredSize ───────────────────────────────────────────────────
var ws8 = new WritableStream();
var w8 = ws8.getWriter();
assertEqual(w8.desiredSize, 1, 'desiredSize is 1 initially');
w8.releaseLock();

// ── WritableStream abort ─────────────────────────────────────────────────
var abortReason = null;
var ws9 = new WritableStream({
    abort: function(reason) {
        abortReason = reason;
    }
});
var w9 = ws9.getWriter();
w9.abort('test reason').then(function() {
    assertEqual(abortReason, 'test reason', 'abort reason passed to sink');
});

// ── Write after close rejects ────────────────────────────────────────────
var ws10 = new WritableStream();
var w10 = ws10.getWriter();
w10.close().then(function() {
    w10.write('fail').then(function() {
        assert(false, 'should not resolve');
    }).catch(function(e) {
        assert(e instanceof TypeError, 'write after close rejects with TypeError');
    });
});

// ── Write error propagation ──────────────────────────────────────────────
var ws11 = new WritableStream({
    write: function() {
        throw new Error('write failed');
    }
});
var w11 = ws11.getWriter();
w11.write('x').catch(function(e) {
    assertEqual(e.message, 'write failed', 'write error propagated');
});

// ═══════════════════════════════════════════════════════════════════════════
// TransformStream
// ═══════════════════════════════════════════════════════════════════════════

// ── Identity transform (default) ─────────────────────────────────────────
var ts1 = new TransformStream();
assert(ts1.readable instanceof ReadableStream, 'ts.readable is ReadableStream');
assert(ts1.writable instanceof WritableStream, 'ts.writable is WritableStream');

var identityWriter = ts1.writable.getWriter();
var identityReader = ts1.readable.getReader();

identityWriter.write('pass-through');
identityReader.read().then(function(result) {
    assertEqual(result.value, 'pass-through', 'identity transform passes data');
    assertEqual(result.done, false, 'identity not done');
});

// ── Custom transform ────────────────────────────────────────────────────
var ts2 = new TransformStream({
    transform: function(chunk, controller) {
        controller.enqueue(chunk.toUpperCase());
    }
});

var tw2 = ts2.writable.getWriter();
var tr2 = ts2.readable.getReader();

tw2.write('hello');
tr2.read().then(function(result) {
    assertEqual(result.value, 'HELLO', 'custom transform applied');
});

// ── Transform with flush ────────────────────────────────────────────────
var ts3 = new TransformStream({
    transform: function(chunk, controller) {
        controller.enqueue(chunk);
    },
    flush: function(controller) {
        controller.enqueue('FLUSHED');
    }
});

var tw3 = ts3.writable.getWriter();
var tr3 = ts3.readable.getReader();

tw3.write('data');
tw3.close();

tr3.read().then(function(r) {
    assertEqual(r.value, 'data', 'flush: data chunk');
    return tr3.read();
}).then(function(r) {
    assertEqual(r.value, 'FLUSHED', 'flush: flush chunk');
    return tr3.read();
}).then(function(r) {
    assertEqual(r.done, true, 'flush: done after flush');
});

// ── Transform with start ────────────────────────────────────────────────
var tsStarted = false;
var ts4 = new TransformStream({
    start: function(controller) {
        tsStarted = true;
        controller.enqueue('INIT');
    },
    transform: function(chunk, controller) {
        controller.enqueue(chunk);
    }
});
assert(tsStarted, 'transform start called');

var tr4 = ts4.readable.getReader();
tr4.read().then(function(r) {
    assertEqual(r.value, 'INIT', 'start enqueued init chunk');
});

// ── 1:N transform (one input, multiple outputs) ─────────────────────────
var ts5 = new TransformStream({
    transform: function(chunk, controller) {
        for (var i = 0; i < chunk.length; i++) {
            controller.enqueue(chunk[i]);
        }
    }
});

var tw5 = ts5.writable.getWriter();
var tr5 = ts5.readable.getReader();

tw5.write('abc');
tr5.read().then(function(r) {
    assertEqual(r.value, 'a', '1:N transform: a');
    return tr5.read();
}).then(function(r) {
    assertEqual(r.value, 'b', '1:N transform: b');
    return tr5.read();
}).then(function(r) {
    assertEqual(r.value, 'c', '1:N transform: c');
});

// ── Transform error ─────────────────────────────────────────────────────
var ts6 = new TransformStream({
    transform: function(chunk, controller) {
        if (chunk === 'bad') throw new Error('bad chunk');
        controller.enqueue(chunk);
    }
});

var tw6 = ts6.writable.getWriter();
tw6.write('bad').catch(function(e) {
    assertEqual(e.message, 'bad chunk', 'transform error propagated');
});

// ═══════════════════════════════════════════════════════════════════════════
// pipeThrough with real TransformStream
// ═══════════════════════════════════════════════════════════════════════════

var source = new ReadableStream({
    start: function(controller) {
        controller.enqueue('hello');
        controller.enqueue('world');
        controller.close();
    }
});

var upper = new TransformStream({
    transform: function(chunk, controller) {
        controller.enqueue(chunk.toUpperCase());
    }
});

var reader = source.pipeThrough(upper).getReader();
reader.read().then(function(r) {
    assertEqual(r.value, 'HELLO', 'pipeThrough: HELLO');
    return reader.read();
}).then(function(r) {
    assertEqual(r.value, 'WORLD', 'pipeThrough: WORLD');
    return reader.read();
}).then(function(r) {
    assertEqual(r.done, true, 'pipeThrough: done');
});

// ═══════════════════════════════════════════════════════════════════════════
// pipeTo with real WritableStream
// ═══════════════════════════════════════════════════════════════════════════

var pipeCollected = [];
var pipeSource = new ReadableStream({
    start: function(controller) {
        controller.enqueue(1);
        controller.enqueue(2);
        controller.enqueue(3);
        controller.close();
    }
});

var pipeDest = new WritableStream({
    write: function(chunk) {
        pipeCollected.push(chunk);
    }
});

pipeSource.pipeTo(pipeDest).then(function() {
    assertEqual(pipeCollected.length, 3, 'pipeTo: 3 chunks');
    assertEqual(pipeCollected[0], 1, 'pipeTo: chunk 1');
    assertEqual(pipeCollected[1], 2, 'pipeTo: chunk 2');
    assertEqual(pipeCollected[2], 3, 'pipeTo: chunk 3');
});

// ═══════════════════════════════════════════════════════════════════════════
// TextDecoderStream (refactored to use TransformStream)
// ═══════════════════════════════════════════════════════════════════════════

var tds = new TextDecoderStream();
assert(tds.readable instanceof ReadableStream, 'TextDecoderStream has readable');
assert(tds.writable instanceof WritableStream, 'TextDecoderStream has writable');

var tdsWriter = tds.writable.getWriter();
var tdsReader = tds.readable.getReader();

// Encode "hello" as Uint8Array
var encoded = new TextEncoder().encode('hello');
tdsWriter.write(encoded);
tdsReader.read().then(function(r) {
    assertEqual(r.value, 'hello', 'TextDecoderStream decodes correctly');
});

// ═══════════════════════════════════════════════════════════════════════════
// TextEncoderStream
// ═══════════════════════════════════════════════════════════════════════════

var tes = new TextEncoderStream();
assertEqual(tes.encoding, 'utf-8', 'TextEncoderStream encoding');
assert(tes.readable instanceof ReadableStream, 'TextEncoderStream has readable');
assert(tes.writable instanceof WritableStream, 'TextEncoderStream has writable');

var tesWriter = tes.writable.getWriter();
var tesReader = tes.readable.getReader();

tesWriter.write('hello');
tesReader.read().then(function(r) {
    assert(r.value instanceof Uint8Array, 'TextEncoderStream produces Uint8Array');
    assertEqual(r.value.length, 5, 'TextEncoderStream: 5 bytes for hello');
    assertEqual(r.value[0], 104, 'TextEncoderStream: h');
    assertEqual(r.value[4], 111, 'TextEncoderStream: o');
});

// ═══════════════════════════════════════════════════════════════════════════
// Chained transforms: ReadableStream -> TextEncoderStream -> TextDecoderStream
// ═══════════════════════════════════════════════════════════════════════════

var chainSource = new ReadableStream({
    start: function(controller) {
        controller.enqueue('round');
        controller.enqueue('trip');
        controller.close();
    }
});

var chainReader = chainSource
    .pipeThrough(new TextEncoderStream())
    .pipeThrough(new TextDecoderStream())
    .getReader();

chainReader.read().then(function(r) {
    assertEqual(r.value, 'round', 'chain: round-tripped round');
    return chainReader.read();
}).then(function(r) {
    assertEqual(r.value, 'trip', 'chain: round-tripped trip');
    return chainReader.read();
}).then(function(r) {
    assertEqual(r.done, true, 'chain: done');
});
