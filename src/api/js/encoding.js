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

    globalThis.TextDecoder = function TextDecoder(encoding) {
        this.encoding = (encoding || 'utf-8').toLowerCase();
        this.fatal = false;
        this.ignoreBOM = false;
    };
    TextDecoder.prototype.decode = function(input) {
        if (!input) return '';
        var raw = (input && input._u8) ? input._u8 : input;
        return (typeof __brokit_textdecoder_decode === 'function' ? __brokit_textdecoder_decode : globalThis.__brokit_textdecoder_decode)(raw);
    };
})();
