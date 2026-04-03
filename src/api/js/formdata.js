(function() {
    if (typeof globalThis.FormData !== 'undefined') return;

    // Each entry is { name, value, filename }.
    // value is either a string or a Blob/File.
    function FormData(form) {
        if (form !== undefined && form !== null) {
            throw new TypeError('FormData from HTMLFormElement is not supported');
        }
        this._entries = [];
    }

    function normalizeValue(value, filename) {
        if (value instanceof File) {
            if (filename !== undefined) {
                value = new File([value], String(filename), {
                    type: value.type,
                    lastModified: value.lastModified
                });
            }
            return value;
        }
        if (value instanceof Blob) {
            var fn = filename !== undefined ? String(filename) : 'blob';
            return new File([value], fn, { type: value.type });
        }
        return String(value);
    }

    FormData.prototype.append = function(name, value, filename) {
        name = String(name);
        this._entries.push({ name: name, value: normalizeValue(value, filename) });
    };

    FormData.prototype.delete = function(name) {
        name = String(name);
        this._entries = this._entries.filter(function(e) { return e.name !== name; });
    };

    FormData.prototype.get = function(name) {
        name = String(name);
        for (var i = 0; i < this._entries.length; i++) {
            if (this._entries[i].name === name) return this._entries[i].value;
        }
        return null;
    };

    FormData.prototype.getAll = function(name) {
        name = String(name);
        var result = [];
        for (var i = 0; i < this._entries.length; i++) {
            if (this._entries[i].name === name) result.push(this._entries[i].value);
        }
        return result;
    };

    FormData.prototype.has = function(name) {
        name = String(name);
        for (var i = 0; i < this._entries.length; i++) {
            if (this._entries[i].name === name) return true;
        }
        return false;
    };

    FormData.prototype.set = function(name, value, filename) {
        name = String(name);
        value = normalizeValue(value, filename);
        var found = false;
        var entries = [];
        for (var i = 0; i < this._entries.length; i++) {
            if (this._entries[i].name === name) {
                if (!found) {
                    entries.push({ name: name, value: value });
                    found = true;
                }
                // skip subsequent entries with same name
            } else {
                entries.push(this._entries[i]);
            }
        }
        if (!found) {
            entries.push({ name: name, value: value });
        }
        this._entries = entries;
    };

    FormData.prototype.entries = function() {
        var idx = 0;
        var entries = this._entries;
        return {
            next: function() {
                if (idx >= entries.length) return { done: true, value: undefined };
                var e = entries[idx++];
                return { done: false, value: [e.name, e.value] };
            },
            [Symbol.iterator]: function() { return this; }
        };
    };

    FormData.prototype.keys = function() {
        var idx = 0;
        var entries = this._entries;
        return {
            next: function() {
                if (idx >= entries.length) return { done: true, value: undefined };
                return { done: false, value: entries[idx++].name };
            },
            [Symbol.iterator]: function() { return this; }
        };
    };

    FormData.prototype.values = function() {
        var idx = 0;
        var entries = this._entries;
        return {
            next: function() {
                if (idx >= entries.length) return { done: true, value: undefined };
                return { done: false, value: entries[idx++].value };
            },
            [Symbol.iterator]: function() { return this; }
        };
    };

    FormData.prototype.forEach = function(callback, thisArg) {
        for (var i = 0; i < this._entries.length; i++) {
            callback.call(thisArg, this._entries[i].value, this._entries[i].name, this);
        }
    };

    FormData.prototype[Symbol.iterator] = FormData.prototype.entries;

    FormData.prototype[Symbol.toStringTag] = 'FormData';

    globalThis.FormData = FormData;
})();
