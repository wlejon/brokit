// Test: XMLHttpRequest over fetch — offline paths only (data:, blob:, local
// files, and an unreachable local port for the error path).

assert(typeof XMLHttpRequest === 'function', 'XMLHttpRequest exists');
assert(typeof XMLHttpRequestUpload === 'function', 'XMLHttpRequestUpload exists');
assert(typeof XMLHttpRequestEventTarget === 'function', 'XMLHttpRequestEventTarget exists');

// --- constants, on the constructor and the prototype ---
assertEqual(XMLHttpRequest.UNSENT, 0, 'UNSENT');
assertEqual(XMLHttpRequest.OPENED, 1, 'OPENED');
assertEqual(XMLHttpRequest.HEADERS_RECEIVED, 2, 'HEADERS_RECEIVED');
assertEqual(XMLHttpRequest.LOADING, 3, 'LOADING');
assertEqual(XMLHttpRequest.DONE, 4, 'DONE');
assertEqual(new XMLHttpRequest().DONE, 4, 'DONE on instance');

// --- fresh instance ---
var x0 = new XMLHttpRequest();
assert(x0 instanceof XMLHttpRequest, 'instanceof XMLHttpRequest');
assert(x0 instanceof XMLHttpRequestEventTarget, 'instanceof XMLHttpRequestEventTarget');
assert(Object.getPrototypeOf(x0) === XMLHttpRequest.prototype, 'prototype is ctor.prototype');
assertEqual(x0.readyState, 0, 'fresh readyState is UNSENT');
assertEqual(x0.status, 0, 'fresh status');
assertEqual(x0.statusText, '', 'fresh statusText');
assertEqual(x0.responseType, '', 'fresh responseType');
assertEqual(x0.responseText, '', 'fresh responseText');
assertEqual(x0.response, '', 'fresh response');
assertEqual(x0.timeout, 0, 'fresh timeout');
assertEqual(x0.withCredentials, false, 'fresh withCredentials');
assert(x0.upload instanceof XMLHttpRequestUpload, 'upload is an XMLHttpRequestUpload');
assertEqual(x0.onreadystatechange, null, 'onreadystatechange defaults null');
assertEqual(x0.onload, null, 'onload defaults null');
assertEqual(x0.upload.onprogress, null, 'upload.onprogress defaults null');
assertEqual(x0.getAllResponseHeaders(), '', 'no headers before a response');
assertEqual(x0.getResponseHeader('content-type'), null, 'no header before a response');

x0.timeout = 1500;
x0.withCredentials = true;
assertEqual(x0.timeout, 1500, 'timeout stored');
assertEqual(x0.withCredentials, true, 'withCredentials stored');

// --- responseType ---
x0.responseType = 'text';
assertEqual(x0.responseType, 'text', 'responseType text');
x0.responseType = 'arraybuffer';
assertEqual(x0.responseType, 'arraybuffer', 'responseType arraybuffer');
x0.responseType = 'blob';
assertEqual(x0.responseType, 'blob', 'responseType blob');
x0.responseType = 'json';
assertEqual(x0.responseType, 'json', 'responseType json');
var badType = false;
try { x0.responseType = 'invalid_type'; } catch (e) { badType = true; }
assert(badType, 'unsupported responseType throws');
assertEqual(x0.responseType, 'json', 'unsupported responseType leaves the old value');
var rtThrew = false;
try { x0.responseText; } catch (e) { rtThrew = e.name === 'InvalidStateError'; }
assert(rtThrew, 'responseText throws InvalidStateError when responseType is json');

// --- synchronous open is refused ---
var syncThrew = null;
try { new XMLHttpRequest().open('GET', 'data:text/plain,x', false); } catch (e) { syncThrew = e; }
assert(syncThrew && syncThrew.name === 'InvalidAccessError', 'open(..., false) throws InvalidAccessError');
assert(syncThrew instanceof DOMException, 'InvalidAccessError is a DOMException');

// --- state guards ---
var guard = new XMLHttpRequest();
var setBeforeOpen = false;
try { guard.setRequestHeader('x-a', '1'); } catch (e) { setBeforeOpen = e.name === 'InvalidStateError'; }
assert(setBeforeOpen, 'setRequestHeader before open throws InvalidStateError');
var sendBeforeOpen = false;
try { guard.send(); } catch (e) { sendBeforeOpen = e.name === 'InvalidStateError'; }
assert(sendBeforeOpen, 'send before open throws InvalidStateError');

