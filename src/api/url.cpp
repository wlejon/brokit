#include "api/api.h"
#include "runtime/runtime.h"

#include <cstring>

namespace brokit::api {

// URL and URLSearchParams implemented as JS polyfill.
// This follows the WHATWG URL Standard closely enough for framework compatibility.

void installURL(JSContext* ctx)
{
    const char* polyfill = R"JS(
(function() {
    // =========================================================================
    // URLSearchParams
    // =========================================================================
    function URLSearchParams(init) {
        this._entries = [];
        if (typeof init === 'string') {
            var s = init;
            if (s.charAt(0) === '?') s = s.substring(1);
            if (s.length === 0) return;
            var pairs = s.split('&');
            for (var i = 0; i < pairs.length; i++) {
                var pair = pairs[i];
                var eq = pair.indexOf('=');
                if (eq === -1) {
                    this._entries.push([decodeURIComponent(pair.replace(/\+/g, ' ')), '']);
                } else {
                    this._entries.push([
                        decodeURIComponent(pair.substring(0, eq).replace(/\+/g, ' ')),
                        decodeURIComponent(pair.substring(eq + 1).replace(/\+/g, ' '))
                    ]);
                }
            }
        } else if (init && typeof init === 'object') {
            if (Array.isArray(init)) {
                for (var i = 0; i < init.length; i++) {
                    this._entries.push([String(init[i][0]), String(init[i][1])]);
                }
            } else {
                var keys = Object.keys(init);
                for (var i = 0; i < keys.length; i++) {
                    this._entries.push([keys[i], String(init[keys[i]])]);
                }
            }
        }
    }

    URLSearchParams.prototype.append = function(name, value) {
        this._entries.push([String(name), String(value)]);
        this._updateUrl();
    };
    URLSearchParams.prototype.delete = function(name) {
        this._entries = this._entries.filter(function(e) { return e[0] !== name; });
        this._updateUrl();
    };
    URLSearchParams.prototype.get = function(name) {
        for (var i = 0; i < this._entries.length; i++) {
            if (this._entries[i][0] === name) return this._entries[i][1];
        }
        return null;
    };
    URLSearchParams.prototype.getAll = function(name) {
        var result = [];
        for (var i = 0; i < this._entries.length; i++) {
            if (this._entries[i][0] === name) result.push(this._entries[i][1]);
        }
        return result;
    };
    URLSearchParams.prototype.has = function(name) {
        for (var i = 0; i < this._entries.length; i++) {
            if (this._entries[i][0] === name) return true;
        }
        return false;
    };
    URLSearchParams.prototype.set = function(name, value) {
        var found = false;
        var entries = [];
        for (var i = 0; i < this._entries.length; i++) {
            if (this._entries[i][0] === name) {
                if (!found) { entries.push([name, String(value)]); found = true; }
            } else {
                entries.push(this._entries[i]);
            }
        }
        if (!found) entries.push([name, String(value)]);
        this._entries = entries;
        this._updateUrl();
    };
    URLSearchParams.prototype.sort = function() {
        this._entries.sort(function(a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; });
        this._updateUrl();
    };
    URLSearchParams.prototype.toString = function() {
        return this._entries.map(function(e) {
            return encodeURIComponent(e[0]) + '=' + encodeURIComponent(e[1]);
        }).join('&');
    };
    URLSearchParams.prototype.forEach = function(callback, thisArg) {
        for (var i = 0; i < this._entries.length; i++) {
            callback.call(thisArg, this._entries[i][1], this._entries[i][0], this);
        }
    };
    URLSearchParams.prototype.keys = function() {
        var entries = this._entries;
        var i = 0;
        return { next: function() {
            if (i >= entries.length) return { done: true };
            return { done: false, value: entries[i++][0] };
        }, [Symbol.iterator]: function() { return this; } };
    };
    URLSearchParams.prototype.values = function() {
        var entries = this._entries;
        var i = 0;
        return { next: function() {
            if (i >= entries.length) return { done: true };
            return { done: false, value: entries[i++][1] };
        }, [Symbol.iterator]: function() { return this; } };
    };
    URLSearchParams.prototype.entries = function() {
        var entries = this._entries;
        var i = 0;
        return { next: function() {
            if (i >= entries.length) return { done: true };
            return { done: false, value: [entries[i][0], entries[i++][1]] };
        }, [Symbol.iterator]: function() { return this; } };
    };
    URLSearchParams.prototype[Symbol.iterator] = URLSearchParams.prototype.entries;
    URLSearchParams.prototype._updateUrl = function() {
        // If linked to a URL object, update its search
        if (this._url) {
            this._url._search = this._entries.length ? '?' + this.toString() : '';
        }
    };

    // =========================================================================
    // URL
    // =========================================================================
    var _knownSchemes = { 'http:': 80, 'https:': 443, 'ftp:': 21, 'ws:': 80, 'wss:': 443 };

    function URL(url, base) {
        if (typeof url !== 'string') url = String(url);

        // Resolve base
        var baseObj = null;
        if (base !== undefined) {
            baseObj = (base instanceof URL) ? base : new URL(String(base));
        }

        // Parse scheme
        var schemeMatch = url.match(/^([a-zA-Z][a-zA-Z0-9+\-.]*:)/);
        if (schemeMatch) {
            this._protocol = schemeMatch[1].toLowerCase();
            url = url.substring(schemeMatch[1].length);
        } else if (baseObj) {
            this._protocol = baseObj._protocol;
        } else {
            throw new TypeError("Invalid URL: " + arguments[0]);
        }

        // Parse authority (//user:pass@host:port)
        this._username = '';
        this._password = '';
        this._hostname = '';
        this._port = '';
        this._pathname = '/';
        this._search = '';
        this._hash = '';

        if (url.substring(0, 2) === '//') {
            url = url.substring(2);
            var authEnd = url.search(/[/?#]/);
            var authority = authEnd === -1 ? url : url.substring(0, authEnd);
            url = authEnd === -1 ? '' : url.substring(authEnd);

            // userinfo@
            var atIdx = authority.lastIndexOf('@');
            if (atIdx !== -1) {
                var userinfo = authority.substring(0, atIdx);
                authority = authority.substring(atIdx + 1);
                var colonIdx = userinfo.indexOf(':');
                if (colonIdx !== -1) {
                    this._username = userinfo.substring(0, colonIdx);
                    this._password = userinfo.substring(colonIdx + 1);
                } else {
                    this._username = userinfo;
                }
            }

            // host:port
            var bracketEnd = authority.indexOf(']');
            var portIdx = authority.indexOf(':', bracketEnd === -1 ? 0 : bracketEnd);
            if (portIdx !== -1 && bracketEnd < portIdx) {
                this._hostname = authority.substring(0, portIdx).toLowerCase();
                this._port = authority.substring(portIdx + 1);
                // Strip default port
                var defPort = _knownSchemes[this._protocol];
                if (defPort !== undefined && parseInt(this._port) === defPort) this._port = '';
            } else {
                this._hostname = authority.toLowerCase();
            }
        } else if (baseObj) {
            this._username = baseObj._username;
            this._password = baseObj._password;
            this._hostname = baseObj._hostname;
            this._port = baseObj._port;
        }

        // Hash
        var hashIdx = url.indexOf('#');
        if (hashIdx !== -1) {
            this._hash = url.substring(hashIdx);
            url = url.substring(0, hashIdx);
        }

        // Search
        var searchIdx = url.indexOf('?');
        if (searchIdx !== -1) {
            this._search = url.substring(searchIdx);
            url = url.substring(0, searchIdx);
        }

        // Pathname
        if (url.length > 0) {
            if (url.charAt(0) === '/') {
                this._pathname = url;
            } else if (baseObj) {
                // Resolve relative path
                var basePath = baseObj._pathname;
                var lastSlash = basePath.lastIndexOf('/');
                this._pathname = basePath.substring(0, lastSlash + 1) + url;
            } else {
                this._pathname = '/' + url;
            }
        } else if (baseObj) {
            this._pathname = baseObj._pathname;
            if (!this._search && baseObj._search) this._search = baseObj._search;
        }

        // Normalize path (resolve . and ..)
        var parts = this._pathname.split('/');
        var resolved = [];
        for (var i = 0; i < parts.length; i++) {
            if (parts[i] === '..') { if (resolved.length > 1) resolved.pop(); }
            else if (parts[i] !== '.') resolved.push(parts[i]);
        }
        this._pathname = resolved.join('/') || '/';
        if (this._pathname.charAt(0) !== '/') this._pathname = '/' + this._pathname;

        // Build searchParams
        this.searchParams = new URLSearchParams(this._search);
        this.searchParams._url = this;
    }

    Object.defineProperties(URL.prototype, {
        protocol: { get: function() { return this._protocol; }, set: function(v) { this._protocol = v; } },
        username: { get: function() { return this._username; }, set: function(v) { this._username = v; } },
        password: { get: function() { return this._password; }, set: function(v) { this._password = v; } },
        hostname: { get: function() { return this._hostname; }, set: function(v) { this._hostname = v; } },
        port:     { get: function() { return this._port; }, set: function(v) { this._port = v; } },
        pathname: { get: function() { return this._pathname; }, set: function(v) { this._pathname = v; } },
        search:   {
            get: function() { return this._search; },
            set: function(v) {
                this._search = v;
                this.searchParams = new URLSearchParams(v);
                this.searchParams._url = this;
            }
        },
        hash:     { get: function() { return this._hash; }, set: function(v) { this._hash = v; } },
        host:     {
            get: function() { return this._port ? this._hostname + ':' + this._port : this._hostname; },
            set: function(v) {
                var idx = v.indexOf(':');
                if (idx !== -1) { this._hostname = v.substring(0, idx); this._port = v.substring(idx + 1); }
                else { this._hostname = v; this._port = ''; }
            }
        },
        origin:   { get: function() { return this._protocol + '//' + this.host; } },
        href:     {
            get: function() {
                var s = this._protocol + '//';
                if (this._username) {
                    s += this._username;
                    if (this._password) s += ':' + this._password;
                    s += '@';
                }
                s += this.host + this._pathname + this._search + this._hash;
                return s;
            },
            set: function(v) {
                var u = new URL(v);
                this._protocol = u._protocol; this._username = u._username;
                this._password = u._password; this._hostname = u._hostname;
                this._port = u._port; this._pathname = u._pathname;
                this._search = u._search; this._hash = u._hash;
                this.searchParams = u.searchParams;
                this.searchParams._url = this;
            }
        }
    });

    URL.prototype.toString = function() { return this.href; };
    URL.prototype.toJSON = function() { return this.href; };

    globalThis.URL = URL;
    globalThis.URLSearchParams = URLSearchParams;
})();
)JS";

    JSValue r = JS_Eval(ctx, polyfill, strlen(polyfill), "<url>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
