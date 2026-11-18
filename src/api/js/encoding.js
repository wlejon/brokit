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

    // The Encoding Standard's labels for the encodings decoded here; any other
    // label is a RangeError, as in browsers that lack it. `encoding` is the
    // canonical name ('latin1' reads 'windows-1252', 'utf-16' 'utf-16le').
    var LABELS = {};
    function labels(name, list) { for (var i = 0; i < list.length; i++) LABELS[list[i]] = name; }
    labels('utf-8', ['unicode-1-1-utf-8', 'unicode11utf8', 'unicode20utf8', 'utf-8', 'utf8', 'x-unicode20utf8']);
    labels('utf-16le', ['csunicode', 'iso-10646-ucs-2', 'ucs-2', 'unicode', 'unicodefeff', 'utf-16', 'utf-16le']);
    labels('utf-16be', ['unicodefffe', 'utf-16be']);
    labels('windows-1252', ['ansi_x3.4-1968', 'ascii', 'cp1252', 'cp819', 'csisolatin1', 'ibm819', 'iso-8859-1',
                            'iso-ir-100', 'iso8859-1', 'iso88591', 'iso_8859-1', 'iso_8859-1:1987', 'l1', 'latin1',
                            'us-ascii', 'windows-1252', 'x-cp1252']);
    // windows-1252's 0x80..0x9F (the rest of the byte range is Latin-1).
    var CP1252 = [0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039,
                  0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                  0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178];

    function codeUnits(units) {
        var out = '';
        for (var i = 0; i < units.length; i += 4096)
            out += String.fromCharCode.apply(null, units.slice(i, i + 4096));
        return out;
    }

    globalThis.TextDecoder = function TextDecoder(label, options) {
        var key = label === undefined ? 'utf-8' : String(label).replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, '').toLowerCase();
        var encoding = LABELS[key];
        if (!encoding) throw new RangeError("The encoding label provided ('" + label + "') is invalid.");
        this.encoding = encoding;
        options = options || {};
        this.fatal = !!options.fatal;
        this.ignoreBOM = !!options.ignoreBOM;
        this._pending = null;
        this._bomSeen = false;
    };
    TextDecoder.prototype.decode = function(input, options) {
        if (this.encoding !== 'utf-8') return decodeOther(this, input, options);
        var str = decodeUtf8.call(this, input, options);
        // The BOM at the start of a stream is not text unless ignoreBOM.
        if (!this._bomSeen && str.length > 0) {
            this._bomSeen = true;
            if (!this.ignoreBOM && str.charCodeAt(0) === 0xFEFF) str = str.slice(1);
        }
        if (!(options && options.stream)) this._bomSeen = false;
        return str;
    };

    function bytesOf(input) {
        if (!input) return null;
        var raw = (input && input._u8) ? input._u8 : input;
        if (raw instanceof ArrayBuffer) return new Uint8Array(raw);
        if (ArrayBuffer.isView(raw)) return new Uint8Array(raw.buffer, raw.byteOffset, raw.byteLength);
        if (typeof raw === 'object' && raw.buffer) return new Uint8Array(raw.buffer, raw.byteOffset || 0, raw.byteLength || raw.length);
        return null;
    }

    function decodeOther(dec, input, options) {
        var isStream = !!(options && options.stream);
        var bytes = bytesOf(input) || new Uint8Array(0);
        if (dec._pending && dec._pending.length) {
            var merged = new Uint8Array(dec._pending.length + bytes.length);
            merged.set(dec._pending, 0);
            merged.set(bytes, dec._pending.length);
            bytes = merged;
        }
        dec._pending = null;
        var units = [];
        if (dec.encoding === 'windows-1252') {
            for (var i = 0; i < bytes.length; i++) {
                var b = bytes[i];
                units.push(b >= 0x80 && b <= 0x9F ? CP1252[b - 0x80] : b);
            }
        } else {
            var be = dec.encoding === 'utf-16be';
            var n = bytes.length & ~1;
            for (var j = 0; j < n; j += 2)
                units.push(be ? (bytes[j] << 8) | bytes[j + 1] : bytes[j] | (bytes[j + 1] << 8));
            // A lone high surrogate at the end of a chunk waits for its pair.
            var keep = n < bytes.length ? 1 : 0;
            if (isStream && units.length && units[units.length - 1] >= 0xD800 && units[units.length - 1] <= 0xDBFF) {
                units.pop();
                keep += 2;
            }
            if (isStream && keep) dec._pending = bytes.slice(bytes.length - keep);
            else if (keep) {
                if (dec.fatal) throw new TypeError('The encoded data was not valid for encoding ' + dec.encoding);
                units.push(0xFFFD);
            }
            for (var k = 0; k < units.length; k++) {
                var u = units[k];
                if (u >= 0xD800 && u <= 0xDBFF) {
                    var next = units[k + 1];
                    if (next >= 0xDC00 && next <= 0xDFFF) { k++; continue; }
                } else if (!(u >= 0xDC00 && u <= 0xDFFF)) {
                    continue;
                }
                if (dec.fatal) throw new TypeError('The encoded data was not valid for encoding ' + dec.encoding);
                units[k] = 0xFFFD;
            }
            if (!dec._bomSeen && units.length) {
                dec._bomSeen = true;
                if (!dec.ignoreBOM && units[0] === 0xFEFF) units.shift();
            }
        }
        if (!isStream) dec._bomSeen = false;
        return codeUnits(units);
    }

    function decodeUtf8(input, options) {
        var isStream = !!(options && options.stream);
        var bytes = bytesOf(input);

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
    }
})();
