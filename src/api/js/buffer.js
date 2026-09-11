(function() {
    'use strict';

    var _enc = null;
    function getEncoder() {
        if (!_enc && typeof TextEncoder !== 'undefined') _enc = new TextEncoder();
        return _enc;
    }

    function utf8Encode(str) {
        var enc = getEncoder();
        if (enc) {
            var out = enc.encode(str);
            return new Uint8Array(out.buffer, out.byteOffset, out.byteLength);
        }
        var bytes = [];
        for (var i = 0; i < str.length; i++) {
            var c = str.charCodeAt(i);
            if (c < 0x80) bytes.push(c);
            else if (c < 0x800) bytes.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
            else if (c < 0xd800 || c >= 0xe000) bytes.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
            else {
                i++;
                var c2 = str.charCodeAt(i);
                var cp = 0x10000 + (((c & 0x3ff) << 10) | (c2 & 0x3ff));
                bytes.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
            }
        }
        return new Uint8Array(bytes);
    }

    function utf8Decode(u8) {
        if (typeof TextDecoder !== 'undefined') {
            return new TextDecoder('utf-8').decode(u8);
        }
        var s = '';
        var i = 0;
        while (i < u8.length) {
            var b1 = u8[i++];
            if (b1 < 0x80) s += String.fromCharCode(b1);
            else if ((b1 >> 5) === 6) s += String.fromCharCode(((b1 & 0x1f) << 6) | (u8[i++] & 0x3f));
            else if ((b1 >> 4) === 14) s += String.fromCharCode(((b1 & 0x0f) << 12) | ((u8[i++] & 0x3f) << 6) | (u8[i++] & 0x3f));
            else if ((b1 >> 3) === 30) {
                var cp = ((b1 & 0x07) << 18) | ((u8[i++] & 0x3f) << 12) | ((u8[i++] & 0x3f) << 6) | (u8[i++] & 0x3f);
                cp -= 0x10000;
                s += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
            }
        }
        return s;
    }

    var HEX = '0123456789abcdef';
    var HEXVAL = {};
    (function() {
        for (var i = 0; i < 16; i++) {
            HEXVAL[HEX[i]] = i;
            HEXVAL[HEX[i].toUpperCase()] = i;
        }
    })();

    function hexEncode(u8) {
        var out = '';
        for (var i = 0; i < u8.length; i++) {
            var b = u8[i];
            out += HEX[(b >> 4) & 0xf] + HEX[b & 0xf];
        }
        return out;
    }

    function hexDecode(str) {
        // Stop at first invalid character / odd trailing nibble (Node behavior).
        var bytes = [];
        for (var i = 0; i + 1 < str.length + 1; i += 2) {
            var hi = HEXVAL[str[i]];
            var lo = HEXVAL[str[i + 1]];
            if (hi === undefined || lo === undefined) break;
            bytes.push((hi << 4) | lo);
        }
        return new Uint8Array(bytes);
    }

    function base64Encode(u8) {
        // btoa needs a binary (Latin1) string; build it byte-by-byte.
        var bin = '';
        var CHUNK = 0x8000;
        for (var i = 0; i < u8.length; i += CHUNK) {
            var slice = u8.subarray(i, Math.min(i + CHUNK, u8.length));
            bin += String.fromCharCode.apply(null, slice);
        }
        return globalThis.btoa(bin);
    }

    function base64Decode(str) {
        var bin = globalThis.atob(str);
        var u8 = new Uint8Array(bin.length);
        for (var i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i) & 0xff;
        return u8;
    }

    function latin1Encode(str) {
        var u8 = new Uint8Array(str.length);
        for (var i = 0; i < str.length; i++) u8[i] = str.charCodeAt(i) & 0xff;
        return u8;
    }

    function latin1Decode(u8) {
        var out = '';
        var CHUNK = 0x8000;
        for (var i = 0; i < u8.length; i += CHUNK) {
            out += String.fromCharCode.apply(null, u8.subarray(i, Math.min(i + CHUNK, u8.length)));
        }
        return out;
    }

    function asciiEncode(str) {
        var u8 = new Uint8Array(str.length);
        for (var i = 0; i < str.length; i++) u8[i] = str.charCodeAt(i) & 0x7f;
        return u8;
    }

    function normEnc(enc) {
        if (!enc) return 'utf8';
        enc = String(enc).toLowerCase();
        if (enc === 'utf-8' || enc === 'utf8') return 'utf8';
        if (enc === 'base64') return 'base64';
        if (enc === 'hex') return 'hex';
        if (enc === 'ascii') return 'ascii';
        if (enc === 'latin1' || enc === 'binary') return 'latin1';
        throw new TypeError('Unknown encoding: ' + enc);
    }

    function strToBytes(str, enc) {
        enc = normEnc(enc);
        switch (enc) {
            case 'utf8': return utf8Encode(str);
            case 'base64': return base64Decode(str);
            case 'hex': return hexDecode(str);
            case 'ascii': return asciiEncode(str);
            case 'latin1': return latin1Encode(str);
        }
    }

    function bytesToStr(u8, enc) {
        enc = normEnc(enc);
        switch (enc) {
            case 'utf8': return utf8Decode(u8);
            case 'base64': return base64Encode(u8);
            case 'hex': return hexEncode(u8);
            case 'ascii':
            case 'latin1': return latin1Decode(u8);
        }
    }

    function createBuffer(u8) {
        var proxy = new Proxy(u8, {
            get: function(target, prop) {
                if (prop === '_isBuffer') return true;
                if (prop === '_u8') return target;
                if (typeof prop === 'string' && prop in bufferPrototype) {
                    var val = bufferPrototype[prop];
                    if (typeof val === 'function') {
                        return function() {
                            return val.apply(proxy, arguments);
                        };
                    }
                    return val;
                }
                var res = target[prop];
                if (typeof res === 'function') {
                    return function() {
                        var args = [];
                        for (var i = 0; i < arguments.length; i++) {
                            var a = arguments[i];
                            args.push((a && a._u8) ? a._u8 : a);
                        }
                        var ret = res.apply(target, args);
                        if (ret instanceof Uint8Array) {
                            return createBuffer(ret);
                        }
                        return ret;
                    };
                }
                return res;
            },
            set: function(target, prop, val) {
                target[prop] = val;
                return true;
            }
        });
        return proxy;
    }

    function Buffer(arg, encodingOrOffset, length) {
        if (typeof arg === 'number') {
            return createBuffer(new Uint8Array(arg | 0));
        }
        if (arg instanceof ArrayBuffer) {
            var off = encodingOrOffset === undefined ? 0 : (encodingOrOffset | 0);
            var len = length === undefined ? (arg.byteLength - off) : (length | 0);
            return createBuffer(new Uint8Array(arg, off, len));
        }
        return Buffer.from(arg, encodingOrOffset, length);
    }

    Buffer.from = function(value, encodingOrOffset, length) {
        if (typeof value === 'string') {
            var bytes = strToBytes(value, encodingOrOffset);
            return Buffer._wrap(bytes);
        }
        if (value instanceof ArrayBuffer) {
            var off = encodingOrOffset === undefined ? 0 : (encodingOrOffset | 0);
            var len = length === undefined ? (value.byteLength - off) : (length | 0);
            return createBuffer(new Uint8Array(value, off, len));
        }
        if (ArrayBuffer.isView(value)) {
            var src = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
            var copy = new Uint8Array(src.length);
            copy.set(src);
            return createBuffer(copy);
        }
        if (value && typeof value.length === 'number') {
            var out = new Uint8Array(value.length);
            for (var i = 0; i < value.length; i++) out[i] = value[i] & 0xff;
            return createBuffer(out);
        }
        if (typeof value === 'number') {
            throw new TypeError('The "value" argument must not be of type number.');
        }
        throw new TypeError('Invalid first argument to Buffer.from');
    };

    Buffer._wrap = function(u8) {
        var b = new Uint8Array(u8.length);
        b.set(u8);
        return createBuffer(b);
    };

    Buffer.alloc = function(size, fill) {
        size = size | 0;
        var b = createBuffer(new Uint8Array(size));
        if (fill !== undefined && fill !== 0) {
            b.fill(fill);
        }
        return b;
    };

    Buffer.allocUnsafe = function(size) {
        return createBuffer(new Uint8Array(size | 0));
    };

    Buffer.isBuffer = function(x) {
        return Boolean(x && x._isBuffer);
    };

    Buffer.byteLength = function(string, encoding) {
        if (typeof string !== 'string') {
            if (ArrayBuffer.isView(string)) return string.byteLength;
            if (string instanceof ArrayBuffer) return string.byteLength;
        }
        return strToBytes(String(string), encoding).length;
    };

    Buffer.concat = function(list, totalLength) {
        if (!Array.isArray(list)) list = Array.prototype.slice.call(list);
        if (totalLength === undefined) {
            totalLength = 0;
            for (var i = 0; i < list.length; i++) totalLength += list[i].length;
        }
        var out = Buffer.alloc(totalLength);
        var offset = 0;
        for (var j = 0; j < list.length; j++) {
            var item = list[j];
            if (offset >= totalLength) break;
            var take = Math.min(item.length, totalLength - offset);
            out.set(take === item.length ? item : item.subarray(0, take), offset);
            offset += take;
        }
        return out;
    };

    Buffer.compare = function(a, b) {
        var len = Math.min(a.length, b.length);
        for (var i = 0; i < len; i++) {
            if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
        }
        if (a.length < b.length) return -1;
        if (a.length > b.length) return 1;
        return 0;
    };

    Buffer[Symbol.hasInstance] = function(inst) {
        return Boolean(inst && inst._isBuffer);
    };

    try {
        Object.defineProperty(Uint8Array, Symbol.hasInstance, {
            value: function(inst) {
                return Boolean(inst && (inst._isBuffer || Object.prototype.toString.call(inst) === '[object Uint8Array]'));
            },
            writable: true,
            configurable: true
        });
    } catch(e) {}

    var bufferPrototype = {
        constructor: Buffer,
        toString: function(encoding, start, end) {
            start = start === undefined ? 0 : (start | 0);
            end = end === undefined ? this.length : (end | 0);
            if (start < 0) start = 0;
            if (end > this.length) end = this.length;
            if (end < start) end = start;
            var view = new Uint8Array(this.buffer, this.byteOffset + start, end - start);
            return bytesToStr(view, encoding);
        },
        write: function(string, offset, length, encoding) {
            if (typeof offset === 'string') { encoding = offset; offset = 0; length = undefined; }
            else if (typeof length === 'string') { encoding = length; length = undefined; }
            offset = offset === undefined ? 0 : (offset | 0);
            var bytes = strToBytes(string, encoding);
            var max = this.length - offset;
            var n = length === undefined ? bytes.length : Math.min(length | 0, bytes.length);
            if (n > max) n = max;
            for (var i = 0; i < n; i++) this[offset + i] = bytes[i];
            return n;
        },
        slice: function(start, end) {
            return this.subarray(start, end);
        },
        subarray: function(start, end) {
            var len = this.length;
            start = start === undefined ? 0 : (start | 0);
            end = end === undefined ? len : (end | 0);
            if (start < 0) start = Math.max(len + start, 0);
            if (end < 0) end = Math.max(len + end, 0);
            if (start > len) start = len;
            if (end > len) end = len;
            if (end < start) end = start;
            return createBuffer(new Uint8Array(this.buffer, this.byteOffset + start, end - start));
        },
        equals: function(other) {
            if (this.length !== other.length) return false;
            for (var i = 0; i < this.length; i++) {
                if (this[i] !== other[i]) return false;
            }
            return true;
        },
        copy: function(target, targetStart, sourceStart, sourceEnd) {
            targetStart = targetStart === undefined ? 0 : (targetStart | 0);
            sourceStart = sourceStart === undefined ? 0 : (sourceStart | 0);
            sourceEnd = sourceEnd === undefined ? this.length : (sourceEnd | 0);
            if (sourceEnd > this.length) sourceEnd = this.length;
            var n = 0;
            for (var i = sourceStart; i < sourceEnd && (targetStart + n) < target.length; i++) {
                target[targetStart + n] = this[i];
                n++;
            }
            return n;
        },
        fill: function(value) {
            if (typeof value === 'string') {
                var bytes = strToBytes(value, 'utf8');
                if (bytes.length === 0) return this;
                for (var i = 0; i < this.length; i++) this[i] = bytes[i % bytes.length];
                return this;
            }
            var v = value & 0xff;
            for (var j = 0; j < this.length; j++) this[j] = v;
            return this;
        },
        indexOf: function(value) {
            if (typeof value === 'number') {
                for (var k = 0; k < this.length; k++) {
                    if (this[k] === (value & 0xff)) return k;
                }
                return -1;
            }
            var needle;
            if (typeof value === 'string') needle = strToBytes(value, 'utf8');
            else needle = value;
            if (needle.length === 0) return 0;
            for (var i = 0; i + needle.length <= this.length; i++) {
                var match = true;
                for (var j = 0; j < needle.length; j++) {
                    if (this[i + j] !== needle[j]) { match = false; break; }
                }
                if (match) return i;
            }
            return -1;
        },
        readUInt8: function(offset) {
            offset = offset | 0;
            return this[offset];
        },
        readUInt16LE: function(offset) {
            offset = offset | 0;
            return this[offset] | (this[offset + 1] << 8);
        },
        readUInt16BE: function(offset) {
            offset = offset | 0;
            return (this[offset] << 8) | this[offset + 1];
        },
        readUInt32LE: function(offset) {
            offset = offset | 0;
            return (this[offset] |
                (this[offset + 1] << 8) |
                (this[offset + 2] << 16) |
                (this[offset + 3] << 24)) >>> 0;
        },
        readUInt32BE: function(offset) {
            offset = offset | 0;
            return ((this[offset] << 24) |
                (this[offset + 1] << 16) |
                (this[offset + 2] << 8) |
                this[offset + 3]) >>> 0;
        },
        writeUInt8: function(value, offset) {
            offset = offset | 0;
            this[offset] = value & 0xff;
            return offset + 1;
        },
        writeUInt16LE: function(value, offset) {
            offset = offset | 0;
            this[offset] = value & 0xff;
            this[offset + 1] = (value >>> 8) & 0xff;
            return offset + 2;
        },
        writeUInt16BE: function(value, offset) {
            offset = offset | 0;
            this[offset] = (value >>> 8) & 0xff;
            this[offset + 1] = value & 0xff;
            return offset + 2;
        },
        writeUInt32LE: function(value, offset) {
            offset = offset | 0;
            this[offset] = value & 0xff;
            this[offset + 1] = (value >>> 8) & 0xff;
            this[offset + 2] = (value >>> 16) & 0xff;
            this[offset + 3] = (value >>> 24) & 0xff;
            return offset + 4;
        },
        writeUInt32BE: function(value, offset) {
            offset = offset | 0;
            this[offset] = (value >>> 24) & 0xff;
            this[offset + 1] = (value >>> 16) & 0xff;
            this[offset + 2] = (value >>> 8) & 0xff;
            this[offset + 3] = value & 0xff;
            return offset + 4;
        }
    };
    Buffer.prototype = bufferPrototype;

    globalThis.Buffer = Buffer;

    globalThis.__brokit_modules = globalThis.__brokit_modules || {};
    globalThis.__brokit_modules['buffer'] = {
        Buffer: Buffer,
        INSPECT_MAX_BYTES: 50,
        kMaxLength: 0x7fffffff
    };
})();
