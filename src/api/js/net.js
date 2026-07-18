// Node-compat `net` module — raw TCP client + server over the native
// __brokit_net_* bindings (net.cpp). Classic script attaching via globalThis
// and self-registering in __brokit_modules, like the other Node-compat
// modules (see events.js).
//
// v1 semantics (documented in bro's docs/brokit-api.js):
//   - write() is backpressure-naive: data buffers unboundedly in native code
//     and always "succeeds" (returns true). No 'drain' event.
//   - allowHalfOpen is always false (Node's default): receiving FIN auto-ends
//     the write side once pending data flushes.
//   - server.listen() binds 127.0.0.1 unless a host is given explicitly —
//     the safe default; pass '0.0.0.0' (or an interface address) to expose.
//   - DNS resolution in connect() is synchronous (instant for IPs/localhost).

(function() {
    'use strict';

    var EventEmitter = globalThis.EventEmitter;

    // ── Shared drain registry ────────────────────────────────────────────────
    // One native drain hook fans out to every JS wrapper (net Socket/Server,
    // dgram Socket) by handle id. dgram.js registers into this same registry.
    var _drains = Object.create(null);

    globalThis.__brokit_net_register = function(id, fn) { _drains[id] = fn; };
    globalThis.__brokit_net_unregister = function(id) { delete _drains[id]; };

    globalThis.__brokit_net_drain_all = function() {
        for (var key in _drains) {
            var id = +key;
            var ev;
            while (_drains[key] && (ev = globalThis.__brokit_net_poll(id)) !== null) {
                var fn = _drains[key];
                if (!fn) break;
                try { fn(ev); } catch (e) {
                    if (globalThis.console && console.error)
                        console.error('net drain error:', e && e.message ? e.message : e);
                }
                if (ev.type === 'close') break; // native handle is gone
            }
        }
    };

    function toBuffer(u8) {
        // Deliver Buffer (Node idiom) when the buffer module is installed;
        // Buffer extends Uint8Array so consumers of either shape work.
        if (typeof globalThis.Buffer === 'function' && globalThis.Buffer.from) {
            return globalThis.Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength);
        }
        return u8;
    }

    // ── Socket ───────────────────────────────────────────────────────────────
    function Socket() {
        EventEmitter.call(this);
        this._id = -1;
        this.connecting = false;
        this.destroyed = false;
        this.readyState = 'closed'; // 'opening' | 'open' | 'closed'
        this.remoteAddress = undefined;
        this.remotePort = undefined;
        this.localAddress = undefined;
        this.localPort = undefined;
        this.bytesRead = 0;
        this.bytesWritten = 0;
    }
    Socket.prototype = Object.create(EventEmitter.prototype);
    Socket.prototype.constructor = Socket;

    Socket.prototype._refreshInfo = function() {
        if (this._id < 0) return;
        var info = globalThis.__brokit_net_info(this._id);
        if (info) {
            this.localAddress = info.localAddress || undefined;
            this.localPort = info.localPort || undefined;
            this.remoteAddress = info.remoteAddress || undefined;
            this.remotePort = info.remotePort || undefined;
        }
    };

    Socket.prototype._attach = function(id, alreadyOpen) {
        var self = this;
        this._id = id;
        this.readyState = alreadyOpen ? 'open' : 'opening';
        this.connecting = !alreadyOpen;
        if (alreadyOpen) this._refreshInfo();

        globalThis.__brokit_net_register(id, function(ev) {
            if (ev.type === 'connect') {
                self.connecting = false;
                self.readyState = 'open';
                self._refreshInfo();
                self.emit('connect');
                self.emit('ready');
            } else if (ev.type === 'data') {
                self.bytesRead += ev.data.length;
                self.emit('data', toBuffer(ev.data));
            } else if (ev.type === 'end') {
                self.emit('end');
            } else if (ev.type === 'error') {
                self._lastError = new Error(ev.message);
                self.emit('error', self._lastError);
            } else if (ev.type === 'close') {
                globalThis.__brokit_net_unregister(id);
                self._id = -1;
                self.connecting = false;
                self.destroyed = true;
                self.readyState = 'closed';
                self.emit('close', !!ev.hadError);
            }
        });
    };

    Socket.prototype.connect = function(port, host, cb) {
        if (typeof port === 'object' && port !== null) {
            cb = host;
            host = port.host;
            port = port.port;
        }
        if (typeof host === 'function') { cb = host; host = undefined; }
        if (typeof cb === 'function') this.once('connect', cb);

        var id = globalThis.__brokit_net_tcp_connect(String(host || '127.0.0.1'),
                                                     port | 0);
        this._attach(id, false);
        return this;
    };

    Socket.prototype.write = function(data, cb) {
        if (this._id < 0) return false;
        var ok = globalThis.__brokit_net_write(this._id, data);
        if (ok) {
            this.bytesWritten += (typeof data === 'string')
                ? data.length // close enough for ASCII; exact count is native-side
                : (data.byteLength !== undefined ? data.byteLength : 0);
        }
        if (typeof cb === 'function') {
            var self = this;
            Promise.resolve().then(function() { cb.call(self); });
        }
        return ok; // always true while writable (backpressure-naive v1)
    };

    Socket.prototype.end = function(data, cb) {
        if (typeof data === 'function') { cb = data; data = undefined; }
        if (data !== undefined && this._id >= 0)
            globalThis.__brokit_net_write(this._id, data);
        if (typeof cb === 'function') this.once('close', cb);
        if (this._id >= 0) globalThis.__brokit_net_end(this._id);
        return this;
    };

    Socket.prototype.destroy = function() {
        if (this._id >= 0) globalThis.__brokit_net_close(this._id);
        return this;
    };

    Socket.prototype.address = function() {
        this._refreshInfo();
        if (this.localAddress === undefined) return {};
        return {
            address: this.localAddress,
            port: this.localPort,
            family: this.localAddress.indexOf(':') >= 0 ? 'IPv6' : 'IPv4'
        };
    };

    // No-op compat shims (no backpressure / TCP tuning surface in v1).
    Socket.prototype.setNoDelay = function() { return this; };
    Socket.prototype.setKeepAlive = function() { return this; };
    Socket.prototype.setTimeout = function() { return this; };
    Socket.prototype.pause = function() { return this; };
    Socket.prototype.resume = function() { return this; };
    Socket.prototype.ref = function() { return this; };
    Socket.prototype.unref = function() { return this; };

    // ── Server ───────────────────────────────────────────────────────────────
    function Server(options, connectionListener) {
        EventEmitter.call(this);
        if (typeof options === 'function') {
            connectionListener = options;
            options = {};
        }
        if (typeof connectionListener === 'function')
            this.on('connection', connectionListener);
        this._id = -1;
        this.listening = false;
        this._address = null;
    }
    Server.prototype = Object.create(EventEmitter.prototype);
    Server.prototype.constructor = Server;

    Server.prototype.listen = function(port, host, cb) {
        if (typeof port === 'object' && port !== null) {
            cb = host;
            host = port.host;
            port = port.port;
        }
        if (typeof host === 'function') { cb = host; host = undefined; }
        if (typeof cb === 'function') this.once('listening', cb);
        if (this.listening) throw new Error('Server is already listening');

        var self = this;
        // Native default is 127.0.0.1 (loopback) when host is omitted.
        this._id = (host === undefined)
            ? globalThis.__brokit_net_tcp_listen(port | 0)
            : globalThis.__brokit_net_tcp_listen(port | 0, String(host));
        this.listening = true;
        var info = globalThis.__brokit_net_info(this._id);
        this._address = info ? {
            address: info.localAddress,
            port: info.localPort,
            family: info.localAddress && info.localAddress.indexOf(':') >= 0
                ? 'IPv6' : 'IPv4'
        } : null;

        globalThis.__brokit_net_register(this._id, function(ev) {
            if (ev.type === 'accept') {
                var sock = new Socket();
                sock.server = self;
                sock._attach(ev.connId, true);
                self.emit('connection', sock);
            } else if (ev.type === 'error') {
                self.emit('error', new Error(ev.message));
            } else if (ev.type === 'close') {
                globalThis.__brokit_net_unregister(self._id);
                self._id = -1;
                self.listening = false;
                self.emit('close');
            }
        });

        // Node emits 'listening' asynchronously.
        Promise.resolve().then(function() {
            if (self.listening) self.emit('listening');
        });
        return this;
    };

    Server.prototype.close = function(cb) {
        if (typeof cb === 'function') this.once('close', cb);
        if (this._id >= 0) globalThis.__brokit_net_close(this._id);
        return this;
    };

    Server.prototype.address = function() { return this._address; };
    Server.prototype.ref = function() { return this; };
    Server.prototype.unref = function() { return this; };

    // ── Module surface ───────────────────────────────────────────────────────
    function createServer(options, connectionListener) {
        return new Server(options, connectionListener);
    }

    function connect(port, host, cb) {
        var sock = new Socket();
        return sock.connect(port, host, cb);
    }

    function isIPv4(s) {
        if (typeof s !== 'string') return false;
        var parts = s.split('.');
        if (parts.length !== 4) return false;
        for (var i = 0; i < 4; i++) {
            if (!/^\d{1,3}$/.test(parts[i])) return false;
            var n = +parts[i];
            if (n > 255) return false;
            if (parts[i].length > 1 && parts[i][0] === '0') return false;
        }
        return true;
    }

    function isIPv6(s) {
        if (typeof s !== 'string' || s.indexOf(':') < 0) return false;
        // Pragmatic check: hex groups and at most one '::'.
        if ((s.match(/::/g) || []).length > 1) return false;
        return /^[0-9a-fA-F:.]+$/.test(s);
    }

    function isIP(s) { return isIPv4(s) ? 4 : (isIPv6(s) ? 6 : 0); }

    var netModule = {
        Socket: Socket,
        Server: Server,
        createServer: createServer,
        createConnection: connect,
        connect: connect,
        isIP: isIP,
        isIPv4: isIPv4,
        isIPv6: isIPv6
    };

    globalThis.__brokit_modules = globalThis.__brokit_modules || {};
    globalThis.__brokit_modules['net'] = netModule;
})();
