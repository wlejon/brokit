(function() {
    // Wrap stat/lstat/dirent results to add isFile()/isDirectory()/isSymbolicLink() methods
    function wrapStats(raw) {
        return {
            size: raw.size,
            mtimeMs: raw.mtimeMs || 0,
            mode: raw.mode || 0,
            isFile: function() { return !!raw._isFile; },
            isDirectory: function() { return !!raw._isDirectory; },
            isSymbolicLink: function() { return !!raw._isSymbolicLink; },
            isBlockDevice: function() { return false; },
            isCharacterDevice: function() { return false; },
            isFIFO: function() { return false; },
            isSocket: function() { return false; },
            // mtime as Date
            get mtime() { return new Date(this.mtimeMs); }
        };
    }

    function wrapDirent(raw) {
        return {
            name: raw.name,
            isFile: function() { return !!raw._isFile; },
            isDirectory: function() { return !!raw._isDirectory; },
            isSymbolicLink: function() { return !!raw._isSymbolicLink; },
            isBlockDevice: function() { return false; },
            isCharacterDevice: function() { return false; },
            isFIFO: function() { return false; },
            isSocket: function() { return false; }
        };
    }

    // ── Sync API ──────────────────────────────────────────────────────────────

    function readFileSync(path, encoding) {
        return globalThis.__brokit_fs_readFileSync(path, encoding);
    }

    function writeFileSync(path, data, encoding) {
        return globalThis.__brokit_fs_writeFileSync(path, data, encoding);
    }

    function appendFileSync(path, data, encoding) {
        return globalThis.__brokit_fs_appendFileSync(path, data, encoding);
    }

    function statSync(path) {
        return wrapStats(globalThis.__brokit_fs_statSync(path));
    }

    function lstatSync(path) {
        return wrapStats(globalThis.__brokit_fs_lstatSync(path));
    }

    function readdirSync(path, options) {
        var raw = globalThis.__brokit_fs_readdirSync(path, options);
        if (options && options.withFileTypes) {
            return raw.map(function(entry) { return wrapDirent(entry); });
        }
        return raw;
    }

    function existsSync(path) {
        return globalThis.__brokit_fs_existsSync(path);
    }

    function mkdirSync(path, options) {
        return globalThis.__brokit_fs_mkdirSync(path, options);
    }

    function rmdirSync(path) {
        return globalThis.__brokit_fs_rmdirSync(path);
    }

    function rmSync(path, options) {
        return globalThis.__brokit_fs_rmSync(path, options);
    }

    function unlinkSync(path) {
        return globalThis.__brokit_fs_unlinkSync(path);
    }

    function renameSync(oldPath, newPath) {
        return globalThis.__brokit_fs_renameSync(oldPath, newPath);
    }

    function copyFileSync(src, dest) {
        return globalThis.__brokit_fs_copyFileSync(src, dest);
    }

    function chmodSync(path, mode) {
        return globalThis.__brokit_fs_chmodSync(path, mode);
    }

    function realpathSync(path) {
        return globalThis.__brokit_fs_realpathSync(path);
    }

    // ── Async (Promise) wrappers ──────────────────────────────────────────────
    // Node.js fs callback-style functions: fs.readFile(path, enc, callback)
    // We support both callback and promise style.

    function wrapAsync(syncFn) {
        return function() {
            var args = Array.prototype.slice.call(arguments);
            var callback = typeof args[args.length - 1] === 'function' ? args.pop() : null;
            try {
                var result = syncFn.apply(null, args);
                if (callback) {
                    callback(null, result);
                } else {
                    return Promise.resolve(result);
                }
            } catch (err) {
                if (callback) {
                    callback(err);
                } else {
                    return Promise.reject(err);
                }
            }
        };
    }

    // ── fs.promises namespace ─────────────────────────────────────────────────

    function promisify(syncFn) {
        return function() {
            var args = Array.prototype.slice.call(arguments);
            try {
                return Promise.resolve(syncFn.apply(null, args));
            } catch (err) {
                return Promise.reject(err);
            }
        };
    }

    var promises = {
        readFile:    promisify(readFileSync),
        writeFile:   promisify(writeFileSync),
        appendFile:  promisify(appendFileSync),
        stat:        promisify(statSync),
        lstat:       promisify(lstatSync),
        readdir:     promisify(readdirSync),
        mkdir:       promisify(mkdirSync),
        rmdir:       promisify(rmdirSync),
        rm:          promisify(rmSync),
        unlink:      promisify(unlinkSync),
        rename:      promisify(renameSync),
        copyFile:    promisify(copyFileSync),
        chmod:       promisify(chmodSync),
        realpath:    promisify(realpathSync),
        access:      function(path) {
            return existsSync(path)
                ? Promise.resolve()
                : Promise.reject(Object.assign(new Error("ENOENT: no such file or directory, access '" + path + "'"), { code: 'ENOENT' }));
        }
    };

    // ── Main fs object ────────────────────────────────────────────────────────

    var fs = {
        // Sync
        readFileSync:    readFileSync,
        writeFileSync:   writeFileSync,
        appendFileSync:  appendFileSync,
        statSync:        statSync,
        lstatSync:       lstatSync,
        readdirSync:     readdirSync,
        existsSync:      existsSync,
        mkdirSync:       mkdirSync,
        rmdirSync:       rmdirSync,
        rmSync:          rmSync,
        unlinkSync:      unlinkSync,
        renameSync:      renameSync,
        copyFileSync:    copyFileSync,
        chmodSync:       chmodSync,
        realpathSync:    realpathSync,

        // Async (callback or Promise)
        readFile:    wrapAsync(readFileSync),
        writeFile:   wrapAsync(writeFileSync),
        appendFile:  wrapAsync(appendFileSync),
        stat:        wrapAsync(statSync),
        lstat:       wrapAsync(lstatSync),
        readdir:     wrapAsync(readdirSync),
        mkdir:       wrapAsync(mkdirSync),
        rmdir:       wrapAsync(rmdirSync),
        rm:          wrapAsync(rmSync),
        unlink:      wrapAsync(unlinkSync),
        rename:      wrapAsync(renameSync),
        copyFile:    wrapAsync(copyFileSync),
        chmod:       wrapAsync(chmodSync),
        realpath:    wrapAsync(realpathSync),

        // Convenience
        existsSync:  existsSync,

        // Promises namespace
        promises: promises,

        // Constants (subset)
        constants: {
            F_OK: 0,
            R_OK: 4,
            W_OK: 2,
            X_OK: 1,
            COPYFILE_EXCL: 1,
            COPYFILE_FICLONE: 2,
            COPYFILE_FICLONE_FORCE: 4
        }
    };

    // Expose on globalThis
    globalThis.__brokit_fs = fs;
})();
