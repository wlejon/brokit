(function() {
    'use strict';

    // ── IDBKeyRange ──────────────────────────────────────────────────────────

    function cmpKeys(a, b) {
        if (typeof a === 'number' && typeof b === 'number') {
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        var sa = String(a);
        var sb = String(b);
        return sa < sb ? -1 : (sa > sb ? 1 : 0);
    }

    function IDBKeyRange(lower, upper, lowerOpen, upperOpen) {
        this.lower = lower;
        this.upper = upper;
        this.lowerOpen = !!lowerOpen;
        this.upperOpen = !!upperOpen;
    }

    IDBKeyRange.only = function(value) {
        if (value === undefined) throw new TypeError('The parameter is not a valid key.');
        return new IDBKeyRange(value, value, false, false);
    };

    IDBKeyRange.lowerBound = function(lower, open) {
        if (lower === undefined) throw new TypeError('The parameter is not a valid key.');
        return new IDBKeyRange(lower, undefined, !!open, true);
    };

    IDBKeyRange.upperBound = function(upper, open) {
        if (upper === undefined) throw new TypeError('The parameter is not a valid key.');
        return new IDBKeyRange(undefined, upper, true, !!open);
    };

    IDBKeyRange.bound = function(lower, upper, lowerOpen, upperOpen) {
        if (lower === undefined || upper === undefined) {
            throw new TypeError('The parameter is not a valid key.');
        }
        return new IDBKeyRange(lower, upper, !!lowerOpen, !!upperOpen);
    };

    IDBKeyRange.prototype.includes = function(key) {
        if (key === undefined) return false;
        if (this.lower !== undefined) {
            var c = cmpKeys(key, this.lower);
            if (this.lowerOpen ? c <= 0 : c < 0) return false;
        }
        if (this.upper !== undefined) {
            var c2 = cmpKeys(key, this.upper);
            if (this.upperOpen ? c2 >= 0 : c2 > 0) return false;
        }
        return true;
    };

    function _matchKey(query, val) {
        if (query === undefined || query === null) return true;
        if (query instanceof IDBKeyRange) return query.includes(val);
        return val === query || String(val) === String(query);
    }

    // ── IDBCursor ────────────────────────────────────────────────────────────

    function IDBCursor(source, direction, request, items, isIndex) {
        this.source = source;
        this.direction = direction || 'next';
        this.request = request;
        this._items = items;
        this._index = 0;
        this._isIndex = !!isIndex;
        this._updatePos();
    }

    IDBCursor.prototype._updatePos = function() {
        if (this._index < this._items.length) {
            var item = this._items[this._index];
            this.key = item.key;
            this.primaryKey = item.primaryKey;
        } else {
            this.key = undefined;
            this.primaryKey = undefined;
        }
    };

    IDBCursor.prototype.continue = function(key) {
        var cursor = this;
        var req = cursor.request;
        req.readyState = 'pending';
        queueMicrotask(function() {
            if (key !== undefined) {
                while (cursor._index < cursor._items.length) {
                    cursor._index++;
                    if (cursor._index < cursor._items.length) {
                        var k = cursor._items[cursor._index].key;
                        var cmp = cmpKeys(k, key);
                        if (cursor.direction.indexOf('prev') === 0 ? cmp <= 0 : cmp >= 0) break;
                    }
                }
            } else {
                cursor._index++;
            }
            if (cursor._index < cursor._items.length) {
                cursor._updatePos();
                req._resolve(cursor);
            } else {
                req._resolve(null);
            }
        });
    };

    IDBCursor.prototype.advance = function(count) {
        if (typeof count !== 'number' || count <= 0) throw new TypeError('Count must be a positive number');
        var cursor = this;
        var req = cursor.request;
        req.readyState = 'pending';
        queueMicrotask(function() {
            cursor._index += count;
            if (cursor._index < cursor._items.length) {
                cursor._updatePos();
                req._resolve(cursor);
            } else {
                req._resolve(null);
            }
        });
    };

    IDBCursor.prototype.update = function(value) {
        if (!this.source || this._index >= this._items.length) {
            throw new DOMException('The cursor is not positioned on a record', 'InvalidStateError');
        }
        var store = this._isIndex ? this.source.objectStore : this.source;
        return store.put(value, this.primaryKey);
    };

    IDBCursor.prototype.delete = function() {
        if (!this.source || this._index >= this._items.length) {
            throw new DOMException('The cursor is not positioned on a record', 'InvalidStateError');
        }
        var store = this._isIndex ? this.source.objectStore : this.source;
        return store.delete(this.primaryKey);
    };

    // ── IDBCursorWithValue ───────────────────────────────────────────────────

    function IDBCursorWithValue(source, direction, request, items, isIndex) {
        IDBCursor.call(this, source, direction, request, items, isIndex);
    }
    IDBCursorWithValue.prototype = Object.create(IDBCursor.prototype);
    IDBCursorWithValue.prototype.constructor = IDBCursorWithValue;

    Object.defineProperty(IDBCursorWithValue.prototype, 'value', {
        get: function() {
            if (this._index < this._items.length) {
                return this._items[this._index].value;
            }
            return undefined;
        }
    });

    // ── IDBRequest ───────────────────────────────────────────────────────────

    function IDBRequest() {
        this.result = undefined;
        this.error = null;
        this.source = null;
        this.transaction = null;
        this.readyState = 'pending';
        this.onsuccess = null;
        this.onerror = null;
    }

    IDBRequest.prototype._resolve = function(value) {
        this.result = value;
        this.readyState = 'done';
        if (this.onsuccess) {
            try { this.onsuccess({ type: 'success', target: this }); } catch (e) {}
        }
    };

    IDBRequest.prototype._reject = function(error) {
        this.error = error;
        this.readyState = 'done';
        if (this.onerror) {
            try { this.onerror({ type: 'error', target: this }); } catch (e) {}
        }
        if (this.transaction && !this.transaction._aborted && !this.transaction._completed) {
            var tx = this.transaction;
            tx.error = error;
            tx._rollback();
            tx._aborted = true;
            tx._completed = true;
            queueMicrotask(function() {
                if (tx.onerror) {
                    try { tx.onerror({ type: 'error', target: tx }); } catch (e) {}
                }
                if (tx.onabort) {
                    try { tx.onabort({ type: 'abort', target: tx }); } catch (e) {}
                }
            });
        }
    };

    // ── IDBOpenDBRequest ─────────────────────────────────────────────────────

    function IDBOpenDBRequest() {
        IDBRequest.call(this);
        this.onupgradeneeded = null;
        this.onblocked = null;
    }
    IDBOpenDBRequest.prototype = Object.create(IDBRequest.prototype);
    IDBOpenDBRequest.prototype.constructor = IDBOpenDBRequest;

    // ── IDBIndex ─────────────────────────────────────────────────────────────

    function _matchIndexKey(query, val) {
        if (query === undefined) return true;
        if (query instanceof IDBKeyRange) return query.includes(val);
        return val === query || String(val) === String(query);
    }

    function IDBIndex(objectStore, name, keyPath, options) {
        this.objectStore = objectStore;
        this.name = name;
        this.keyPath = keyPath;
        options = options || {};
        this.multiEntry = !!options.multiEntry;
        this.unique = !!options.unique;
    }

    IDBIndex.prototype.get = function(query) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.objectStore ? this.objectStore.transaction : null;
        var dbName = this.objectStore ? this.objectStore._dbName : '';
        var storeName = this.objectStore ? this.objectStore.name : '';
        var keyPath = this.keyPath;

        try {
            var pairs = globalThis.__brokit_idb_get_all(dbName, storeName, 0) || [];
            var matched = undefined;
            for (var i = 0; i < pairs.length; i++) {
                try {
                    var obj = JSON.parse(pairs[i][1]);
                    if (obj && _matchIndexKey(query, obj[keyPath])) {
                        matched = obj;
                        break;
                    }
                } catch (e) {}
            }
            queueMicrotask(function() { req._resolve(matched); });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBIndex.prototype.getKey = function(query) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.objectStore ? this.objectStore.transaction : null;
        var dbName = this.objectStore ? this.objectStore._dbName : '';
        var storeName = this.objectStore ? this.objectStore.name : '';
        var keyPath = this.keyPath;

        try {
            var pairs = globalThis.__brokit_idb_get_all(dbName, storeName, 0) || [];
            var matchedKey = undefined;
            for (var i = 0; i < pairs.length; i++) {
                try {
                    var obj = JSON.parse(pairs[i][1]);
                    if (obj && _matchIndexKey(query, obj[keyPath])) {
                        matchedKey = pairs[i][0];
                        break;
                    }
                } catch (e) {}
            }
            queueMicrotask(function() { req._resolve(matchedKey); });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBIndex.prototype.getAll = function(query, count) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.objectStore ? this.objectStore.transaction : null;
        var dbName = this.objectStore ? this.objectStore._dbName : '';
        var storeName = this.objectStore ? this.objectStore.name : '';
        var keyPath = this.keyPath;
        var limit = (typeof count === 'number' && count > 0) ? count : Infinity;

        try {
            var pairs = globalThis.__brokit_idb_get_all(dbName, storeName, 0) || [];
            var results = [];
            for (var i = 0; i < pairs.length && results.length < limit; i++) {
                try {
                    var obj = JSON.parse(pairs[i][1]);
                    if (obj && _matchIndexKey(query, obj[keyPath])) {
                        results.push(obj);
                    }
                } catch (e) {}
            }
            queueMicrotask(function() { req._resolve(results); });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBIndex.prototype.getAllKeys = function(query, count) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.objectStore ? this.objectStore.transaction : null;
        var dbName = this.objectStore ? this.objectStore._dbName : '';
        var storeName = this.objectStore ? this.objectStore.name : '';
        var keyPath = this.keyPath;
        var limit = (typeof count === 'number' && count > 0) ? count : Infinity;

        try {
            var pairs = globalThis.__brokit_idb_get_all(dbName, storeName, 0) || [];
            var keys = [];
            for (var i = 0; i < pairs.length && keys.length < limit; i++) {
                try {
                    var obj = JSON.parse(pairs[i][1]);
                    if (obj && _matchIndexKey(query, obj[keyPath])) {
                        keys.push(pairs[i][0]);
                    }
                } catch (e) {}
            }
            queueMicrotask(function() { req._resolve(keys); });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBIndex.prototype.count = function(query) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.objectStore ? this.objectStore.transaction : null;
        var dbName = this.objectStore ? this.objectStore._dbName : '';
        var storeName = this.objectStore ? this.objectStore.name : '';
        var keyPath = this.keyPath;

        try {
            var pairs = globalThis.__brokit_idb_get_all(dbName, storeName, 0) || [];
            var count = 0;
            for (var i = 0; i < pairs.length; i++) {
                try {
                    var obj = JSON.parse(pairs[i][1]);
                    if (obj && _matchIndexKey(query, obj[keyPath])) {
                        count++;
                    }
                } catch (e) {}
            }
            queueMicrotask(function() { req._resolve(count); });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBIndex.prototype.openCursor = function(query, direction) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.objectStore ? this.objectStore.transaction : null;
        var dir = direction || 'next';
        var dbName = this.objectStore ? this.objectStore._dbName : '';
        var storeName = this.objectStore ? this.objectStore.name : '';
        var keyPath = this.keyPath;
        var self = this;

        try {
            var pairs = globalThis.__brokit_idb_get_all(dbName, storeName, 0) || [];
            var items = [];
            for (var i = 0; i < pairs.length; i++) {
                var pk = pairs[i][0];
                var obj = undefined;
                try { obj = JSON.parse(pairs[i][1]); } catch (e) {}
                if (obj && obj[keyPath] !== undefined) {
                    var idxKey = obj[keyPath];
                    if (_matchKey(query, idxKey)) {
                        items.push({ key: idxKey, primaryKey: pk, value: obj });
                    }
                }
            }
            items.sort(function(a, b) {
                var c = cmpKeys(a.key, b.key);
                return c !== 0 ? c : cmpKeys(a.primaryKey, b.primaryKey);
            });
            if (dir === 'prev' || dir === 'prevunique') items.reverse();
            if (dir === 'nextunique' || dir === 'prevunique') {
                var uniq = [];
                var lastKey = undefined;
                for (var j = 0; j < items.length; j++) {
                    if (j === 0 || cmpKeys(items[j].key, lastKey) !== 0) {
                        uniq.push(items[j]);
                        lastKey = items[j].key;
                    }
                }
                items = uniq;
            }

            queueMicrotask(function() {
                if (items.length > 0) {
                    var cursor = new IDBCursorWithValue(self, dir, req, items, true);
                    req._resolve(cursor);
                } else {
                    req._resolve(null);
                }
            });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBIndex.prototype.openKeyCursor = function(query, direction) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.objectStore ? this.objectStore.transaction : null;
        var dir = direction || 'next';
        var dbName = this.objectStore ? this.objectStore._dbName : '';
        var storeName = this.objectStore ? this.objectStore.name : '';
        var keyPath = this.keyPath;
        var self = this;

        try {
            var pairs = globalThis.__brokit_idb_get_all(dbName, storeName, 0) || [];
            var items = [];
            for (var i = 0; i < pairs.length; i++) {
                var pk = pairs[i][0];
                var obj = undefined;
                try { obj = JSON.parse(pairs[i][1]); } catch (e) {}
                if (obj && obj[keyPath] !== undefined) {
                    var idxKey = obj[keyPath];
                    if (_matchKey(query, idxKey)) {
                        items.push({ key: idxKey, primaryKey: pk, value: undefined });
                    }
                }
            }
            items.sort(function(a, b) {
                var c = cmpKeys(a.key, b.key);
                return c !== 0 ? c : cmpKeys(a.primaryKey, b.primaryKey);
            });
            if (dir === 'prev' || dir === 'prevunique') items.reverse();
            if (dir === 'nextunique' || dir === 'prevunique') {
                var uniq = [];
                var lastKey = undefined;
                for (var j = 0; j < items.length; j++) {
                    if (j === 0 || cmpKeys(items[j].key, lastKey) !== 0) {
                        uniq.push(items[j]);
                        lastKey = items[j].key;
                    }
                }
                items = uniq;
            }

            queueMicrotask(function() {
                if (items.length > 0) {
                    var cursor = new IDBCursor(self, dir, req, items, true);
                    req._resolve(cursor);
                } else {
                    req._resolve(null);
                }
            });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    // ── IDBObjectStore ───────────────────────────────────────────────────────

    function IDBObjectStore(dbName, storeName, transaction, db) {
        this.name = storeName;
        this._dbName = dbName;
        this._db = db;
        this.transaction = transaction;
        this.keyPath = null;
        this.autoIncrement = false;
        this._indexes = {};
        this.indexNames = [];

        if (db && db._storeMeta && db._storeMeta[storeName]) {
            var meta = db._storeMeta[storeName];
            this.keyPath = meta.keyPath || null;
            this.autoIncrement = !!meta.autoIncrement;
            var idxMeta = meta.indexes || {};
            for (var idxName in idxMeta) {
                var im = idxMeta[idxName];
                var index = new IDBIndex(this, im.name, im.keyPath, im.options);
                this._indexes[im.name] = index;
                this.indexNames.push(im.name);
            }
        }
    }

    IDBObjectStore.prototype.createIndex = function(name, keyPath, options) {
        var index = new IDBIndex(this, name, keyPath, options);
        this._indexes[name] = index;
        if (this.indexNames.indexOf(name) === -1) {
            this.indexNames.push(name);
        }
        if (this._db && this._db._storeMeta && this._db._storeMeta[this.name]) {
            this._db._storeMeta[this.name].indexes[name] = {
                name: name,
                keyPath: keyPath,
                options: options
            };
        }
        return index;
    };

    IDBObjectStore.prototype.index = function(name) {
        var idx = this._indexes[name];
        if (!idx) {
            throw new DOMException('Index not found: ' + name, 'NotFoundError');
        }
        return idx;
    };

    IDBObjectStore.prototype.deleteIndex = function(name) {
        delete this._indexes[name];
        var pos = this.indexNames.indexOf(name);
        if (pos !== -1) this.indexNames.splice(pos, 1);
        if (this._db && this._db._storeMeta && this._db._storeMeta[this.name]) {
            delete this._db._storeMeta[this.name].indexes[name];
        }
    };

    IDBObjectStore.prototype.put = function(value, key) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.transaction;
        var k = (key !== undefined) ? String(key) : (this.keyPath ? String(value[this.keyPath]) : String(key));
        var dbName = this._dbName;
        var storeName = this.name;
        var tx = this.transaction;

        try {
            if (tx && tx.mode === 'readwrite' && tx._undoLog) {
                var prevVal = globalThis.__brokit_idb_get(dbName, storeName, k);
                if (prevVal !== undefined) {
                    tx._undoLog.push(function() {
                        globalThis.__brokit_idb_put(dbName, storeName, k, prevVal);
                    });
                } else {
                    tx._undoLog.push(function() {
                        globalThis.__brokit_idb_delete(dbName, storeName, k);
                    });
                }
            }
            var valStr = JSON.stringify(value);
            var resultKey = globalThis.__brokit_idb_put(dbName, storeName, k, valStr);
            queueMicrotask(function() { req._resolve(resultKey); });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBObjectStore.prototype.add = function(value, key) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.transaction;
        var k = (key !== undefined) ? String(key) : (this.keyPath ? String(value[this.keyPath]) : String(key));
        var dbName = this._dbName;
        var storeName = this.name;
        var tx = this.transaction;

        try {
            var existing = globalThis.__brokit_idb_get(dbName, storeName, k);
            if (existing !== undefined) {
                queueMicrotask(function() {
                    req._reject(new DOMException('Key already exists', 'ConstraintError'));
                });
            } else {
                if (tx && tx.mode === 'readwrite' && tx._undoLog) {
                    tx._undoLog.push(function() {
                        globalThis.__brokit_idb_delete(dbName, storeName, k);
                    });
                }
                var valStr = JSON.stringify(value);
                var resultKey = globalThis.__brokit_idb_put(dbName, storeName, k, valStr);
                queueMicrotask(function() { req._resolve(resultKey); });
            }
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBObjectStore.prototype.get = function(key) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.transaction;
        var dbName = this._dbName;
        var storeName = this.name;
        try {
            var valStr = globalThis.__brokit_idb_get(dbName, storeName, String(key));
            var result = (valStr !== undefined) ? JSON.parse(valStr) : undefined;
            queueMicrotask(function() { req._resolve(result); });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBObjectStore.prototype.delete = function(key) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.transaction;
        var dbName = this._dbName;
        var storeName = this.name;
        var k = String(key);
        var tx = this.transaction;

        try {
            if (tx && tx.mode === 'readwrite' && tx._undoLog) {
                var prevVal = globalThis.__brokit_idb_get(dbName, storeName, k);
                if (prevVal !== undefined) {
                    tx._undoLog.push(function() {
                        globalThis.__brokit_idb_put(dbName, storeName, k, prevVal);
                    });
                }
            }
            globalThis.__brokit_idb_delete(dbName, storeName, k);
            queueMicrotask(function() { req._resolve(undefined); });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBObjectStore.prototype.clear = function() {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.transaction;
        var dbName = this._dbName;
        var storeName = this.name;
        var tx = this.transaction;

        try {
            if (tx && tx.mode === 'readwrite' && tx._undoLog) {
                var prevPairs = globalThis.__brokit_idb_get_all(dbName, storeName, 0) || [];
                tx._undoLog.push(function() {
                    for (var i = 0; i < prevPairs.length; i++) {
                        globalThis.__brokit_idb_put(dbName, storeName, prevPairs[i][0], prevPairs[i][1]);
                    }
                });
            }
            globalThis.__brokit_idb_clear(dbName, storeName);
            queueMicrotask(function() { req._resolve(undefined); });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBObjectStore.prototype.getAll = function(query, count) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.transaction;
        var dbName = this._dbName;
        var storeName = this.name;
        var limit = (typeof count === 'number' && count > 0) ? count : Infinity;
        try {
            var pairs = globalThis.__brokit_idb_get_all(dbName, storeName, 0);
            var results = [];
            for (var i = 0; i < pairs.length && results.length < limit; i++) {
                var k = pairs[i][0];
                if (query instanceof IDBKeyRange && !query.includes(k)) continue;
                try { results.push(JSON.parse(pairs[i][1])); }
                catch (e2) { results.push(pairs[i][1]); }
            }
            queueMicrotask(function() { req._resolve(results); });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBObjectStore.prototype.getAllKeys = function(query, count) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.transaction;
        var dbName = this._dbName;
        var storeName = this.name;
        var limit = (typeof count === 'number' && count > 0) ? count : Infinity;
        try {
            var pairs = globalThis.__brokit_idb_get_all(dbName, storeName, 0);
            var keys = [];
            for (var i = 0; i < pairs.length && keys.length < limit; i++) {
                var k = pairs[i][0];
                if (query instanceof IDBKeyRange && !query.includes(k)) continue;
                keys.push(k);
            }
            queueMicrotask(function() { req._resolve(keys); });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBObjectStore.prototype.count = function(query) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.transaction;
        var dbName = this._dbName;
        var storeName = this.name;
        try {
            if (query instanceof IDBKeyRange) {
                var pairs = globalThis.__brokit_idb_get_all(dbName, storeName, 0);
                var c = 0;
                for (var i = 0; i < pairs.length; i++) {
                    if (query.includes(pairs[i][0])) c++;
                }
                queueMicrotask(function() { req._resolve(c); });
            } else {
                var c = globalThis.__brokit_idb_count(dbName, storeName);
                queueMicrotask(function() { req._resolve(c); });
            }
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBObjectStore.prototype.openCursor = function(query, direction) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.transaction;
        var dir = direction || 'next';
        var self = this;
        try {
            var pairs = globalThis.__brokit_idb_get_all(this._dbName, this.name, 0) || [];
            var items = [];
            for (var i = 0; i < pairs.length; i++) {
                var pk = pairs[i][0];
                var val = undefined;
                try { val = JSON.parse(pairs[i][1]); } catch (e) { val = pairs[i][1]; }
                var effectiveKey = (val && self.keyPath && val[self.keyPath] !== undefined) ? val[self.keyPath] : pk;
                if (_matchKey(query, effectiveKey)) {
                    items.push({ key: effectiveKey, primaryKey: pk, value: val });
                }
            }
            items.sort(function(a, b) {
                var c = cmpKeys(a.key, b.key);
                return c !== 0 ? c : cmpKeys(a.primaryKey, b.primaryKey);
            });
            if (dir === 'prev' || dir === 'prevunique') items.reverse();
            if (dir === 'nextunique' || dir === 'prevunique') {
                var uniq = [];
                var lastKey = undefined;
                for (var j = 0; j < items.length; j++) {
                    if (j === 0 || cmpKeys(items[j].key, lastKey) !== 0) {
                        uniq.push(items[j]);
                        lastKey = items[j].key;
                    }
                }
                items = uniq;
            }
            queueMicrotask(function() {
                req._resolve(items.length > 0 ? new IDBCursorWithValue(self, dir, req, items, false) : null);
            });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBObjectStore.prototype.openKeyCursor = function(query, direction) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.transaction;
        var dir = direction || 'next';
        var self = this;
        try {
            var pairs = globalThis.__brokit_idb_get_all(this._dbName, this.name, 0) || [];
            var items = [];
            for (var i = 0; i < pairs.length; i++) {
                var pk = pairs[i][0];
                var val = undefined;
                try { val = JSON.parse(pairs[i][1]); } catch (e) { val = pairs[i][1]; }
                var effectiveKey = (val && self.keyPath && val[self.keyPath] !== undefined) ? val[self.keyPath] : pk;
                if (_matchKey(query, effectiveKey)) {
                    items.push({ key: effectiveKey, primaryKey: pk, value: undefined });
                }
            }
            items.sort(function(a, b) {
                var c = cmpKeys(a.key, b.key);
                return c !== 0 ? c : cmpKeys(a.primaryKey, b.primaryKey);
            });
            if (dir === 'prev' || dir === 'prevunique') items.reverse();
            if (dir === 'nextunique' || dir === 'prevunique') {
                var uniq = [];
                var lastKey = undefined;
                for (var j = 0; j < items.length; j++) {
                    if (j === 0 || cmpKeys(items[j].key, lastKey) !== 0) {
                        uniq.push(items[j]);
                        lastKey = items[j].key;
                    }
                }
                items = uniq;
            }
            queueMicrotask(function() {
                req._resolve(items.length > 0 ? new IDBCursor(self, dir, req, items, false) : null);
            });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    // ── IDBTransaction ───────────────────────────────────────────────────────

    function IDBTransaction(db, storeNames, mode) {
        this.db = db;
        this.mode = mode || 'readonly';
        this.objectStoreNames = storeNames;
        this.error = null;
        this.oncomplete = null;
        this.onerror = null;
        this.onabort = null;
        this._completed = false;
        this._aborted = false;
        this._undoLog = [];

        var self = this;
        queueMicrotask(function() {
            queueMicrotask(function() {
                if (!self._completed && !self._aborted) {
                    self._completed = true;
                    if (self.oncomplete) {
                        try { self.oncomplete({ type: 'complete', target: self }); } catch (e) {}
                    }
                }
            });
        });
    }

    IDBTransaction.prototype._rollback = function() {
        while (this._undoLog && this._undoLog.length > 0) {
            var undo = this._undoLog.pop();
            try { undo(); } catch (e) {}
        }
        this._undoLog = [];
    };

    IDBTransaction.prototype.objectStore = function(name) {
        return new IDBObjectStore(this.db.name, name, this, this.db);
    };

    IDBTransaction.prototype.abort = function(err) {
        if (this._aborted) return;
        this._aborted = true;
        this._completed = true;
        if (err) this.error = err;
        else if (!this.error) this.error = new DOMException('The transaction was aborted.', 'AbortError');
        this._rollback();
        var self = this;
        queueMicrotask(function() {
            if (self.onabort) {
                try { self.onabort({ type: 'abort', target: self }); } catch (e) {}
            }
        });
    };

    // ── IDBDatabase ──────────────────────────────────────────────────────────

    function IDBDatabase(name, version) {
        this.name = name;
        this.version = version;
        this.onclose = null;
        this.onversionchange = null;
        this._storeMeta = {};

        var names = globalThis.__brokit_idb_store_names(name);
        this.objectStoreNames = names || [];
        for (var i = 0; i < this.objectStoreNames.length; i++) {
            this._storeMeta[this.objectStoreNames[i]] = { indexes: {} };
        }
    }

    IDBDatabase.prototype.transaction = function(storeNames, mode) {
        if (typeof storeNames === 'string') storeNames = [storeNames];
        return new IDBTransaction(this, storeNames, mode);
    };

    IDBDatabase.prototype.createObjectStore = function(name, options) {
        options = options || {};
        globalThis.__brokit_idb_create_store(this.name, name, options);
        if (this.objectStoreNames.indexOf(name) === -1) {
            this.objectStoreNames.push(name);
        }
        this._storeMeta[name] = {
            keyPath: options.keyPath || null,
            autoIncrement: !!options.autoIncrement,
            indexes: {}
        };
        return new IDBObjectStore(this.name, name, null, this);
    };

    IDBDatabase.prototype.deleteObjectStore = function(name) {
        globalThis.__brokit_idb_delete_store(this.name, name);
        var idx = this.objectStoreNames.indexOf(name);
        if (idx !== -1) this.objectStoreNames.splice(idx, 1);
        delete this._storeMeta[name];
    };

    IDBDatabase.prototype.close = function() {
        if (this.onclose) {
            try { this.onclose({ type: 'close', target: this }); } catch (e) {}
        }
    };

    // ── IDBFactory (indexedDB global) ────────────────────────────────────────

    var indexedDB = {
        open: function(name, version) {
            var request = new IDBOpenDBRequest();

            try {
                var info = globalThis.__brokit_idb_open(name, version || 1);
                var db = new IDBDatabase(info.name, info.version);

                if (info.needsUpgrade) {
                    queueMicrotask(function() {
                        if (request.onupgradeneeded) {
                            var event = {
                                type: 'upgradeneeded',
                                target: request,
                                oldVersion: info.oldVersion,
                                newVersion: info.version
                            };
                            request.result = db;
                            try { request.onupgradeneeded(event); } catch (e) {}
                        }
                        // Refresh store names after upgrade
                        db.objectStoreNames = globalThis.__brokit_idb_store_names(db.name) || [];
                        for (var i = 0; i < db.objectStoreNames.length; i++) {
                            if (!db._storeMeta[db.objectStoreNames[i]]) {
                                db._storeMeta[db.objectStoreNames[i]] = { indexes: {} };
                            }
                        }
                        queueMicrotask(function() { request._resolve(db); });
                    });
                } else {
                    queueMicrotask(function() { request._resolve(db); });
                }
            } catch (e) {
                queueMicrotask(function() { request._reject(e); });
            }

            return request;
        },

        deleteDatabase: function(name) {
            var request = new IDBRequest();
            try {
                globalThis.__brokit_idb_delete_db(name);
                queueMicrotask(function() { request._resolve(undefined); });
            } catch (e) {
                queueMicrotask(function() { request._reject(e); });
            }
            return request;
        }
    };

    // Expose
    globalThis.indexedDB = indexedDB;
    globalThis.IDBRequest = IDBRequest;
    globalThis.IDBOpenDBRequest = IDBOpenDBRequest;
    globalThis.IDBDatabase = IDBDatabase;
    globalThis.IDBTransaction = IDBTransaction;
    globalThis.IDBObjectStore = IDBObjectStore;
    globalThis.IDBIndex = IDBIndex;
    globalThis.IDBCursor = IDBCursor;
    globalThis.IDBCursorWithValue = IDBCursorWithValue;
    globalThis.IDBKeyRange = IDBKeyRange;
})();
