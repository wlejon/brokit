(function() {
    if (typeof globalThis.DOMException === 'undefined') {
        var _DOMException = function DOMException(message, name) {
            this.message = message || '';
            this.name = name || 'Error';
        };
        _DOMException.prototype = Object.create(Error.prototype);
        _DOMException.prototype.constructor = _DOMException;
        globalThis.DOMException = _DOMException;
    }

    var TypedArrayTypes = [
        'Int8Array', 'Uint8Array', 'Uint8ClampedArray',
        'Int16Array', 'Uint16Array',
        'Int32Array', 'Uint32Array',
        'Float32Array', 'Float64Array',
        'BigInt64Array', 'BigUint64Array'
    ];

    function cloneValue(value, seen) {
        // Primitives
        if (value === null || value === undefined) return value;
        var t = typeof value;
        if (t === 'boolean' || t === 'number' || t === 'string' || t === 'bigint') return value;

        if (t === 'symbol') {
            throw new globalThis.DOMException('Symbols cannot be cloned.', 'DataCloneError');
        }
        if (t === 'function') {
            throw new globalThis.DOMException('Functions cannot be cloned.', 'DataCloneError');
        }

        // Circular reference check
        for (var i = 0; i < seen.length; i++) {
            if (seen[i].src === value) return seen[i].dst;
        }

        // Date
        if (value instanceof Date) {
            return new Date(value.getTime());
        }

        // RegExp
        if (value instanceof RegExp) {
            return new RegExp(value.source, value.flags);
        }

        // Error types
        if (value instanceof Error) {
            var ErrorCtor = value.constructor || Error;
            try {
                var errClone = new ErrorCtor(value.message);
            } catch(e) {
                var errClone = new Error(value.message);
            }
            errClone.name = value.name;
            if (value.stack) errClone.stack = value.stack;
            if (value.cause !== undefined) errClone.cause = cloneValue(value.cause, seen);
            return errClone;
        }

        // ArrayBuffer
        if (value instanceof ArrayBuffer) {
            var cloned = value.slice(0);
            seen.push({ src: value, dst: cloned });
            return cloned;
        }

        // TypedArrays
        for (var ti = 0; ti < TypedArrayTypes.length; ti++) {
            var TACtor = globalThis[TypedArrayTypes[ti]];
            if (TACtor && value instanceof TACtor) {
                var abClone = value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength);
                var taClone = new TACtor(abClone);
                seen.push({ src: value, dst: taClone });
                return taClone;
            }
        }

        // DataView
        if (typeof DataView !== 'undefined' && value instanceof DataView) {
            var dvBuf = value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength);
            var dvClone = new DataView(dvBuf);
            seen.push({ src: value, dst: dvClone });
            return dvClone;
        }

        // Map
        if (value instanceof Map) {
            var mapClone = new Map();
            seen.push({ src: value, dst: mapClone });
            value.forEach(function(v, k) {
                mapClone.set(cloneValue(k, seen), cloneValue(v, seen));
            });
            return mapClone;
        }

        // Set
        if (value instanceof Set) {
            var setClone = new Set();
            seen.push({ src: value, dst: setClone });
            value.forEach(function(v) {
                setClone.add(cloneValue(v, seen));
            });
            return setClone;
        }

        // WeakMap / WeakSet / Promise — not cloneable
        if (typeof WeakMap !== 'undefined' && value instanceof WeakMap) {
            throw new globalThis.DOMException('WeakMap cannot be cloned.', 'DataCloneError');
        }
        if (typeof WeakSet !== 'undefined' && value instanceof WeakSet) {
            throw new globalThis.DOMException('WeakSet cannot be cloned.', 'DataCloneError');
        }
        if (typeof Promise !== 'undefined' && value instanceof Promise) {
            throw new globalThis.DOMException('Promise cannot be cloned.', 'DataCloneError');
        }

        // Array
        if (Array.isArray(value)) {
            var arrClone = [];
            seen.push({ src: value, dst: arrClone });
            for (var ai = 0; ai < value.length; ai++) {
                arrClone.push(cloneValue(value[ai], seen));
            }
            return arrClone;
        }

        // Plain object
        var objClone = {};
        seen.push({ src: value, dst: objClone });
        var keys = Object.keys(value);
        for (var ki = 0; ki < keys.length; ki++) {
            objClone[keys[ki]] = cloneValue(value[keys[ki]], seen);
        }
        return objClone;
    }

    globalThis.structuredClone = function structuredClone(value) {
        return cloneValue(value, []);
    };
})();
