(function() {
    'use strict';

    var util = {};

    // ---------------------------------------------------------------
    // Symbols
    // ---------------------------------------------------------------
    var kPromisifyCustom = Symbol.for('nodejs.util.promisify.custom');
    var kInspectCustom = Symbol.for('nodejs.util.inspect.custom');

    // ---------------------------------------------------------------
    // promisify(fn)
    // ---------------------------------------------------------------
    function promisify(fn) {
        if (typeof fn !== 'function') {
            throw new TypeError('The "original" argument must be of type Function');
        }
        // Honor a custom promisified implementation if present.
        if (fn[kPromisifyCustom]) {
            var custom = fn[kPromisifyCustom];
            if (typeof custom !== 'function') {
                throw new TypeError('The "util.promisify.custom" property must be of type Function');
            }
            return custom;
        }
        function promisified() {
            var args = Array.prototype.slice.call(arguments);
            var self = this;
            return new Promise(function(resolve, reject) {
                args.push(function(err, result) {
                    if (err) reject(err);
                    else resolve(result);
                });
                fn.apply(self, args);
            });
        }
        // Preserve prototype so `Object.setPrototypeOf`-style expectations hold.
        Object.setPrototypeOf(promisified, Object.getPrototypeOf(fn));
        return promisified;
    }
    promisify.custom = kPromisifyCustom;
    util.promisify = promisify;

    // ---------------------------------------------------------------
    // callbackify(fn)  — wrap an async fn into (...args, cb) form
    // ---------------------------------------------------------------
    function callbackify(fn) {
        if (typeof fn !== 'function') {
            throw new TypeError('The "original" argument must be of type Function');
        }
        function callbackified() {
            var args = Array.prototype.slice.call(arguments);
            var cb = args.pop();
            if (typeof cb !== 'function') {
                throw new TypeError('The last argument must be of type Function');
            }
            var self = this;
            var result;
            try {
                result = Promise.resolve(fn.apply(self, args));
            } catch (err) {
                result = Promise.reject(err);
            }
            result.then(
                function(value) { cb.call(self, null, value); },
                function(err) {
                    if (!err) {
                        var e = new Error(err ? String(err) : 'Promise was rejected with a falsy value');
                        e.reason = err;
                        err = e;
                    }
                    cb.call(self, err);
                }
            );
        }
        Object.setPrototypeOf(callbackified, Object.getPrototypeOf(fn));
        return callbackified;
    }
    util.callbackify = callbackify;

    // ---------------------------------------------------------------
    // inspect(obj, opts?)
    // ---------------------------------------------------------------
    function quoteString(s) {
        var out = s
            .replace(/\\/g, '\\\\')
            .replace(/'/g, "\\'")
            .replace(/\n/g, '\\n');
        return "'" + out + "'";
    }

    function inspect(obj, opts) {
        var depth = 2;
        if (opts && typeof opts === 'object' && typeof opts.depth === 'number') {
            depth = opts.depth;
        } else if (typeof opts === 'number') {
            depth = opts;
        }
        var seen = new Set();

        function fmt(value, currentDepth) {
            // Primitives
            if (value === null) return 'null';
            if (value === undefined) return 'undefined';

            var t = typeof value;
            if (t === 'string') return quoteString(value);
            if (t === 'number' || t === 'boolean' || t === 'bigint') return String(value);
            if (t === 'symbol') return value.toString();
            if (t === 'function') {
                var name = value.name;
                return name ? '[Function: ' + name + ']' : '[Function (anonymous)]';
            }

            // Custom inspect hook
            if (value && typeof value[kInspectCustom] === 'function') {
                try {
                    return String(value[kInspectCustom](currentDepth, opts || {}));
                } catch (e) { /* fall through */ }
            }

            // Well-known object types
            if (value instanceof Date) {
                return isNaN(value.getTime()) ? 'Invalid Date' : value.toISOString();
            }
            if (value instanceof RegExp) return String(value);
            if (value instanceof Error) {
                return value.stack ? String(value.stack) : (value.name + ': ' + value.message);
            }

            // Circular check
            if (seen.has(value)) return '[Circular]';

            var isArray = Array.isArray(value);

            // Depth limit
            if (currentDepth < 0) {
                return isArray ? '[Array]' : '[Object]';
            }

            seen.add(value);
            var result;

            if (isArray) {
                var arrParts = [];
                for (var i = 0; i < value.length; i++) {
                    arrParts.push(fmt(value[i], currentDepth - 1));
                }
                result = arrParts.length ? '[ ' + arrParts.join(', ') + ' ]' : '[]';
            } else {
                var keys = Object.keys(value);
                var objParts = [];
                for (var k = 0; k < keys.length; k++) {
                    var key = keys[k];
                    var keyStr = /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(key) ? key : quoteString(key);
                    objParts.push(keyStr + ': ' + fmt(value[key], currentDepth - 1));
                }
                var prefix = '';
                var ctorName = value.constructor && value.constructor.name;
                if (ctorName && ctorName !== 'Object') prefix = ctorName + ' ';
                result = objParts.length ? prefix + '{ ' + objParts.join(', ') + ' }' : prefix + '{}';
            }

            seen.delete(value);
            return result;
        }

        return fmt(obj, depth);
    }
    inspect.custom = kInspectCustom;
    util.inspect = inspect;

    // ---------------------------------------------------------------
    // format(fmt, ...args)  — printf-style
    // ---------------------------------------------------------------
    function format(f) {
        var args = Array.prototype.slice.call(arguments);
        var i = 1;

        if (typeof f !== 'string') {
            // No format string: join all args by inspect/String.
            var pieces = [];
            for (var a = 0; a < args.length; a++) {
                pieces.push(formatOne(args[a]));
            }
            return pieces.join(' ');
        }

        var str = f.replace(/%[sdifjoO%]/g, function(match) {
            if (match === '%%') return '%';
            if (i >= args.length) return match; // missing arg -> leave literal
            var arg = args[i++];
            switch (match) {
                case '%s': return typeof arg === 'bigint' ? String(arg) :
                                  (typeof arg === 'object' && arg !== null ? inspect(arg) : String(arg));
                case '%d':
                case '%i': return typeof arg === 'bigint' ? String(arg) : String(parseInt(arg, 10));
                case '%f': return String(parseFloat(arg));
                case '%j':
                    try { return JSON.stringify(arg); }
                    catch (e) { return '[Circular]'; }
                case '%o':
                case '%O': return inspect(arg);
                default: return match;
            }
        });

        // Append any remaining args, space-separated.
        for (; i < args.length; i++) {
            str += ' ' + formatOne(args[i]);
        }
        return str;
    }

    function formatOne(arg) {
        if (arg === null) return 'null';
        if (typeof arg === 'object' || typeof arg === 'function') return inspect(arg);
        return String(arg);
    }
    util.format = format;

    // ---------------------------------------------------------------
    // inherits(ctor, superCtor)
    // ---------------------------------------------------------------
    function inherits(ctor, superCtor) {
        if (ctor === undefined || ctor === null) {
            throw new TypeError('The constructor to "inherits" must not be null or undefined');
        }
        if (superCtor === undefined || superCtor === null) {
            throw new TypeError('The super constructor to "inherits" must not be null or undefined');
        }
        if (superCtor.prototype === undefined) {
            throw new TypeError('The super constructor to "inherits" must have a prototype');
        }
        ctor.super_ = superCtor;
        Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
    }
    util.inherits = inherits;

    // ---------------------------------------------------------------
    // deprecate(fn, msg)  — warn once, then delegate
    // ---------------------------------------------------------------
    function deprecate(fn, msg) {
        var warned = false;
        function deprecated() {
            if (!warned) {
                warned = true;
                if (typeof console !== 'undefined' && console.warn) {
                    console.warn(msg);
                }
            }
            return fn.apply(this, arguments);
        }
        return deprecated;
    }
    util.deprecate = deprecate;

    // ---------------------------------------------------------------
    // types
    // ---------------------------------------------------------------
    function tag(v) {
        return Object.prototype.toString.call(v);
    }
    var types = {
        isDate: function(v) { return tag(v) === '[object Date]'; },
        isRegExp: function(v) { return tag(v) === '[object RegExp]'; },
        isNativeError: function(v) {
            return v instanceof Error || tag(v) === '[object Error]';
        },
        isPromise: function(v) {
            return v instanceof Promise || tag(v) === '[object Promise]' ||
                   (v !== null && typeof v === 'object' && typeof v.then === 'function');
        },
        isTypedArray: function(v) {
            return ArrayBuffer.isView(v) && !(v instanceof DataView);
        },
        isArrayBuffer: function(v) { return tag(v) === '[object ArrayBuffer]'; },
        isMap: function(v) { return tag(v) === '[object Map]'; },
        isSet: function(v) { return tag(v) === '[object Set]'; }
    };
    util.types = types;

    // ---------------------------------------------------------------
    // isDeepStrictEqual(a, b)
    // ---------------------------------------------------------------
    function isDeepStrictEqual(a, b) {
        return deepEqual(a, b, new Set());
    }

    function deepEqual(a, b, seen) {
        // Identity + NaN handling via Object.is
        if (Object.is(a, b)) return true;

        // Both must be non-null objects to recurse.
        if (typeof a !== 'object' || typeof b !== 'object' || a === null || b === null) {
            return false;
        }

        // Prototype must match (different key sets / types => false).
        if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;

        // Circular guard
        if (seen.has(a)) return true;
        seen.add(a);

        var equal = false;

        // Dates
        if (a instanceof Date && b instanceof Date) {
            equal = a.getTime() === b.getTime();
            seen.delete(a);
            return equal;
        }

        // RegExp
        if (a instanceof RegExp && b instanceof RegExp) {
            equal = a.source === b.source && a.flags === b.flags;
            seen.delete(a);
            return equal;
        }

        // Arrays
        if (Array.isArray(a) || Array.isArray(b)) {
            if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) {
                seen.delete(a);
                return false;
            }
            for (var i = 0; i < a.length; i++) {
                if (!deepEqual(a[i], b[i], seen)) { seen.delete(a); return false; }
            }
            seen.delete(a);
            return true;
        }

        // Maps
        if (a instanceof Map && b instanceof Map) {
            if (a.size !== b.size) { seen.delete(a); return false; }
            var mapIt = a.entries();
            var me = mapIt.next();
            while (!me.done) {
                var mk = me.value[0];
                if (!b.has(mk) || !deepEqual(me.value[1], b.get(mk), seen)) {
                    seen.delete(a);
                    return false;
                }
                me = mapIt.next();
            }
            seen.delete(a);
            return true;
        }

        // Sets
        if (a instanceof Set && b instanceof Set) {
            if (a.size !== b.size) { seen.delete(a); return false; }
            var setIt = a.values();
            var se = setIt.next();
            while (!se.done) {
                if (!b.has(se.value)) { seen.delete(a); return false; }
                se = setIt.next();
            }
            seen.delete(a);
            return true;
        }

        // Plain objects
        var keysA = Object.keys(a);
        var keysB = Object.keys(b);
        if (keysA.length !== keysB.length) { seen.delete(a); return false; }
        for (var k = 0; k < keysA.length; k++) {
            var key = keysA[k];
            if (!Object.prototype.hasOwnProperty.call(b, key)) { seen.delete(a); return false; }
            if (!deepEqual(a[key], b[key], seen)) { seen.delete(a); return false; }
        }
        seen.delete(a);
        return true;
    }
    util.isDeepStrictEqual = isDeepStrictEqual;

    // ---------------------------------------------------------------
    // Text encoders (re-export existing globals)
    // ---------------------------------------------------------------
    util.TextEncoder = globalThis.TextEncoder;
    util.TextDecoder = globalThis.TextDecoder;

    // ---------------------------------------------------------------
    // Register in the module registry (do NOT attach to globalThis)
    // ---------------------------------------------------------------
    globalThis.__brokit_modules = globalThis.__brokit_modules || {};
    globalThis.__brokit_modules['util'] = util;
})();
