(function() {
    globalThis.TextEncoder = function TextEncoder() {
        this.encoding = 'utf-8';
    };
    TextEncoder.prototype.encode = function(str) {
        return globalThis.__brokit_textencoder_encode(str || '');
    };
    TextEncoder.prototype.encodeInto = function(str, dest) {
        var encoded = this.encode(str);
        var len = Math.min(encoded.length, dest.length);
        for (var i = 0; i < len; i++) dest[i] = encoded[i];
        return { read: str.length, written: len };
    };

    globalThis.TextDecoder = function TextDecoder(encoding) {
        this.encoding = (encoding || 'utf-8').toLowerCase();
        this.fatal = false;
        this.ignoreBOM = false;
    };
    TextDecoder.prototype.decode = function(input) {
        if (!input) return '';
        return globalThis.__brokit_textdecoder_decode(input);
    };
})();