// --- open() moves to OPENED and fires readystatechange ---
var opened = new XMLHttpRequest();
var openedStates = [];
opened.onreadystatechange = function(e) {
    openedStates.push(this.readyState);
    assert(e.target === opened, 'readystatechange target is the xhr');
    assert(e.type === 'readystatechange', 'readystatechange type');
};
opened.open('get', 'data:text/plain,x');
assertEqual(opened.readyState, 1, 'open() -> OPENED');
assertEqual(openedStates.length, 1, 'open() fires readystatechange once');
opened.open('GET', 'data:text/plain,y');
assertEqual(openedStates.length, 1, 'reopening an OPENED request does not refire');
opened.setRequestHeader('X-One', 'a');
opened.setRequestHeader('x-one', 'b');
assert(true, 'setRequestHeader combines repeated names');

// --- GET data: URL, the four transitions, headers, events ---
var payload = 'Hello XHR';
var x1 = new XMLHttpRequest();
var x1States = [];
var x1Events = [];
x1.onreadystatechange = function() { x1States.push(x1.readyState); };
x1.onloadstart = function(e) { x1Events.push('loadstart'); };
x1.onprogress = function(e) {
    x1Events.push('progress');
    assertEqual(e.loaded, payload.length, 'progress loaded');
    assertEqual(e.total, payload.length, 'progress total');
    assert(e.lengthComputable === true, 'progress lengthComputable');
};
x1.onload = function(e) {
    x1Events.push('load');
    assert(e instanceof Event, 'load event is an Event');
    assert(e.target === x1, 'load event target is the xhr');
    assert(e.currentTarget === x1, 'load event currentTarget is the xhr');
    assertEqual(x1.readyState, 4, 'DONE inside onload');
    assertEqual(x1.status, 200, 'status 200');
    assertEqual(x1.statusText, 'OK', 'statusText OK');
    assertEqual(x1.responseText, payload, 'responseText');
    assertEqual(x1.response, payload, 'response equals responseText for the default type');
    var ct = x1.getResponseHeader('Content-Type');
    assert(ct && ct.indexOf('text/plain') === 0, 'getResponseHeader is case-insensitive');
    assertEqual(x1.getResponseHeader('x-missing'), null, 'missing header is null');
    var all = x1.getAllResponseHeaders();
    assert(all.indexOf('content-type: text/plain') !== -1, 'getAllResponseHeaders lists content-type');
    assert(all.indexOf('content-length: ' + payload.length) !== -1, 'getAllResponseHeaders lists content-length');
    assert(all.slice(-2) === '\r\n', 'getAllResponseHeaders lines end with CRLF');
    assertEqual(x1.responseURL, 'data:text/plain,' + encodeURIComponent(payload), 'responseURL');
};
x1.addEventListener('load', function() { x1Events.push('load-listener'); });
x1.onloadend = function() {
    x1Events.push('loadend');
    assertEqual(x1States.join(','), '1,2,3,4', 'readyState transitions 1,2,3,4');
    assertEqual(x1Events.join(','), 'loadstart,progress,load,load-listener,loadend', 'event order for a GET');
};
x1.onerror = function() { assert(false, 'data: GET must not error'); };
x1.open('GET', 'data:text/plain,' + encodeURIComponent(payload));
x1.send();
assertEqual(x1.readyState, 1, 'still OPENED right after send()');
assertEqual(x1Events.join(','), 'loadstart', 'loadstart fires synchronously from send()');
var sendTwice = false;
try { x1.send(); } catch (e) { sendTwice = e.name === 'InvalidStateError'; }
assert(sendTwice, 'second send() throws InvalidStateError');

// --- responseType json / arraybuffer / blob / text ---
var xj = new XMLHttpRequest();
xj.responseType = 'json';
xj.onload = function() {
    assertEqual(xj.response.a, 1, 'json response parsed');
    assertEqual(xj.response.b[1], 'y', 'json response nested');
    assert(xj.response === xj.response, 'json response is cached');
};
xj.open('GET', 'data:application/json,' + encodeURIComponent('{"a":1,"b":["x","y"]}'));
xj.send();

var xjBad = new XMLHttpRequest();
xjBad.responseType = 'json';
xjBad.onload = function() {
    assertEqual(xjBad.response, null, 'unparseable json response is null');
    assertEqual(xjBad.status, 200, 'unparseable json still has status 200');
};
xjBad.open('GET', 'data:text/plain,not-json');
xjBad.send();

