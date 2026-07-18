// Node-compat `dgram` module — UDP sockets over the native __brokit_net_*
// bindings (net.cpp). Uses the drain registry installed by net.js.
//
// v1 semantics (documented in bro's docs/brokit-api.js):
//   - 'udp4' and 'udp6' are supported (same native code path, AF_INET/AF_INET6).
//   - bind() defaults to 127.0.0.1 (::1 for udp6) unless a host is given —
//     the safe default; pass '0.0.0.0' to receive from the network.
//   - Broadcast is opt-in via setBroadcast(true).
//   - Multicast (addMembership etc.) is deferred — not implemented.
//   - send() is fire-and-forget: a full kernel buffer drops the datagram
//     (faithful UDP), and the optional callback reports only local errors.

(function() {
    'use strict';

    var EventEmitter = globalThis.EventEmitter;

    function toBuffer(u8) {
        if (typeof globalThis.Buffer === 'function' && globalThis.Buffer.from) {
            return globalThis.Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength);
        }
        return u8;
    }

    function UdpSocket(type) {
        EventEmitter.call(this);
        if (type !== 'udp4' && type !== 'udp6') {
            throw new Error("createSocket: type must be 'udp4' or 'udp6'");
        }
        this.type = type;
        this._id = globalThis.__brokit_net_udp_open(type === 'udp6');
        this._bound = false;
        this._closed = false;

        var self = this;
        globalThis.__brokit_net_register(this._id, function(ev) {
            if (ev.type === 'message') {
                self.emit('message', toBuffer(ev.data), {
                    address: ev.address,
                    family: ev.family,
                    port: ev.port,
                    size: ev.data.length
                });
            } else if (ev.type === 'error') {
                self.emit('error', new Error(ev.message));
            } else if (ev.type === 'close') {
                globalThis.__brokit_net_unregister(self._id);
                self._id = -1;
                self._closed = true;
                self.emit('close');
            }
        });
    }
    UdpSocket.prototype = Object.create(EventEmitter.prototype);
    UdpSocket.prototype.constructor = UdpSocket;

    UdpSocket.prototype.bind = function(port, host, cb) {
        if (typeof port === 'object' && port !== null) {
            cb = host;
            host = port.address;
            port = port.port;
        }
        if (typeof port === 'function') { cb = port; port = 0; host = undefined; }
        if (typeof host === 'function') { cb = host; host = undefined; }
        if (this._closed) throw new Error('Socket is closed');
        if (this._bound) throw new Error('Socket is already bound');
        if (typeof cb === 'function') this.once('listening', cb);

        // Native default binds loopback when host is omitted.
        if (host === undefined) {
            globalThis.__brokit_net_udp_bind(this._id, port | 0);
        } else {
            globalThis.__brokit_net_udp_bind(this._id, port | 0, String(host));
        }
        this._bound = true;

        var self = this;
        Promise.resolve().then(function() {
            if (!self._closed) self.emit('listening');
        });
        return this;
    };

    UdpSocket.prototype.send = function(data, port, host, cb) {
        if (typeof host === 'function') { cb = host; host = undefined; }
        if (this._closed || this._id < 0) {
            if (typeof cb === 'function') cb(new Error('Socket is closed'));
            return;
        }
        if (host === undefined) host = this.type === 'udp6' ? '::1' : '127.0.0.1';
        var ok = globalThis.__brokit_net_udp_send(this._id, data, port | 0,
                                                  String(host));
        if (typeof cb === 'function') {
            Promise.resolve().then(function() {
                cb(ok ? null : new Error('send failed'));
            });
        }
    };

    UdpSocket.prototype.close = function(cb) {
        if (typeof cb === 'function') this.once('close', cb);
        if (this._id >= 0) globalThis.__brokit_net_close(this._id);
        return this;
    };

    UdpSocket.prototype.address = function() {
        if (this._id < 0) return {};
        var info = globalThis.__brokit_net_info(this._id);
        if (!info) return {};
        return {
            address: info.localAddress,
            port: info.localPort,
            family: this.type === 'udp6' ? 'IPv6' : 'IPv4'
        };
    };

    UdpSocket.prototype.setBroadcast = function(flag) {
        if (this._id >= 0)
            globalThis.__brokit_net_set_broadcast(this._id, !!flag);
    };

    UdpSocket.prototype.ref = function() { return this; };
    UdpSocket.prototype.unref = function() { return this; };

    function createSocket(options, messageListener) {
        var type = typeof options === 'string' ? options
                                               : (options && options.type);
        var sock = new UdpSocket(type);
        if (typeof messageListener === 'function')
            sock.on('message', messageListener);
        return sock;
    }

    var dgramModule = {
        createSocket: createSocket,
        Socket: UdpSocket
    };

    globalThis.__brokit_modules = globalThis.__brokit_modules || {};
    globalThis.__brokit_modules['dgram'] = dgramModule;
})();
