// WebSocketServer — RFC 6455 server on top of the `net` module's TCP listener.
//
// The browser-side WebSocket client (websocket.js) rides libcurl's frame codec
// in C; that codec is client-only (curl cannot accept), so the server carries
// its own compact framing here in JS. Delivered connections mirror the client
// object's surface: readyState / send() / close() / onopen-onmessage-onclose-
// onerror / addEventListener / binaryType.
//
// v1 scope (documented in bro's docs/brokit-api.js):
//   - No TLS (no wss:// serving) — the client side already supports wss://.
//   - No permessage-deflate; a client offering it gets plain frames (the
//     extension is simply not acknowledged, which RFC 7692 permits).
//   - Subprotocols are not negotiated (Sec-WebSocket-Protocol is ignored).
//   - Client→server frames MUST be masked (RFC 6455 §5.1) — unmasked input
//     fails the connection with 1002. Server→client frames are never masked.
//   - Fragmented messages are reassembled; control frames interleave fine.
//   - Binds 127.0.0.1 unless an explicit host is given (safe default).

(function() {
    'use strict';

    var EventEmitter = globalThis.EventEmitter;
    var net = globalThis.__brokit_modules && globalThis.__brokit_modules['net'];

    var CONNECTING = 0, OPEN = 1, CLOSING = 2, CLOSED = 3;
    var GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
    var MAX_MESSAGE = 64 * 1024 * 1024; // 64 MB reassembled-message cap → 1009
    var MAX_HEADER = 16 * 1024;         // handshake header cap

    // ── SHA-1 (handshake accept key only — not a general-purpose hash) ──────
    function sha1(bytes) {
        var ml = bytes.length;
        var withPad = ((ml + 8) >> 6 << 6) + 64; // pad to 512-bit blocks
        var msg = new Uint8Array(withPad);
        msg.set(bytes);
        msg[ml] = 0x80;
        var bitLen = ml * 8;
        // 64-bit big-endian length; JS bitops are 32-bit so split manually.
        var hi = Math.floor(bitLen / 0x100000000);
        var dv = new DataView(msg.buffer);
        dv.setUint32(withPad - 8, hi);
        dv.setUint32(withPad - 4, bitLen >>> 0);

        var h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
            h3 = 0x10325476, h4 = 0xC3D2E1F0;
        var w = new Int32Array(80);

        for (var off = 0; off < withPad; off += 64) {
            for (var i = 0; i < 16; i++) w[i] = dv.getInt32(off + i * 4);
            for (i = 16; i < 80; i++) {
                var x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
                w[i] = (x << 1) | (x >>> 31);
            }
            var a = h0, b = h1, c = h2, d = h3, e = h4, f, k;
            for (i = 0; i < 80; i++) {
                if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
                else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC | 0; }
                else             { f = b ^ c ^ d;                   k = 0xCA62C1D6 | 0; }
                var t = (((a << 5) | (a >>> 27)) + f + e + k + w[i]) | 0;
                e = d; d = c; c = (b << 30) | (b >>> 2); b = a; a = t;
            }
            h0 = (h0 + a) | 0; h1 = (h1 + b) | 0; h2 = (h2 + c) | 0;
            h3 = (h3 + d) | 0; h4 = (h4 + e) | 0;
        }

        var out = new Uint8Array(20);
        var odv = new DataView(out.buffer);
        odv.setInt32(0, h0); odv.setInt32(4, h1); odv.setInt32(8, h2);
        odv.setInt32(12, h3); odv.setInt32(16, h4);
        return out;
    }

    function bytesToBase64(bytes) {
        var bin = '';
        for (var i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
        return btoa(bin);
    }

    function concatBytes(a, b) {
        if (!a || a.length === 0) return b;
        var out = new Uint8Array(a.length + b.length);
        out.set(a);
        out.set(b, a.length);
        return out;
    }

    // Server→client frame: FIN always set, never masked.
    function encodeFrame(opcode, payload) {
        var len = payload.length;
        var head;
        if (len < 126) {
            head = new Uint8Array(2);
            head[1] = len;
        } else if (len < 65536) {
            head = new Uint8Array(4);
            head[1] = 126;
            head[2] = len >>> 8;
            head[3] = len & 0xFF;
        } else {
            head = new Uint8Array(10);
            head[1] = 127;
            var hi = Math.floor(len / 0x100000000);
            var dv = new DataView(head.buffer);
            dv.setUint32(2, hi);
            dv.setUint32(6, len >>> 0);
        }
        head[0] = 0x80 | opcode; // FIN + opcode
        var frame = new Uint8Array(head.length + len);
        frame.set(head);
        frame.set(payload, head.length);
        return frame;
    }

    // ── Server-side connection (client-like surface) ─────────────────────────
    function ServerWebSocket(socket, request) {
        this.readyState = OPEN;
        this.binaryType = 'blob'; // matches the client: delivers Uint8Array
                                  // unless set to 'arraybuffer'
        this.protocol = '';
        this.extensions = '';
        this.url = request.url;
        this.headers = request.headers;
        this.remoteAddress = socket.remoteAddress;
        this.remotePort = socket.remotePort;

        this._socket = socket;
        this._onopen = null;    // present for surface parity; never fires
        this._onmessage = null; // (the socket arrives already OPEN)
        this._onclose = null;
        this._onerror = null;
        this._listeners = {};

        this._recvBuf = null;      // unparsed bytes
        this._fragOpcode = 0;      // in-progress fragmented message opcode
        this._fragData = null;
        this._closeSent = false;
        this._closeRcvd = false;
        this._closeCode = 0;
        this._closeReason = '';
        this._closeTimer = null;
    }

    Object.defineProperties(ServerWebSocket.prototype, {
        onopen: {
            get: function() { return this._onopen; },
            set: function(fn) { this._onopen = typeof fn === 'function' ? fn : null; }
        },
        onmessage: {
            get: function() { return this._onmessage; },
            set: function(fn) { this._onmessage = typeof fn === 'function' ? fn : null; }
        },
        onclose: {
            get: function() { return this._onclose; },
            set: function(fn) { this._onclose = typeof fn === 'function' ? fn : null; }
        },
        onerror: {
            get: function() { return this._onerror; },
            set: function(fn) { this._onerror = typeof fn === 'function' ? fn : null; }
        }
    });

    ServerWebSocket.CONNECTING = CONNECTING;
    ServerWebSocket.OPEN = OPEN;
    ServerWebSocket.CLOSING = CLOSING;
    ServerWebSocket.CLOSED = CLOSED;

    ServerWebSocket.prototype.addEventListener = function(type, listener) {
        if (typeof listener !== 'function') return;
        if (!this._listeners[type]) this._listeners[type] = [];
        this._listeners[type].push(listener);
    };

    ServerWebSocket.prototype.removeEventListener = function(type, listener) {
        var list = this._listeners[type];
        if (!list) return;
        for (var i = list.length - 1; i >= 0; i--) {
            if (list[i] === listener) { list.splice(i, 1); break; }
        }
    };

    ServerWebSocket.prototype._dispatch = function(event) {
        var list = this._listeners[event.type];
        if (list) {
            for (var i = 0; i < list.length; i++) {
                try { list[i].call(this, event); } catch (e) {}
            }
        }
    };

    ServerWebSocket.prototype.send = function(data) {
        if (this.readyState !== OPEN) return;
        var payload, opcode;
        if (data instanceof ArrayBuffer) {
            payload = new Uint8Array(data);
            opcode = 0x2;
        } else if (ArrayBuffer.isView(data)) {
            payload = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
            opcode = 0x2;
        } else {
            payload = new TextEncoder().encode(String(data));
            opcode = 0x1;
        }
        this._socket.write(encodeFrame(opcode, payload));
    };

    ServerWebSocket.prototype.close = function(code, reason) {
        if (this.readyState === CLOSING || this.readyState === CLOSED) return;
        this._startClose(code === undefined ? 1000 : code, reason || '', true);
    };

    ServerWebSocket.prototype._sendClose = function(code, reason) {
        if (this._closeSent) return;
        this._closeSent = true;
        var payload;
        if (code === 1005) {
            payload = new Uint8Array(0); // "no status" is expressed by absence
        } else {
            var rbytes = new TextEncoder().encode(reason || '');
            payload = new Uint8Array(2 + rbytes.length);
            payload[0] = (code >> 8) & 0xFF;
            payload[1] = code & 0xFF;
            payload.set(rbytes, 2);
        }
        this._socket.write(encodeFrame(0x8, payload));
    };

    ServerWebSocket.prototype._startClose = function(code, reason, weInitiate) {
        this.readyState = CLOSING;
        this._closeCode = code;
        this._closeReason = reason;
        this._sendClose(code, reason);
        if (this._closeRcvd) {
            // Handshake complete both ways — drop TCP.
            this._socket.end();
        } else if (weInitiate) {
            // Give the peer a moment to echo; then force the TCP down.
            var self = this;
            this._closeTimer = setTimeout(function() {
                self._closeTimer = null;
                self._socket.destroy();
            }, 5000);
        }
    };

    ServerWebSocket.prototype._fail = function(code, reason) {
        // Protocol failure (RFC 6455 §7.1.7 "Fail the WebSocket Connection"):
        // send a close frame, then drop the TCP immediately — do not wait for
        // the peer's echo.
        var ev = { type: 'error', message: reason };
        if (this._onerror) { try { this._onerror(ev); } catch (e) {} }
        this._dispatch(ev);
        this.readyState = CLOSING;
        this._closeCode = code;
        this._closeReason = reason;
        this._sendClose(code, reason);
        this._socket.end();
    };

    ServerWebSocket.prototype._finish = function(wasClean) {
        if (this.readyState === CLOSED) return;
        this.readyState = CLOSED;
        if (this._closeTimer !== null) {
            clearTimeout(this._closeTimer);
            this._closeTimer = null;
        }
        var ev = {
            type: 'close',
            code: this._closeCode || 1006,
            reason: this._closeReason || '',
            wasClean: !!wasClean
        };
        if (this._onclose) { try { this._onclose(ev); } catch (e) {} }
        this._dispatch(ev);
    };

    ServerWebSocket.prototype._deliver = function(opcode, data) {
        if (opcode === 0x1) { // text
            var text;
            try {
                text = new TextDecoder('utf-8', { fatal: true }).decode(data);
            } catch (e) {
                this._fail(1007, 'invalid UTF-8 in text message');
                return;
            }
            var ev = { type: 'message', data: text };
        } else { // binary
            var payload = (this.binaryType === 'arraybuffer')
                ? data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength)
                : data;
            ev = { type: 'message', data: payload };
        }
        if (this._onmessage) { try { this._onmessage(ev); } catch (e) {} }
        this._dispatch(ev);
    };

    ServerWebSocket.prototype._handleControl = function(opcode, payload) {
        if (opcode === 0x9) { // ping → pong with same payload
            if (this.readyState === OPEN)
                this._socket.write(encodeFrame(0xA, payload));
        } else if (opcode === 0xA) { // pong → ignore
        } else if (opcode === 0x8) { // close
            this._closeRcvd = true;
            var code = 1005, reason = '';
            if (payload.length >= 2) {
                code = (payload[0] << 8) | payload[1];
                if (payload.length > 2) {
                    try {
                        reason = new TextDecoder('utf-8', { fatal: true })
                            .decode(payload.subarray(2));
                    } catch (e) { reason = ''; }
                }
            } else if (payload.length === 1) {
                this._fail(1002, 'close frame with 1-byte payload');
                return;
            }
            this._closeCode = code === 1005 ? 1005 : code;
            this._closeReason = reason;
            if (!this._closeSent) {
                // Echo the close handshake, then drop TCP.
                this.readyState = CLOSING;
                this._sendClose(code, reason);
            }
            this._socket.end();
        }
    };

    // Feed raw TCP bytes; parse as many complete frames as available.
    ServerWebSocket.prototype._feed = function(chunk) {
        this._recvBuf = concatBytes(this._recvBuf, chunk);

        for (;;) {
            var buf = this._recvBuf;
            if (!buf || buf.length < 2) return;

            var b0 = buf[0], b1 = buf[1];
            var fin = (b0 & 0x80) !== 0;
            var rsv = b0 & 0x70;
            var opcode = b0 & 0x0F;
            var masked = (b1 & 0x80) !== 0;
            var len = b1 & 0x7F;
            var off = 2;

            if (rsv !== 0) {
                this._fail(1002, 'RSV bits set (no extension negotiated)');
                return;
            }
            if (!masked) {
                // RFC 6455 §5.1: a server MUST fail on unmasked client frames.
                this._fail(1002, 'client frame not masked');
                return;
            }

            if (len === 126) {
                if (buf.length < off + 2) return;
                len = (buf[off] << 8) | buf[off + 1];
                off += 2;
            } else if (len === 127) {
                if (buf.length < off + 8) return;
                var dv = new DataView(buf.buffer, buf.byteOffset + off, 8);
                var hi = dv.getUint32(0), lo = dv.getUint32(4);
                len = hi * 0x100000000 + lo;
                off += 8;
            }
            if (len > MAX_MESSAGE) {
                this._fail(1009, 'message too big');
                return;
            }

            if (buf.length < off + 4) return;
            var mask = buf.subarray(off, off + 4);
            off += 4;

            if (buf.length < off + len) return; // frame incomplete — wait

            var payload = new Uint8Array(len);
            for (var i = 0; i < len; i++)
                payload[i] = buf[off + i] ^ mask[i & 3];
            this._recvBuf = buf.length === off + len
                ? null : buf.subarray(off + len);

            if (opcode >= 0x8) { // control frame
                if (!fin || len > 125) {
                    this._fail(1002, 'invalid control frame');
                    return;
                }
                this._handleControl(opcode, payload);
                if (this.readyState === CLOSED) return;
                continue;
            }

            if (opcode === 0x0) { // continuation
                if (this._fragOpcode === 0) {
                    this._fail(1002, 'continuation frame with nothing to continue');
                    return;
                }
                this._fragData = concatBytes(this._fragData, payload);
                if (this._fragData.length > MAX_MESSAGE) {
                    this._fail(1009, 'message too big');
                    return;
                }
                if (fin) {
                    var fop = this._fragOpcode;
                    var fdata = this._fragData;
                    this._fragOpcode = 0;
                    this._fragData = null;
                    this._deliver(fop, fdata);
                }
            } else if (opcode === 0x1 || opcode === 0x2) {
                if (this._fragOpcode !== 0) {
                    this._fail(1002, 'new data frame during fragmented message');
                    return;
                }
                if (fin) {
                    this._deliver(opcode, payload);
                } else {
                    this._fragOpcode = opcode;
                    this._fragData = payload;
                }
            } else {
                this._fail(1002, 'unknown opcode ' + opcode);
                return;
            }
            if (this.readyState === CLOSED) return;
        }
    };

    // ── WebSocketServer ──────────────────────────────────────────────────────
    function WebSocketServer(options) {
        if (!(this instanceof WebSocketServer))
            throw new TypeError("Failed to construct 'WebSocketServer': use 'new'");
        if (!net)
            throw new Error('WebSocketServer: net module not installed');
        EventEmitter.call(this);
        options = options || {};
        if (typeof options.port !== 'number')
            throw new TypeError('WebSocketServer: options.port (number) required');

        var self = this;
        this.clients = [];

        this._server = net.createServer(function(socket) {
            self._onTcpConnection(socket);
        });
        this._server.on('error', function(err) { self.emit('error', err); });
        this._server.on('close', function() { self.emit('close'); });
        this._server.on('listening', function() { self.emit('listening'); });
        // net's listen() already defaults to 127.0.0.1 when host is omitted.
        if (options.host === undefined) this._server.listen(options.port);
        else this._server.listen(options.port, options.host);
    }
    WebSocketServer.prototype = Object.create(EventEmitter.prototype);
    WebSocketServer.prototype.constructor = WebSocketServer;

    WebSocketServer.prototype.address = function() {
        return this._server.address();
    };

    WebSocketServer.prototype.close = function(cb) {
        if (typeof cb === 'function') this._server.once('close', cb);
        // 1001 "going away" to every live client, then drop the listener.
        for (var i = 0; i < this.clients.length; i++) {
            var ws = this.clients[i];
            if (ws.readyState === OPEN) ws.close(1001, 'server shutting down');
        }
        this._server.close();
        return this;
    };

    WebSocketServer.prototype._onTcpConnection = function(socket) {
        var self = this;
        var headerBuf = null;
        var ws = null;

        socket.on('data', function(chunk) {
            var u8 = (chunk instanceof Uint8Array)
                ? chunk : new Uint8Array(chunk);
            if (ws) { ws._feed(u8); return; }

            headerBuf = concatBytes(headerBuf, u8);
            if (headerBuf.length > MAX_HEADER) {
                socket.destroy();
                return;
            }
            // Scan for end of HTTP header.
            var endIdx = -1;
            for (var i = 3; i < headerBuf.length; i++) {
                if (headerBuf[i - 3] === 13 && headerBuf[i - 2] === 10 &&
                    headerBuf[i - 1] === 13 && headerBuf[i] === 10) {
                    endIdx = i + 1;
                    break;
                }
            }
            if (endIdx < 0) return; // header incomplete

            var headerText = new TextDecoder().decode(headerBuf.subarray(0, endIdx));
            var rest = endIdx < headerBuf.length ? headerBuf.subarray(endIdx) : null;
            headerBuf = null;

            var request = self._parseHandshake(headerText);
            if (!request) {
                socket.write('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
                socket.end();
                return;
            }
            if (request.badVersion) {
                socket.write('HTTP/1.1 426 Upgrade Required\r\n' +
                             'Sec-WebSocket-Version: 13\r\nConnection: close\r\n\r\n');
                socket.end();
                return;
            }

            var keyBytes = new TextEncoder().encode(request.key + GUID);
            var accept = bytesToBase64(sha1(keyBytes));
            socket.write('HTTP/1.1 101 Switching Protocols\r\n' +
                         'Upgrade: websocket\r\n' +
                         'Connection: Upgrade\r\n' +
                         'Sec-WebSocket-Accept: ' + accept + '\r\n\r\n');

            ws = new ServerWebSocket(socket, request);
            self.clients.push(ws);
            socket.on('close', function() {
                var idx = self.clients.indexOf(ws);
                if (idx >= 0) self.clients.splice(idx, 1);
                // wasClean iff the close handshake completed both directions.
                ws._finish(ws._closeSent && ws._closeRcvd);
            });
            self.emit('connection', ws, request);
            if (rest && rest.length) ws._feed(rest);
        });

        socket.on('error', function() {}); // close(hadError) covers reporting
        socket.on('close', function() {
            // Pre-upgrade drop: nothing to clean; post-upgrade handled above.
        });
    };

    WebSocketServer.prototype._parseHandshake = function(text) {
        var lines = text.split('\r\n');
        var reqLine = lines[0].split(' ');
        if (reqLine.length < 3 || reqLine[0] !== 'GET') return null;

        var headers = {};
        for (var i = 1; i < lines.length; i++) {
            var line = lines[i];
            if (!line) continue;
            var colon = line.indexOf(':');
            if (colon < 0) return null;
            headers[line.slice(0, colon).trim().toLowerCase()] =
                line.slice(colon + 1).trim();
        }

        var upgrade = (headers['upgrade'] || '').toLowerCase();
        var connection = (headers['connection'] || '').toLowerCase();
        var key = headers['sec-websocket-key'];
        var version = headers['sec-websocket-version'];

        if (upgrade !== 'websocket') return null;
        if (connection.split(',').map(function(s) { return s.trim(); })
                .indexOf('upgrade') < 0) return null;
        if (!key) return null;
        if (version !== '13') return { badVersion: true };

        return { url: reqLine[1], headers: headers, key: key };
    };

    // Expose — global (like WebSocket) and as a require()-able module.
    globalThis.WebSocketServer = WebSocketServer;
    globalThis.__brokit_modules = globalThis.__brokit_modules || {};
    globalThis.__brokit_modules['websocket-server'] = {
        WebSocketServer: WebSocketServer
    };
})();