var xa = new XMLHttpRequest();
xa.responseType = 'arraybuffer';
assertEqual(xa.response, null, 'arraybuffer response is null before DONE');
xa.onload = function() {
    assert(xa.response instanceof ArrayBuffer, 'arraybuffer response is an ArrayBuffer');
    var v = new Uint8Array(xa.response);
    assertEqual(v.length, 6, 'arraybuffer length');
    assertEqual(v[0], 0, 'arraybuffer byte 0');
    assertEqual(v[5], 5, 'arraybuffer byte 5');
};
xa.open('GET', 'data:application/octet-stream;base64,AAECAwQF');
xa.send();

var xb = new XMLHttpRequest();
xb.responseType = 'blob';
xb.onload = function() {
    assert(xb.response instanceof Blob, 'blob response is a Blob');
    assertEqual(xb.response.size, 6, 'blob response size');
    assertEqual(xb.response.type, 'application/octet-stream', 'blob response type from content-type');
};
xb.open('GET', 'data:application/octet-stream;base64,AAECAwQF');
xb.send();

var xo = new XMLHttpRequest();
xo.responseType = 'blob';
xo.overrideMimeType('image/x-test');
xo.onload = function() {
    assertEqual(xo.response.type, 'image/x-test', 'overrideMimeType sets the blob type');
};
xo.open('GET', 'data:application/octet-stream;base64,AAECAwQF');
xo.send();

var xt = new XMLHttpRequest();
xt.responseType = 'text';
xt.onload = function() {
    assertEqual(xt.response, 'plain', 'text response');
    assertEqual(xt.responseText, 'plain', 'text responseText');
};
xt.open('GET', 'data:text/plain,plain');
xt.send();

// --- abort(): same-turn abort of a sent request ---
var xab = new XMLHttpRequest();
var abEvents = [];
var abStateInAbort = -1;
xab.onreadystatechange = function() { abEvents.push('rsc' + xab.readyState); };
xab.onabort = function(e) {
    abEvents.push('abort');
    abStateInAbort = xab.readyState;
    assert(e.target === xab, 'abort event target is the xhr');
};
xab.onload = function() { assert(false, 'aborted request must not load'); };
xab.onerror = function() { assert(false, 'aborted request must not error'); };
xab.onloadend = function() { abEvents.push('loadend'); };
xab.open('GET', 'data:text/plain,never');
xab.send();
xab.abort();
assertEqual(abStateInAbort, 4, 'readyState is DONE inside onabort');
assertEqual(xab.readyState, 0, 'readyState is UNSENT after abort()');
assertEqual(xab.status, 0, 'status is 0 after abort()');
assertEqual(abEvents.join(','), 'rsc1,rsc4,abort,loadend', 'abort event sequence');
xab.abort();
assertEqual(abEvents.length, 4, 'abort() on an idle request fires nothing');

// --- abort() on a fresh request is a no-op ---
var idle = new XMLHttpRequest();
idle.onabort = function() { assert(false, 'abort on UNSENT must not fire'); };
idle.abort();
assertEqual(idle.readyState, 0, 'abort on UNSENT leaves UNSENT');

// --- open() during a request drops the old one silently ---
var reopen = new XMLHttpRequest();
var reopenLoads = 0;
reopen.onload = function() { reopenLoads++; assertEqual(reopen.responseText, 'second', 'reopened request delivers the second response'); };
reopen.onabort = function() { assert(false, 'reopen must not fire abort'); };
reopen.open('GET', 'data:text/plain,first');
reopen.send();
reopen.open('GET', 'data:text/plain,second');
reopen.send();

// --- request bodies: string / ArrayBuffer / typed array / Blob / FormData ---
// data: ignores the body, so each one exercises the body branch and must
// still complete with the payload. The upload target sees loadstart and
// loadend when a body was sent.
function postBody(label, body, expectUpload) {
    var xp = new XMLHttpRequest();
    var uploadEvents = [];
    xp.upload.onloadstart = function(e) {
        uploadEvents.push('loadstart');
        assert(e.target === xp.upload, label + ': upload event target is the upload');
    };
    xp.upload.addEventListener('loadend', function() { uploadEvents.push('loadend'); });
    xp.onload = function() {
        assertEqual(xp.status, 200, label + ': POST completes');
        assertEqual(xp.responseText, 'posted', label + ': POST response');
        assertEqual(uploadEvents.join(','), expectUpload ? 'loadstart,loadend' : '', label + ': upload events');
    };
    xp.onerror = function() { assert(false, label + ': POST must not error'); };
    xp.open('POST', 'data:text/plain,posted');
    xp.send(body);
    if (expectUpload) {
        assertEqual(uploadEvents.join(','), 'loadstart', label + ': upload loadstart is synchronous');
    }
}
postBody('string', 'hello body', true);
postBody('arraybuffer', new Uint8Array([1, 2, 3]).buffer, true);
postBody('typedarray', new Uint8Array([4, 5, 6]), true);
postBody('blob', new Blob(['blob body'], { type: 'text/plain' }), true);
var fd = new FormData();
fd.append('k', 'v');
postBody('formdata', fd, true);
postBody('none', undefined, false);

