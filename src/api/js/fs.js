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
        if (data && data._u8) data = data._u8;
        return globalThis.__brokit_fs_writeFileSync(path, data, encoding);
    }

    function appendFileSync(path, data, encoding) {
        if (data && data._u8) data = data._u8;
        return globalThis.__brokit_fs_appendFileSync(path, data, encoding);
    }

    function statSync(path) {
        return wrapStats(globalThis.__brokit_fs_statSync(path));
    }

    // ── File descriptors ──────────────────────────────────────────────────────
    // readSync(fd, buffer, offset, length, position) is the only way to read
    // part of a file. `position` of null means "continue from the last read",
    // matching Node; a number seeks first.

    function openSync(path, flags) {
        return globalThis.__brokit_fs_openSync(path, flags === undefined ? 'r' : flags);
    }

    function readSync(fd, buffer, offset, length, position) {
        if (offset !== null && typeof offset === 'object') {
            // Node's options-object form: readSync(fd, buffer, { offset, length, position })
            var o = offset;
            return globalThis.__brokit_fs_readSync(
                fd, buffer,
                o.offset === undefined ? 0 : o.offset,
                o.length === undefined ? buffer.byteLength : o.length,
                o.position === undefined ? null : o.position);
        }
        return globalThis.__brokit_fs_readSync(
            fd, buffer,
            offset === undefined ? 0 : offset,
            length === undefined ? buffer.byteLength : length,
            position === undefined ? null : position);
    }

    // A string's bytes in a Node encoding (utf8 when none is named), by
    // Buffer's encoders where Buffer is installed.
    function stringBytes(str, encoding) {
        var B = globalThis.Buffer;
        if (typeof B === 'function' && B.from) return B.from(str, encoding || 'utf8')._u8;
        var enc = encoding ? String(encoding).toLowerCase() : 'utf8';
        if (enc !== 'utf8' && enc !== 'utf-8') throw new TypeError('Unknown encoding: ' + encoding);
        return new TextEncoder().encode(str);
    }

    // writeSync(fd, buffer[, offset[, length[, position]]])
    // writeSync(fd, buffer, { offset, length, position })
    // writeSync(fd, string[, position[, encoding]])
    function writeSync(fd, buffer, offset, length, position) {
        if (typeof buffer === 'string') {
            var bytes = stringBytes(buffer, typeof length === 'string' ? length : undefined);
            return globalThis.__brokit_fs_writeSync(
                fd, bytes, 0, bytes.byteLength,
                typeof offset === 'number' ? offset : null);
        }
        if (buffer === null || typeof buffer !== 'object') {
            throw new TypeError('writeSync: data must be a string, Buffer, TypedArray or DataView');
        }
        if (offset !== null && typeof offset === 'object') {
            var o = offset;
            offset = o.offset;
            length = o.length;
            position = o.position;
        }
        var off = offset === undefined || offset === null ? 0 : offset;
        return globalThis.__brokit_fs_writeSync(
            fd, buffer, off,
            length === undefined || length === null ? buffer.byteLength - off : length,
            position === undefined ? null : position);
    }

    function fsyncSync(fd) {
        globalThis.__brokit_fs_fsyncSync(fd, false);
    }

    function fdatasyncSync(fd) {
        globalThis.__brokit_fs_fsyncSync(fd, true);
    }

    function mkdtempSync(prefix) {
        return globalThis.__brokit_fs_mkdtempSync(prefix);
    }

    function fstatSync(fd) {
        return wrapStats(globalThis.__brokit_fs_fstatSync(fd));
    }

    function closeSync(fd) {
        return globalThis.__brokit_fs_closeSync(fd);
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

    // Node's cpSync: a file (its parent directories made), or a directory
    // tree with { recursive: true }. force (default true) overwrites a file
    // that exists; with force false it is left, or with errorOnExist throws
    // EEXIST.
    function cpSync(src, dest, options) {
        var o = options || {};
        var force = o.force !== false;
        var st = statSync(src);
        if (st.isDirectory()) {
            if (!o.recursive) {
                var e = new Error("cpSync: '" + src + "' is a directory (pass { recursive: true })");
                e.code = 'ERR_FS_EISDIR';
                throw e;
            }
            mkdirSync(dest, { recursive: true });
            var names = readdirSync(src);
            for (var i = 0; i < names.length; i++)
                cpSync(src + '/' + names[i], dest + '/' + names[i], o);
            return;
        }
        if (existsSync(dest)) {
            if (o.errorOnExist && !force) {
                var x = new Error("cpSync: '" + dest + "' exists");
                x.code = 'EEXIST';
                throw x;
            }
            if (!force) return;
        }
        var cut = Math.max(dest.lastIndexOf('/'), dest.lastIndexOf('\\'));
        if (cut > 0) mkdirSync(dest.slice(0, cut), { recursive: true });
        copyFileSync(src, dest);
    }

    function chmodSync(path, mode) {
        return globalThis.__brokit_fs_chmodSync(path, mode);
    }

    function realpathSync(path) {
        return globalThis.__brokit_fs_realpathSync(path);
    }

    // ── fs.promises namespace ─────────────────────────────────────────────────
    // Asynchronous for real: the I/O runs on brokit's fs threads and the
    // promise settles on this thread at its next frame (fs_async.cpp), so
    // awaiting a slow disk never stalls the page.

    function run(op, a, b, c) {
        return globalThis.__brokit_fs_async(op, a, b, c);
    }

    function encodingOf(options) {
        if (typeof options === 'string') return options;
        if (options && typeof options.encoding === 'string') return options.encoding;
        return '';
    }

    function bytesOf(data) {
        return data && data._u8 ? data._u8 : data;
    }

    var promises = {
        readFile:   function(path, options) { return run('readFile', path, encodingOf(options)); },
        writeFile:  function(path, data) { return run('writeFile', path, bytesOf(data)); },
        appendFile: function(path, data) { return run('appendFile', path, bytesOf(data)); },
        stat:       function(path) { return run('stat', path).then(wrapStats); },
        lstat:      function(path) { return run('lstat', path).then(wrapStats); },
        readdir:    function(path, options) {
            var types = !!(options && options.withFileTypes);
            return run('readdir', path, types).then(function(raw) {
                return types ? raw.map(wrapDirent) : raw;
            });
        },
        mkdir:      function(path, options) { return run('mkdir', path, !!(options && options.recursive)); },
        rmdir:      function(path) { return run('rmdir', path); },
        rm:         function(path, options) {
            return run('rm', path, !!(options && options.recursive), !!(options && options.force));
        },
        unlink:     function(path) { return run('unlink', path); },
        rename:     function(from, to) { return run('rename', from, to); },
        copyFile:   function(from, to) { return run('copyFile', from, to); },
        realpath:   function(path) { return run('realpath', path); },
        access:     function(path) { return run('access', path); },
        mkdtemp:    function(prefix) { return run('mkdtemp', prefix); },
        // chmod is a metadata write with nothing to wait on.
        chmod:      function(path, mode) {
            try { return Promise.resolve(chmodSync(path, mode)); } catch (err) { return Promise.reject(err); }
        }
    };

    // ── Callback functions: fs.readFile(path, enc, callback) ──────────────────
    // The callback runs later, never inside the call, as in Node; without a
    // callback the promise is returned.

    function wrapAsync(promiseFn) {
        return function() {
            var args = Array.prototype.slice.call(arguments);
            var callback = typeof args[args.length - 1] === 'function' ? args.pop() : null;
            var p = promiseFn.apply(null, args);
            if (!callback) return p;
            p.then(function(v) { callback(null, v); }, function(err) { callback(err); });
        };
    }

    // fs.write(fd, buffer, offset, length, position, callback), (fd, buffer,
    // options, callback) or (fd, string, position, encoding, callback):
    // callback(err, bytesWritten, data). A write lands in the fd's stream
    // buffer, so it runs in the call; the callback still runs later.
    function write(fd, data) {
        var args = Array.prototype.slice.call(arguments);
        var callback = typeof args[args.length - 1] === 'function' ? args.pop() : null;
        var p;
        try {
            p = Promise.resolve(writeSync.apply(null, args));
        } catch (err) {
            p = Promise.reject(err);
        }
        if (!callback) return p;
        p.then(function(n) { callback(null, n, data); }, function(err) { callback(err); });
    }

    // fsync / fdatasync: the stream's buffer reaches the OS in the call, the
    // wait for the disk runs on the fs threads.
    function fsyncAsync(fd) { return run('fsync', fd); }
    function fdatasyncAsync(fd) { return run('fdatasync', fd); }

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
        cpSync:          cpSync,
        chmodSync:       chmodSync,
        realpathSync:    realpathSync,

        // File descriptors
        openSync:        openSync,
        readSync:        readSync,
        writeSync:       writeSync,
        fstatSync:       fstatSync,
        closeSync:       closeSync,
        fsyncSync:       fsyncSync,
        fdatasyncSync:   fdatasyncSync,
        mkdtempSync:     mkdtempSync,
        write:           write,
        fsync:           wrapAsync(fsyncAsync),
        fdatasync:       wrapAsync(fdatasyncAsync),

        // Async (callback or Promise)
        readFile:    wrapAsync(promises.readFile),
        writeFile:   wrapAsync(promises.writeFile),
        appendFile:  wrapAsync(promises.appendFile),
        stat:        wrapAsync(promises.stat),
        lstat:       wrapAsync(promises.lstat),
        readdir:     wrapAsync(promises.readdir),
        mkdir:       wrapAsync(promises.mkdir),
        rmdir:       wrapAsync(promises.rmdir),
        rm:          wrapAsync(promises.rm),
        unlink:      wrapAsync(promises.unlink),
        rename:      wrapAsync(promises.rename),
        copyFile:    wrapAsync(promises.copyFile),
        chmod:       wrapAsync(promises.chmod),
        realpath:    wrapAsync(promises.realpath),
        mkdtemp:     wrapAsync(promises.mkdtemp),

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
