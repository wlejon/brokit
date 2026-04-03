(function() {
    if (typeof globalThis.atob !== 'undefined') return;

    var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    var lookup = new Uint8Array(128);
    for (var i = 0; i < chars.length; i++) lookup[chars.charCodeAt(i)] = i;

    globalThis.btoa = function(str) {
        str = String(str);
        for (var i = 0; i < str.length; i++) {
            if (str.charCodeAt(i) > 255) {
                throw new DOMException(
                    "The string to be encoded contains characters outside of the Latin1 range.",
                    "InvalidCharacterError"
                );
            }
        }
        var out = '';
        var len = str.length;
        for (var i = 0; i < len; i += 3) {
            var a = str.charCodeAt(i);
            var b = (i + 1 < len) ? str.charCodeAt(i + 1) : 0;
            var c = (i + 2 < len) ? str.charCodeAt(i + 2) : 0;
            var triple = (a << 16) | (b << 8) | c;
            out += chars[(triple >> 18) & 63];
            out += chars[(triple >> 12) & 63];
            out += (i + 1 < len) ? chars[(triple >> 6) & 63] : '=';
            out += (i + 2 < len) ? chars[triple & 63] : '=';
        }
        return out;
    };

    globalThis.atob = function(str) {
        str = String(str).replace(/[\t\n\f\r ]/g, '');
        if (str.length % 4 === 1) {
            throw new DOMException(
                "The string to be decoded is not correctly encoded.",
                "InvalidCharacterError"
            );
        }
        var out = '';
        var i = 0;
        var len = str.length;
        while (i < len) {
            var a = lookup[str.charCodeAt(i++)] || 0;
            var b = lookup[str.charCodeAt(i++)] || 0;
            var c = lookup[str.charCodeAt(i++)] || 0;
            var d = lookup[str.charCodeAt(i++)] || 0;
            var triple = (a << 18) | (b << 12) | (c << 6) | d;
            out += String.fromCharCode((triple >> 16) & 255);
            if (str[i - 2] !== '=') out += String.fromCharCode((triple >> 8) & 255);
            if (str[i - 1] !== '=') out += String.fromCharCode(triple & 255);
        }
        return out;
    };
})();
