// Test: MessageChannel, MessagePort, MessageEvent

// --- MessageEvent ---
assert(typeof MessageEvent === 'function', 'MessageEvent exists');

var me = new MessageEvent('message', { data: 'hello' });
assertEqual(me.type, 'message', 'MessageEvent type');
assertEqual(me.data, 'hello', 'MessageEvent data');
assert(me instanceof Event, 'MessageEvent instanceof Event');

// --- MessageChannel ---
assert(typeof MessageChannel === 'function', 'MessageChannel exists');

var ch = new MessageChannel();
assert(ch.port1 instanceof MessagePort, 'port1 is MessagePort');
assert(ch.port2 instanceof MessagePort, 'port2 is MessagePort');

// --- Basic messaging (ports auto-start with onmessage) ---
var received = [];
ch.port2.onmessage = function(e) { received.push(e.data); };
ch.port2.start();
ch.port1.postMessage('hello');
ch.port1.postMessage('world');

// Messages are delivered via queueMicrotask, need to pump
// (test harness pumps pending jobs after eval)

// --- Structured clone of data ---
var ch2 = new MessageChannel();
var receivedObj;
ch2.port2.onmessage = function(e) { receivedObj = e.data; };
ch2.port2.start();
ch2.port1.postMessage({ key: 'value', num: 42 });

// --- EventTarget integration ---
var ch3 = new MessageChannel();
var eventReceived;
ch3.port2.addEventListener('message', function(e) {
    eventReceived = e.data;
});
ch3.port2.start();
ch3.port1.postMessage('via-event');

// --- Close ---
var ch4 = new MessageChannel();
var closedReceived = [];
ch4.port2.onmessage = function(e) { closedReceived.push(e.data); };
ch4.port2.start();
ch4.port1.postMessage('before');
ch4.port1.close();
ch4.port1.postMessage('after'); // should not arrive

// --- Queue before start ---
var ch5 = new MessageChannel();
var queuedData = [];
ch5.port1.postMessage('queued1');
ch5.port1.postMessage('queued2');
// Not started yet — messages are queued
ch5.port2.onmessage = function(e) { queuedData.push(e.data); };
ch5.port2.start(); // should drain queue
