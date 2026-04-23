(function() {
    if (typeof globalThis.FormData !== 'undefined') return;

    // Each entry is { name, value, filename }.
    // value is either a string or a Blob/File.
    function FormData(form, submitter) {
        this._entries = [];
        if (form === undefined || form === null) return;

        // Per spec "constructing the form data set": walk the form's
        // associated controls in tree order, skipping disabled, unnamed,
        // and otherwise-unsuccessful controls.
        if (!form || typeof form.tagName !== 'string' ||
            form.tagName.toUpperCase() !== 'FORM') {
            throw new TypeError('FormData argument must be an HTMLFormElement');
        }
        if (submitter !== undefined && submitter !== null) {
            // Spec: submitter must be a submit button that is form-associated
            // with the given form. We verify loosely — mismatched submitters
            // get ignored rather than throwing, which is friendlier for tests.
            var st = submitter.tagName && submitter.tagName.toUpperCase();
            if (st !== 'BUTTON' && !(st === 'INPUT' && (submitter.type === 'submit' || submitter.type === 'image'))) {
                submitter = null;
            }
        }

        var els = form.elements;
        var n = (els && els.length) || 0;
        for (var i = 0; i < n; i++) {
            var c = els[i];
            if (!c) continue;
            var tag = (c.tagName || '').toUpperCase();
            var name = c.name || c.getAttribute && c.getAttribute('name');
            if (!name) continue;
            if (c.disabled) continue;
            if (tag === 'FIELDSET' || tag === 'OBJECT') continue;

            if (tag === 'INPUT') {
                var t = (c.type || 'text').toLowerCase();
                if (t === 'button' || t === 'reset') continue;
                if (t === 'submit' || t === 'image') {
                    // Only the submitter contributes; other submits are skipped.
                    if (submitter && c === submitter) {
                        this._entries.push({ name: name, value: String(c.value || '') });
                    }
                    continue;
                }
                if (t === 'checkbox' || t === 'radio') {
                    if (!c.checked) continue;
                    this._entries.push({ name: name, value: String(c.value || 'on') });
                    continue;
                }
                if (t === 'file') {
                    var files = c.files || [];
                    if (files.length === 0) {
                        // Spec: append an empty File with empty name and type.
                        this._entries.push({ name: name,
                            value: new File([], '', { type: 'application/octet-stream' }) });
                    } else {
                        for (var f = 0; f < files.length; f++) {
                            this._entries.push({ name: name, value: files[f] });
                        }
                    }
                    continue;
                }
                this._entries.push({ name: name, value: String(c.value || '') });
            } else if (tag === 'TEXTAREA') {
                this._entries.push({ name: name, value: String(c.value || '') });
            } else if (tag === 'SELECT') {
                // <select multiple> contributes every selected option; single
                // select contributes the current value.
                if (c.multiple) {
                    var opts = c.children || [];
                    for (var j = 0; j < opts.length; j++) {
                        var o = opts[j];
                        if (o.tagName && o.tagName.toUpperCase() === 'OPTION' && o.selected) {
                            this._entries.push({ name: name,
                                value: String(o.value || o.textContent || '') });
                        }
                    }
                } else {
                    this._entries.push({ name: name, value: String(c.value || '') });
                }
            } else if (tag === 'BUTTON') {
                if (submitter && c === submitter && c.type !== 'reset' && c.type !== 'button') {
                    this._entries.push({ name: name, value: String(c.value || '') });
                }
            }
        }
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
