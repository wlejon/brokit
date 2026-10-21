// Node-compatible `http` module — HTTP/1.1 server + client built on llhttp
// (__brokit_http_parser_*) and the `net` module's non-blocking TCP sockets.
//
// Security default: like net.Server, server.listen(port) binds 127.0.0.1
// unless an explicit host ('0.0.0.0', interface IP, etc.) is passed.

(function() {
    'use strict';

    var EventEmitter = globalThis.EventEmitter;
    var net = globalThis.__brokit_modules && globalThis.__brokit_modules['net'];

    var STATUS_CODES = {
        100: 'Continue',
        101: 'Switching Protocols',
        200: 'OK',
        201: 'Created',
        202: 'Accepted',
        204: 'No Content',
        206: 'Partial Content',
        301: 'Moved Permanently',
        302: 'Found',
        304: 'Not Modified',
        307: 'Temporary Redirect',
        308: 'Permanent Redirect',
        400: 'Bad Request',
        401: 'Unauthorized',
        403: 'Forbidden',
        404: 'Not Found',
        405: 'Method Not Allowed',
        408: 'Request Timeout',
        409: 'Conflict',
        410: 'Gone',
        413: 'Payload Too Large',
        415: 'Unsupported Media Type',
        422: 'Unprocessable Entity',
        426: 'Upgrade Required',
        429: 'Too Many Requests',
        500: 'Internal Server Error',
        501: 'Not Implemented',
        502: 'Bad Gateway',
        503: 'Service Unavailable',
        504: 'Gateway Timeout'
    };

    var METHODS = [
        'ACL', 'BIND', 'CHECKOUT', 'CONNECT', 'COPY', 'DELETE', 'GET', 'HEAD',
        'LINK', 'LOCK', 'M-SEARCH', 'MERGE', 'MKACTIVITY', 'MKCALENDAR', 'MKCOL',
        'MOVE', 'NOTIFY', 'OPTIONS', 'PATCH', 'POST', 'PROPFIND', 'PROPPATCH',
        'PURGE', 'PUT', 'REBIND', 'REPORT', 'SEARCH', 'SOURCE', 'SUBSCRIBE',
        'TRACE', 'UNBIND', 'UNLINK', 'UNLOCK', 'UNSUBSCRIBE'
    ];

    function toBuffer(u8) {
        if (typeof globalThis.Buffer === 'function' && globalThis.Buffer.from) {
            return globalThis.Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength);
        }
        return u8;
    }

    function toBytes(chunk, encoding) {
        if (chunk instanceof Uint8Array) return chunk;
        if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk);
        if (ArrayBuffer.isView(chunk)) {
            return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
        }
        if (typeof globalThis.Buffer === 'function' && encoding && encoding !== 'utf8' && encoding !== 'utf-8') {
            return globalThis.Buffer.from(String(chunk), encoding);
        }
        return new TextEncoder().encode(String(chunk));
    }

    function buildHeadersMap(rawHeaders) {
        var headers = {};
        if (!rawHeaders) return headers;
        for (var i = 0; i + 1 < rawHeaders.length; i += 2) {
            var k = String(rawHeaders[i]).toLowerCase();
            var v = String(rawHeaders[i + 1]);
            if (k === 'set-cookie') {
                if (!headers[k]) headers[k] = [];
                headers[k].push(v);
            } else if (headers[k] !== undefined) {
                headers[k] += ', ' + v;
            } else {
                headers[k] = v;
            }
        }
        return headers;
    }

    function statusReason(code) {
        if (STATUS_CODES[code]) return STATUS_CODES[code];
        if (typeof globalThis.__brokit_http_status_text === 'function') {
            return globalThis.__brokit_http_status_text(code | 0) || 'OK';
        }
        return 'OK';
    }

    // ── IncomingMessage ──────────────────────────────────────────────────────
    function IncomingMessage(socket, hdrEvent) {
        EventEmitter.call(this);
        this.socket = socket;
        this.connection = socket;
        this.method = hdrEvent.method || null;
        this.url = hdrEvent.url || '';
        this.statusCode = hdrEvent.statusCode || null;
        this.statusMessage = hdrEvent.statusMessage || (this.statusCode ? statusReason(this.statusCode) : null);
        this.httpVersionMajor = hdrEvent.versionMajor || 1;
        this.httpVersionMinor = hdrEvent.versionMinor || 1;
        this.httpVersion = this.httpVersionMajor + '.' + this.httpVersionMinor;
        this.rawHeaders = hdrEvent.rawHeaders || [];
        this.headers = buildHeadersMap(this.rawHeaders);
        this.upgrade = !!hdrEvent.upgrade;
        this.shouldKeepAlive = !!hdrEvent.shouldKeepAlive;
        this.complete = false;
        this.aborted = false;
        this._encoding = null;
    }
    IncomingMessage.prototype = Object.create(EventEmitter.prototype);
    IncomingMessage.prototype.constructor = IncomingMessage;

    IncomingMessage.prototype.setEncoding = function(enc) {
        this._encoding = enc || 'utf8';
        return this;
    };

    IncomingMessage.prototype._pushBody = function(u8) {
        if (this._encoding) {
            var str = new TextDecoder(this._encoding === 'utf8' ? 'utf-8' : this._encoding).decode(u8);
            this.emit('data', str);
        } else {
            this.emit('data', toBuffer(u8));
        }
    };

    IncomingMessage.prototype._complete = function() {
        if (this.complete) return;
        this.complete = true;
        this.emit('end');
        this.emit('close');
    };

    IncomingMessage.prototype.destroy = function(err) {
        if (err) this.emit('error', err);
        if (this.socket) this.socket.destroy();
        return this;
    };

    IncomingMessage.prototype.pause = function() { return this; };
    IncomingMessage.prototype.resume = function() { return this; };

    // ── ServerResponse ───────────────────────────────────────────────────────
    function ServerResponse(req, socket) {
        EventEmitter.call(this);
        this.req = req;
        this.socket = socket;
        this.connection = socket;
        this.statusCode = 200;
        this.statusMessage = undefined;
        this.headersSent = false;
        this.writableEnded = false;
        this.finished = false;
        this.sendDate = true;
        this._headers = {};     // lowerKey -> { name, value }
        this._isHead = (req && req.method === 'HEAD');
        this._chunked = false;
        this._keepAlive = req ? !!req.shouldKeepAlive : true;
    }
    ServerResponse.prototype = Object.create(EventEmitter.prototype);
    ServerResponse.prototype.constructor = ServerResponse;

    ServerResponse.prototype.setHeader = function(name, value) {
        if (this.headersSent) throw new Error('Cannot set headers after they are sent to the client');
        var key = String(name).toLowerCase();
        this._headers[key] = { name: String(name), value: value };
        return this;
    };

    ServerResponse.prototype.getHeader = function(name) {
        var entry = this._headers[String(name).toLowerCase()];
        return entry ? entry.value : undefined;
    };

    ServerResponse.prototype.getHeaders = function() {
        var out = {};
        for (var k in this._headers) {
            out[k] = this._headers[k].value;
        }
        return out;
    };

    ServerResponse.prototype.getHeaderNames = function() {
        return Object.keys(this._headers);
    };

    ServerResponse.prototype.hasHeader = function(name) {
        return Object.prototype.hasOwnProperty.call(this._headers, String(name).toLowerCase());
    };

    ServerResponse.prototype.removeHeader = function(name) {
        if (this.headersSent) throw new Error('Cannot remove headers after they are sent to the client');
        delete this._headers[String(name).toLowerCase()];
    };

    ServerResponse.prototype.writeHead = function(statusCode, statusMessage, headers) {
        if (typeof statusMessage === 'object' && statusMessage !== null) {
            headers = statusMessage;
            statusMessage = undefined;
        }
        this.statusCode = statusCode | 0;
        if (statusMessage !== undefined) this.statusMessage = String(statusMessage);
        if (headers) {
            var keys = Object.keys(headers);
            for (var i = 0; i < keys.length; i++) {
                this.setHeader(keys[i], headers[keys[i]]);
            }
        }
        return this;
    };

    ServerResponse.prototype._serializeHeaders = function(knownBodyLength) {
        var code = this.statusCode | 0;
        var reason = this.statusMessage !== undefined ? this.statusMessage : statusReason(code);
        var lines = ['HTTP/1.1 ' + code + ' ' + reason];

        var hasContentLen = this.hasHeader('content-length');
        var hasTransferEnc = this.hasHeader('transfer-encoding');
        var hasConnection = this.hasHeader('connection');
        var hasDate = this.hasHeader('date');

        if (this.sendDate && !hasDate) {
            lines.push('Date: ' + new Date().toUTCString());
        }

        var noBodyStatus = (code === 204 || code === 304 || (code >= 100 && code < 200));
        if (!noBodyStatus && !hasContentLen && !hasTransferEnc) {
            if (knownBodyLength !== null && knownBodyLength !== undefined) {
                lines.push('Content-Length: ' + knownBodyLength);
                this._chunked = false;
            } else {
                lines.push('Transfer-Encoding: chunked');
                this._chunked = true;
            }
        } else if (hasTransferEnc) {
            var te = String(this.getHeader('transfer-encoding')).toLowerCase();
            this._chunked = te.indexOf('chunked') >= 0;
        }

        if (!hasConnection) {
            lines.push('Connection: ' + (this._keepAlive ? 'keep-alive' : 'close'));
        } else {
            var connVal = String(this.getHeader('connection')).toLowerCase();
            if (connVal.indexOf('close') >= 0) this._keepAlive = false;
        }

        for (var k in this._headers) {
            var entry = this._headers[k];
            if (Array.isArray(entry.value)) {
                for (var j = 0; j < entry.value.length; j++) {
                    lines.push(entry.name + ': ' + entry.value[j]);
                }
            } else {
                lines.push(entry.name + ': ' + entry.value);
            }
        }

        this.headersSent = true;
        return lines.join('\r\n') + '\r\n\r\n';
    };

    ServerResponse.prototype.flushHeaders = function() {
        if (this.headersSent) return;
        var head = this._serializeHeaders(null);
        if (this.socket) this.socket.write(head);
    };

    ServerResponse.prototype.write = function(chunk, encoding, cb) {
        if (typeof encoding === 'function') { cb = encoding; encoding = undefined; }
        if (this.writableEnded) throw new Error('write after end');
        if (!this.headersSent) {
            var head = this._serializeHeaders(null);
            if (this.socket) this.socket.write(head);
        }
        if (chunk !== undefined && chunk !== null && !this._isHead) {
            var bytes = toBytes(chunk, encoding);
            if (bytes.length > 0 && this.socket) {
                if (this._chunked) {
                    this.socket.write(bytes.length.toString(16) + '\r\n');
                    this.socket.write(bytes);
                    this.socket.write('\r\n');
                } else {
                    this.socket.write(bytes);
                }
            }
        }
        if (typeof cb === 'function') {
            var self = this;
            Promise.resolve().then(function() { cb.call(self); });
        }
        return true;
    };

    ServerResponse.prototype.end = function(chunk, encoding, cb) {
        if (typeof chunk === 'function') { cb = chunk; chunk = undefined; encoding = undefined; }
        else if (typeof encoding === 'function') { cb = encoding; encoding = undefined; }
        if (this.writableEnded) return this;

        var bodyBytes = null;
        if (chunk !== undefined && chunk !== null) {
            bodyBytes = toBytes(chunk, encoding);
        }

        if (!this.headersSent) {
            var len = bodyBytes ? bodyBytes.length : 0;
            var head = this._serializeHeaders(len);
            if (this.socket) {
                this.socket.write(head);
                if (bodyBytes && bodyBytes.length > 0 && !this._isHead) {
                    this.socket.write(bodyBytes);
                }
            }
        } else {
            if (bodyBytes && bodyBytes.length > 0 && !this._isHead && this.socket) {
                if (this._chunked) {
                    this.socket.write(bodyBytes.length.toString(16) + '\r\n');
                    this.socket.write(bodyBytes);
                    this.socket.write('\r\n');
                } else {
                    this.socket.write(bodyBytes);
                }
            }
            if (this._chunked && !this._isHead && this.socket) {
                this.socket.write('0\r\n\r\n');
            }
        }

        this.writableEnded = true;
        this.finished = true;

        var self = this;
        if (typeof cb === 'function') this.once('finish', cb);
        this.emit('finish');
        this.emit('close');

        if (!this._keepAlive && this.socket) {
            this.socket.end();
        }
        return this;
    };

    // ── Server ───────────────────────────────────────────────────────────────
    function Server(options, requestListener) {
        if (!(this instanceof Server)) return new Server(options, requestListener);
        if (typeof options === 'function') {
            requestListener = options;
            options = {};
        }
        net.Server.call(this, options);
        this._sockets = [];
        if (typeof requestListener === 'function') {
            this.on('request', requestListener);
        }
        var self = this;
        this.on('connection', function(socket) {
            self._sockets.push(socket);
            socket.on('close', function() {
                var idx = self._sockets.indexOf(socket);
                if (idx >= 0) self._sockets.splice(idx, 1);
            });
            self._attachHttpSocket(socket);
        });
    }
    Server.prototype = Object.create(net.Server.prototype);
    Server.prototype.constructor = Server;

    Server.prototype.closeAllConnections = function() {
        var list = this._sockets.slice();
        this._sockets.length = 0;
        for (var i = 0; i < list.length; i++) {
            try { list[i].destroy(); } catch (e) {}
        }
    };

    Server.prototype.close = function(cb) {
        this.closeAllConnections();
        return net.Server.prototype.close.call(this, cb);
    };

    Server.prototype._attachHttpSocket = function(socket) {
        var self = this;
        var parserId = globalThis.__brokit_http_parser_create(false);
        var currentReq = null;
        var upgraded = false;

        function cleanupParser() {
            if (parserId > 0) {
                globalThis.__brokit_http_parser_destroy(parserId);
                parserId = -1;
            }
        }

        function handleEvents(events) {
            for (var i = 0; i < events.length; i++) {
                var ev = events[i];
                if (ev.type === 'headers') {
                    currentReq = new IncomingMessage(socket, ev);
                    if (!ev.upgrade) {
                        var res = new ServerResponse(currentReq, socket);
                        self.emit('request', currentReq, res);
                    }
                } else if (ev.type === 'body') {
                    if (currentReq && !currentReq.upgrade) {
                        currentReq._pushBody(ev.data);
                    }
                } else if (ev.type === 'complete') {
                    if (currentReq && !currentReq.upgrade) {
                        var doneReq = currentReq;
                        currentReq = null;
                        doneReq._complete();
                    }
                } else if (ev.type === 'upgrade') {
                    upgraded = true;
                    var upReq = currentReq;
                    currentReq = null;
                    cleanupParser();
                    socket.removeListener('data', onData);
                    socket.removeListener('end', onEnd);
                    var headBuf = toBuffer(ev.head || new Uint8Array(0));
                    if (self.listenerCount('upgrade') > 0 && upReq) {
                        self.emit('upgrade', upReq, socket, headBuf);
                    } else {
                        socket.destroy();
                    }
                    return;
                } else if (ev.type === 'error') {
                    var err = new Error('HTTP parse error (' + ev.code + '): ' + ev.reason);
                    err.code = ev.code;
                    if (self.listenerCount('clientError') > 0) {
                        self.emit('clientError', err, socket);
                    } else {
                        socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
                    }
                    cleanupParser();
                    return;
                }
            }
        }

        function onData(chunk) {
            if (upgraded || parserId < 0) return;
            var events = globalThis.__brokit_http_parser_execute(parserId, chunk);
            handleEvents(events);
        }

        function onEnd() {
            if (upgraded || parserId < 0) return;
            var events = globalThis.__brokit_http_parser_finish(parserId);
            handleEvents(events);
            cleanupParser();
        }

        socket.on('data', onData);
        socket.on('end', onEnd);
        socket.on('close', function() {
            if (currentReq && !currentReq.complete) {
                currentReq.aborted = true;
                currentReq.emit('aborted');
                currentReq.emit('close');
                currentReq = null;
            }
            cleanupParser();
        });
    };

    // ── ClientRequest (http.request / http.get) ──────────────────────────────
    function ClientRequest(urlOrOpts, cb) {
        EventEmitter.call(this);
        var opts = {};
        if (typeof urlOrOpts === 'string' || (typeof URL !== 'undefined' && urlOrOpts instanceof URL)) {
            var u = typeof urlOrOpts === 'string' ? new URL(urlOrOpts) : urlOrOpts;
            opts.hostname = u.hostname || '127.0.0.1';
            opts.port = u.port ? (+u.port) : 80;
            opts.path = (u.pathname || '/') + (u.search || '');
            opts.method = 'GET';
        } else if (urlOrOpts && typeof urlOrOpts === 'object') {
            opts = Object.assign({}, urlOrOpts);
        }
        this.method = (opts.method || 'GET').toUpperCase();
        this.host = opts.hostname || opts.host || '127.0.0.1';
        this.port = opts.port ? (+opts.port) : 80;
        this.path = opts.path || '/';
        this._headers = {};
        this.setHeader('Host', this.host + (this.port !== 80 ? ':' + this.port : ''));
        this.setHeader('Connection', 'close');
        if (opts.headers) {
            var ks = Object.keys(opts.headers);
            for (var i = 0; i < ks.length; i++) {
                this.setHeader(ks[i], opts.headers[ks[i]]);
            }
        }
        if (typeof cb === 'function') this.once('response', cb);

        this._bodyChunks = [];
        this._ended = false;
        this.socket = null;
    }
    ClientRequest.prototype = Object.create(EventEmitter.prototype);
    ClientRequest.prototype.constructor = ClientRequest;

    ClientRequest.prototype.setHeader = function(name, value) {
        this._headers[String(name).toLowerCase()] = { name: String(name), value: value };
        return this;
    };

    ClientRequest.prototype.getHeader = function(name) {
        var e = this._headers[String(name).toLowerCase()];
        return e ? e.value : undefined;
    };

    ClientRequest.prototype.removeHeader = function(name) {
        delete this._headers[String(name).toLowerCase()];
    };

    ClientRequest.prototype.write = function(chunk, encoding) {
        if (chunk !== undefined && chunk !== null) {
            this._bodyChunks.push(toBytes(chunk, encoding));
        }
        return true;
    };

    ClientRequest.prototype.end = function(chunk, encoding, cb) {
        if (typeof chunk === 'function') { cb = chunk; chunk = undefined; }
        else if (typeof encoding === 'function') { cb = encoding; encoding = undefined; }
        if (this._ended) return this;
        this._ended = true;
        if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
        if (typeof cb === 'function') this.once('finish', cb);

        var totalLen = 0;
        for (var i = 0; i < this._bodyChunks.length; i++) totalLen += this._bodyChunks[i].length;
        if ((totalLen > 0 || this.method === 'POST' || this.method === 'PUT' || this.method === 'PATCH') &&
            !this._headers['content-length']) {
            this.setHeader('Content-Length', String(totalLen));
        }

        var lines = [this.method + ' ' + this.path + ' HTTP/1.1'];
        for (var k in this._headers) {
            lines.push(this._headers[k].name + ': ' + this._headers[k].value);
        }
        var headStr = lines.join('\r\n') + '\r\n\r\n';

        var self = this;
        var sock = net.connect(this.port, this.host, function() {
            sock.write(headStr);
            for (var j = 0; j < self._bodyChunks.length; j++) {
                sock.write(self._bodyChunks[j]);
            }
            self.emit('finish');
        });
        this.socket = sock;

        var parserId = globalThis.__brokit_http_parser_create(true, this.method === 'HEAD');
        var currentRes = null;

        function cleanup() {
            if (parserId > 0) {
                globalThis.__brokit_http_parser_destroy(parserId);
                parserId = -1;
            }
        }

        function handleEvents(events) {
            for (var n = 0; n < events.length; n++) {
                var ev = events[n];
                if (ev.type === 'headers') {
                    currentRes = new IncomingMessage(sock, ev);
                    self.emit('response', currentRes);
                } else if (ev.type === 'body') {
                    if (currentRes) currentRes._pushBody(ev.data);
                } else if (ev.type === 'complete') {
                    if (currentRes) {
                        var r = currentRes;
                        currentRes = null;
                        r._complete();
                    }
                    cleanup();
                } else if (ev.type === 'error') {
                    cleanup();
                    self.emit('error', new Error('HTTP response parse error: ' + ev.reason));
                }
            }
        }

        sock.on('data', function(c) {
            if (parserId < 0) return;
            handleEvents(globalThis.__brokit_http_parser_execute(parserId, c));
        });
        sock.on('end', function() {
            if (parserId < 0) return;
            handleEvents(globalThis.__brokit_http_parser_finish(parserId));
            cleanup();
        });
        sock.on('error', function(err) {
            cleanup();
            self.emit('error', err);
        });
        sock.on('close', function() {
            if (currentRes && !currentRes.complete) {
                currentRes._complete();
                currentRes = null;
            }
            cleanup();
        });

        return this;
    };

    ClientRequest.prototype.destroy = function(err) {
        if (this.socket) this.socket.destroy();
        if (err) this.emit('error', err);
        return this;
    };

    function createServer(options, requestListener) {
        return new Server(options, requestListener);
    }

    function request(urlOrOpts, optsOrCb, maybeCb) {
        if (typeof optsOrCb === 'object' && optsOrCb !== null &&
            (typeof urlOrOpts === 'string' || (typeof URL !== 'undefined' && urlOrOpts instanceof URL))) {
            var u = typeof urlOrOpts === 'string' ? new URL(urlOrOpts) : urlOrOpts;
            var merged = Object.assign({
                hostname: u.hostname || '127.0.0.1',
                port: u.port ? (+u.port) : 80,
                path: (u.pathname || '/') + (u.search || '')
            }, optsOrCb);
            return new ClientRequest(merged, maybeCb);
        }
        return new ClientRequest(urlOrOpts, optsOrCb);
    }

    function get(urlOrOpts, optsOrCb, maybeCb) {
        var req = request(urlOrOpts, optsOrCb, maybeCb);
        req.end();
        return req;
    }

    var httpModule = {
        Server: Server,
        IncomingMessage: IncomingMessage,
        ServerResponse: ServerResponse,
        ClientRequest: ClientRequest,
        STATUS_CODES: STATUS_CODES,
        METHODS: METHODS,
        createServer: createServer,
        request: request,
        get: get
    };

    globalThis.__brokit_modules = globalThis.__brokit_modules || {};
    globalThis.__brokit_modules['http'] = httpModule;
})();
