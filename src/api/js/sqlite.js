// Node-compatible `sqlite` (`node:sqlite`) module over the native
// __brokit_sqlite_* bindings (sqlite.cpp).

(function() {
    'use strict';

    function StatementSync(db, sql) {
        this._db = db;
        this.sourceSQL = String(sql);
    }

    StatementSync.prototype._ensureOpen = function() {
        if (!this._db || !this._db.isOpen) {
            throw new Error('Database is not open');
        }
    };

    StatementSync.prototype.run = function() {
        this._ensureOpen();
        var args = [];
        for (var i = 0; i < arguments.length; i++) args.push(arguments[i]);
        return globalThis.__brokit_sqlite_query(this._db._id, 'run', this.sourceSQL, args);
    };

    StatementSync.prototype.get = function() {
        this._ensureOpen();
        var args = [];
        for (var i = 0; i < arguments.length; i++) args.push(arguments[i]);
        return globalThis.__brokit_sqlite_query(this._db._id, 'get', this.sourceSQL, args);
    };

    StatementSync.prototype.all = function() {
        this._ensureOpen();
        var args = [];
        for (var i = 0; i < arguments.length; i++) args.push(arguments[i]);
        return globalThis.__brokit_sqlite_query(this._db._id, 'all', this.sourceSQL, args);
    };

    function DatabaseSync(location) {
        if (!(this instanceof DatabaseSync)) {
            return new DatabaseSync(location);
        }
        var loc = (location === undefined || location === null) ? ':memory:' : String(location);
        this.location = loc;
        this._id = globalThis.__brokit_sqlite_open(loc);
        this.isOpen = true;
    }

    DatabaseSync.prototype.exec = function(sql) {
        if (!this.isOpen) throw new Error('Database is not open');
        globalThis.__brokit_sqlite_exec(this._id, String(sql));
    };

    DatabaseSync.prototype.prepare = function(sql) {
        if (!this.isOpen) throw new Error('Database is not open');
        if (typeof sql !== 'string' || !sql.trim()) {
            throw new TypeError('SQL string is required');
        }
        return new StatementSync(this, sql);
    };

    DatabaseSync.prototype.transaction = function(fn) {
        if (typeof fn !== 'function') throw new TypeError('transaction() expects a function');
        var self = this;
        return function() {
            if (!self.isOpen) throw new Error('Database is not open');
            self.exec('BEGIN IMMEDIATE');
            try {
                var res = fn.apply(this, arguments);
                self.exec('COMMIT');
                return res;
            } catch (e) {
                try { self.exec('ROLLBACK'); } catch (_) {}
                throw e;
            }
        };
    };

    DatabaseSync.prototype.close = function() {
        if (!this.isOpen) return;
        globalThis.__brokit_sqlite_close(this._id);
        this._id = -1;
        this.isOpen = false;
    };

    var sqliteModule = {
        DatabaseSync: DatabaseSync,
        Database: DatabaseSync,
        StatementSync: StatementSync
    };

    globalThis.__brokit_modules = globalThis.__brokit_modules || {};
    globalThis.__brokit_modules['sqlite'] = sqliteModule;
})();
