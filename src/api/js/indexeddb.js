(function() {
    'use strict';

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
    };

    // ── IDBOpenDBRequest ─────────────────────────────────────────────────────

    function IDBOpenDBRequest() {
        IDBRequest.call(this);
        this.onupgradeneeded = null;
        this.onblocked = null;
    }
    IDBOpenDBRequest.prototype = Object.create(IDBRequest.prototype);
    IDBOpenDBRequest.prototype.constructor = IDBOpenDBRequest;

    // ── IDBObjectStore ───────────────────────────────────────────────────────

    function IDBObjectStore(dbName, storeName, transaction) {
        this.name = storeName;
        this._dbName = dbName;
        this.transaction = transaction;
        this.keyPath = null;
        this.autoIncrement = false;
        this.indexNames = [];
    }

    IDBObjectStore.prototype.put = function(value, key) {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.transaction;
        var k = (key !== undefined) ? String(key) : (this.keyPath ? String(value[this.keyPath]) : String(key));
        try {
            var valStr = JSON.stringify(value);
            var resultKey = globalThis.__brokit_idb_put(this._dbName, this.name, k, valStr);
            queueMicrotask(function() { req._resolve(resultKey); });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBObjectStore.prototype.add = function(value, key) {
        // add() fails if key already exists — check first
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.transaction;
        var k = (key !== undefined) ? String(key) : (this.keyPath ? String(value[this.keyPath]) : String(key));
        var dbName = this._dbName;
        var storeName = this.name;
        try {
            var existing = globalThis.__brokit_idb_get(dbName, storeName, k);
            if (existing !== undefined) {
                queueMicrotask(function() {
                    req._reject(new DOMException('Key already exists', 'ConstraintError'));
                });
            } else {
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
        try {
            globalThis.__brokit_idb_delete(dbName, storeName, String(key));
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
        try {
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
        try {
            var pairs = globalThis.__brokit_idb_get_all(dbName, storeName, count);
            var results = [];
            for (var i = 0; i < pairs.length; i++) {
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
        try {
            var pairs = globalThis.__brokit_idb_get_all(dbName, storeName, count);
            var keys = [];
            for (var i = 0; i < pairs.length; i++) keys.push(pairs[i][0]);
            queueMicrotask(function() { req._resolve(keys); });
        } catch (e) {
            queueMicrotask(function() { req._reject(e); });
        }
        return req;
    };

    IDBObjectStore.prototype.count = function() {
        var req = new IDBRequest();
        req.source = this;
        req.transaction = this.transaction;
        var dbName = this._dbName;
        var storeName = this.name;
        try {
            var c = globalThis.__brokit_idb_count(dbName, storeName);
            queueMicrotask(function() { req._resolve(c); });
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

        // Auto-complete after microtask drain
        var self = this;
        queueMicrotask(function() {
            queueMicrotask(function() {
                if (!self._completed) {
                    self._completed = true;
                    if (self.oncomplete) {
                        try { self.oncomplete({ type: 'complete', target: self }); } catch (e) {}
                    }
                }
            });
        });
    }

    IDBTransaction.prototype.objectStore = function(name) {
        return new IDBObjectStore(this.db.name, name, this);
    };

    IDBTransaction.prototype.abort = function() {
        this._completed = true;
        if (this.onabort) {
            try { this.onabort({ type: 'abort', target: this }); } catch (e) {}
        }
    };

    // ── IDBDatabase ──────────────────────────────────────────────────────────

    function IDBDatabase(name, version) {
        this.name = name;
        this.version = version;
        this.onclose = null;
        this.onversionchange = null;

        // Get store names
        var names = globalThis.__brokit_idb_store_names(name);
        this.objectStoreNames = names || [];
    }

    IDBDatabase.prototype.transaction = function(storeNames, mode) {
        if (typeof storeNames === 'string') storeNames = [storeNames];
        return new IDBTransaction(this, storeNames, mode);
    };

    IDBDatabase.prototype.createObjectStore = function(name, options) {
        globalThis.__brokit_idb_create_store(this.name, name, options);
        if (this.objectStoreNames.indexOf(name) === -1) {
            this.objectStoreNames.push(name);
        }
        return new IDBObjectStore(this.name, name, null);
    };

    IDBDatabase.prototype.deleteObjectStore = function(name) {
        globalThis.__brokit_idb_delete_store(this.name, name);
        var idx = this.objectStoreNames.indexOf(name);
        if (idx !== -1) this.objectStoreNames.splice(idx, 1);
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
})();
