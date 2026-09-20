(function() {
    globalThis.TextEncoder = function TextEncoder() {
        this.encoding = 'utf-8';
    };
    TextEncoder.prototype.encode = function(str) {
        return (typeof __brokit_textencoder_encode === 'function' ? __brokit_textencoder_encode : globalThis.__brokit_textencoder_encode)(str || '');
    };
    TextEncoder.prototype.encodeInto = function(str, dest) {
        str = String(str || '');
        var read = 0;
        var written = 0;
        var strLen = str.length;
        var destLen = (dest && typeof dest.length === 'number') ? dest.length : 0;

        for (var i = 0; i < strLen;) {
            var c1 = str.charCodeAt(i);
            var cp = c1;
            var units = 1;

            if (c1 >= 0xD800 && c1 <= 0xDBFF) {
                if (i + 1 < strLen) {
                    var c2 = str.charCodeAt(i + 1);
                    if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                        cp = ((c1 - 0xD800) << 10) + (c2 - 0xDC00) + 0x10000;
                        units = 2;
                    } else {
                        cp = 0xFFFD;
                    }
                } else {
                    cp = 0xFFFD;
                }
            } else if (c1 >= 0xDC00 && c1 <= 0xDFFF) {
                cp = 0xFFFD;
            }

            var b0, b1, b2, b3;
            var needed;
            if (cp <= 0x7F) {
                needed = 1;
                b0 = cp;
            } else if (cp <= 0x7FF) {
                needed = 2;
                b0 = 0xC0 | (cp >> 6);
                b1 = 0x80 | (cp & 0x3F);
            } else if (cp <= 0xFFFF) {
                needed = 3;
                b0 = 0xE0 | (cp >> 12);
                b1 = 0x80 | ((cp >> 6) & 0x3F);
                b2 = 0x80 | (cp & 0x3F);
            } else {
                needed = 4;
                b0 = 0xF0 | (cp >> 18);
                b1 = 0x80 | ((cp >> 12) & 0x3F);
                b2 = 0x80 | ((cp >> 6) & 0x3F);
                b3 = 0x80 | (cp & 0x3F);
            }

            if (written + needed > destLen) {
                break;
            }

            if (needed === 1) {
                dest[written++] = b0;
            } else if (needed === 2) {
                dest[written++] = b0;
                dest[written++] = b1;
            } else if (needed === 3) {
                dest[written++] = b0;
                dest[written++] = b1;
                dest[written++] = b2;
            } else {
                dest[written++] = b0;
                dest[written++] = b1;
                dest[written++] = b2;
                dest[written++] = b3;
            }

            read += units;
            i += units;
        }

        return { read: read, written: written };
    };

    globalThis.TextDecoder = function TextDecoder(label, options) {
        this.encoding = (label || 'utf-8').toLowerCase();
        options = options || {};
        this.fatal = !!options.fatal;
        this.ignoreBOM = !!options.ignoreBOM;
        this._pending = null;
    };
    TextDecoder.prototype.decode = function(input, options) {
        var isStream = !!(options && options.stream);
        var bytes = null;
        if (input) {
            var raw = (input && input._u8) ? input._u8 : input;
            if (raw instanceof ArrayBuffer) {
                bytes = new Uint8Array(raw);
            } else if (ArrayBuffer.isView(raw)) {
                bytes = new Uint8Array(raw.buffer, raw.byteOffset, raw.byteLength);
            } else if (typeof raw === 'object' && raw.buffer) {
                bytes = new Uint8Array(raw.buffer, raw.byteOffset || 0, raw.byteLength || raw.length);
            }
        }

        if (this._pending && this._pending.length > 0) {
            if (bytes && bytes.length > 0) {
                var merged = new Uint8Array(this._pending.length + bytes.length);
                merged.set(this._pending, 0);
                merged.set(bytes, this._pending.length);
                bytes = merged;
            } else {
                bytes = this._pending;
            }
            this._pending = null;
        }

        if (!bytes || bytes.length === 0) {
            return '';
        }

        var toDecode = bytes;
        if (isStream) {
            var len = bytes.length;
            var cut = 0;
            for (var i = 1; i <= 4 && i <= len; i++) {
                var b = bytes[len - i];
                if ((b & 0x80) === 0x00) {
                    break;
                } else if ((b & 0xC0) === 0x80) {
                    continue;
                } else if ((b & 0xE0) === 0xC0) {
                    if (i < 2) cut = i;
                    break;
                } else if ((b & 0xF0) === 0xE0) {
                    if (i < 3) cut = i;
                    break;
                } else if ((b & 0xF8) === 0xF0) {
                    if (i < 4) cut = i;
                    break;
                } else {
                    break;
                }
            }

            if (cut > 0) {
                this._pending = bytes.slice(len - cut);
                toDecode = bytes.subarray(0, len - cut);
            }
        }

        if (toDecode.length === 0) {
            return '';
        }

        var nativeDecode = typeof __brokit_textdecoder_decode === 'function'
            ? __brokit_textdecoder_decode
            : globalThis.__brokit_textdecoder_decode;
        return nativeDecode(toDecode);
    };
})();