// A GET drops its body: no upload events.
var xg = new XMLHttpRequest();
var getUpload = 0;
xg.upload.onloadstart = function() { getUpload++; };
xg.onload = function() { assertEqual(getUpload, 0, 'GET sends no body, so no upload events'); };
xg.open('GET', 'data:text/plain,get');
xg.send('ignored');

// --- local file ---
var fs = globalThis.__brokit_fs;
var os = globalThis.__brokit_os;
var tmpFile = os.tmpdir() + '/brokit_xhr_test_' + Date.now() + '.json';
fs.writeFileSync(tmpFile, '{"local":true}');
var xf = new XMLHttpRequest();
xf.responseType = 'json';
xf.onload = function() {
    assertEqual(xf.status, 200, 'local file status');
    assertEqual(xf.response.local, true, 'local file json body');
    var ct = xf.getResponseHeader('content-type');
    assert(ct && ct.indexOf('application/json') === 0, 'local file content-type from extension');
    fs.unlinkSync(tmpFile);
};
xf.onerror = function() { assert(false, 'local file must not error'); };
xf.open('GET', tmpFile);
xf.send();

var xm = new XMLHttpRequest();
xm.onload = function() {
    assertEqual(xm.status, 404, 'missing local file is 404');
    assertEqual(xm.responseText, '', 'missing local file has an empty body');
};
xm.open('GET', '/this_file_definitely_does_not_exist_brokit_xhr.xyz');
xm.send();

// --- blob: URL ---
var blobBytes = new Uint8Array([104, 105, 0, 255]);
var blobUrl = URL.createObjectURL(new Blob([blobBytes], { type: 'application/x-brokit' }));
var xu = new XMLHttpRequest();
xu.responseType = 'arraybuffer';
xu.onload = function() {
    var v = new Uint8Array(xu.response);
    assertEqual(v.length, 4, 'blob: URL length');
    assertEqual(v[3], 255, 'blob: URL bytes intact');
    assertEqual(xu.getResponseHeader('content-type'), 'application/x-brokit', 'blob: URL content-type is the blob type');
    URL.revokeObjectURL(blobUrl);
};
xu.onerror = function() { assert(false, 'blob: URL must not error'); };
xu.open('GET', blobUrl);
xu.send();

// --- error path: nothing listens on this port ---
var xe = new XMLHttpRequest();
var errEvents = [];
xe.onreadystatechange = function() { errEvents.push('rsc' + xe.readyState); };
xe.onerror = function(e) {
    errEvents.push('error');
    assert(e.target === xe, 'error event target');
    assertEqual(xe.readyState, 4, 'DONE inside onerror');
    assertEqual(xe.status, 0, 'status 0 on network error');
};
xe.onload = function() { assert(false, 'unreachable host must not load'); };
xe.onloadend = function() {
    errEvents.push('loadend');
    assertEqual(errEvents.join(','), 'rsc1,rsc4,error,loadend', 'network error event sequence');
};
xe.open('GET', 'http://127.0.0.1:1/no-such-port-brokit-xhr');
xe.send();

// --- timeout: whichever of the refused connection or the deadline lands
// first, exactly one terminal event fires and the request ends DONE/0.
var xto = new XMLHttpRequest();
xto.timeout = 20;
var toTerminal = [];
xto.ontimeout = function() { toTerminal.push('timeout'); };
xto.onerror = function() { toTerminal.push('error'); };
xto.onload = function() { assert(false, 'unreachable host must not load'); };
xto.onloadend = function() {
    assertEqual(toTerminal.length, 1, 'exactly one terminal event (' + toTerminal.join(',') + ')');
    assertEqual(xto.readyState, 4, 'DONE after timeout/error');
    assertEqual(xto.status, 0, 'status 0 after timeout/error');
};
xto.open('GET', 'http://127.0.0.1:1/no-such-port-brokit-xhr-timeout');
xto.send();
