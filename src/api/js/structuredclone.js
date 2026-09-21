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

    function detachArrayBuffer(ab) {
        if (typeof ab.transfer === 'function') {
            return ab.transfer();
        }
        var copy = ab.slice(0);
        try {
            new Uint8Array(ab).fill(0);
        } catch (e) {}
        try {
            Object.defineProperty(ab, 'byteLength', { value: 0 });
            Object.defineProperty(ab, 'detached', { value: true });
        } catch (e) {}
        return copy;
    }

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

        // Circular reference / transfer check
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
            if (value.detached) {
                throw new globalThis.DOMException('Cannot clone detached ArrayBuffer', 'DataCloneError');
            }
            var cloned = value.slice(0);
            seen.push({ src: value, dst: cloned });
            return cloned;
        }

        // TypedArrays
        for (var ti = 0; ti < TypedArrayTypes.length; ti++) {
            var TACtor = globalThis[TypedArrayTypes[ti]];
            if (TACtor && value instanceof TACtor) {
                if (value.buffer && value.buffer.detached) {
                    throw new globalThis.DOMException('Cannot clone TypedArray with detached buffer', 'DataCloneError');
                }
                var abTarget = null;
                for (var si = 0; si < seen.length; si++) {
                    if (seen[si].src === value.buffer) {
                        abTarget = seen[si].dst;
                        break;
                    }
                }
                var abClone = abTarget ? abTarget : value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength);
                var taClone = abTarget ? new TACtor(abClone, value.byteOffset, value.length) : new TACtor(abClone);
                seen.push({ src: value, dst: taClone });
                return taClone;
            }
        }

        // DataView
        if (typeof DataView !== 'undefined' && value instanceof DataView) {
            if (value.buffer && value.buffer.detached) {
                throw new globalThis.DOMException('Cannot clone DataView with detached buffer', 'DataCloneError');
            }
            var abTargetDv = null;
            for (var sdi = 0; sdi < seen.length; sdi++) {
                if (seen[sdi].src === value.buffer) {
                    abTargetDv = seen[sdi].dst;
                    break;
                }
            }
            var dvBuf = abTargetDv ? abTargetDv : value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength);
            var dvClone = new DataView(dvBuf, abTargetDv ? value.byteOffset : 0, value.byteLength);
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

        // File (checked before Blob since File extends Blob)
        if (typeof File !== 'undefined' && value instanceof File) {
            var fileClone = new File([value], value.name, {
                type: value.type,
                lastModified: value.lastModified,
                webkitRelativePath: value.webkitRelativePath
            });
            seen.push({ src: value, dst: fileClone });
            return fileClone;
        }

        // Blob
        if (typeof Blob !== 'undefined' && value instanceof Blob) {
            var blobClone = new Blob([value], { type: value.type });
            seen.push({ src: value, dst: blobClone });
            return blobClone;
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

    globalThis.structuredClone = function structuredClone(value, options) {
        var transferList = [];
        if (options) {
            if (Array.isArray(options)) {
                transferList = options;
            } else if (Array.isArray(options.transfer)) {
                transferList = options.transfer;
            }
        }
        var seen = [];
        if (transferList.length > 0) {
            for (var ti = 0; ti < transferList.length; ti++) {
                for (var tj = ti + 1; tj < transferList.length; tj++) {
                    if (transferList[ti] === transferList[tj]) {
                        throw new globalThis.DOMException('Duplicate transferable in transfer list', 'DataCloneError');
                    }
                }
            }
            for (var i = 0; i < transferList.length; i++) {
                var item = transferList[i];
                if (item instanceof ArrayBuffer) {
                    if (item.detached) {
                        throw new globalThis.DOMException('Cannot transfer detached ArrayBuffer', 'DataCloneError');
                    }
                    var transferred = detachArrayBuffer(item);
                    seen.push({ src: item, dst: transferred });
                } else if (typeof MessagePort !== 'undefined' && item instanceof MessagePort) {
                    seen.push({ src: item, dst: item });
                } else {
                    throw new globalThis.DOMException('Transfer list element is not transferable', 'DataCloneError');
                }
            }
        }
        return cloneValue(value, seen);
    };
})();
