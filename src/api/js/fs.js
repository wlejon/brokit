(function() {
    // Wrap stat/lstat/fstat results to add isFile()/isDirectory()/isSymbolicLink() methods
    function wrapStats(raw) {
        var mtimeMs = raw.mtimeMs || 0;
        var atimeMs = raw.atimeMs !== undefined ? raw.atimeMs : mtimeMs;
        var ctimeMs = raw.ctimeMs !== undefined ? raw.ctimeMs : mtimeMs;
        var birthtimeMs = raw.birthtimeMs !== undefined ? raw.birthtimeMs : ctimeMs;
        return {
            size: raw.size,
            mtimeMs: mtimeMs,
            atimeMs: atimeMs,
            ctimeMs: ctimeMs,
            birthtimeMs: birthtimeMs,
            mode: raw.mode || 0,
            isFile: function() { return !!raw._isFile; },
            isDirectory: function() { return !!raw._isDirectory; },
            isSymbolicLink: function() { return !!raw._isSymbolicLink; },
            isBlockDevice: function() { return false; },
            isCharacterDevice: function() { return false; },
            isFIFO: function() { return false; },
            isSocket: function() { return false; },
            get mtime() { return new Date(this.mtimeMs); },
            get atime() { return new Date(this.atimeMs); },
            get ctime() { return new Date(this.ctimeMs); },
            get birthtime() { return new Date(this.birthtimeMs); }
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

    // Convert Node timestamp argument (number in Unix seconds, numeric string,
    // or Date object) to Unix epoch seconds (with fractional sub-second precision).
    function toUnixTimestamp(time, name) {
        if (typeof time === 'string') {
            var trimmed = time.trim();
            var num = Number(trimmed);
            if (trimmed !== '' && Number.isFinite(num)) return num;
        } else if (typeof time === 'number') {
            if (Number.isFinite(time)) return time;
        } else if (time instanceof Date || (time && typeof time === 'object' && typeof time.getTime === 'function')) {
            var ms = time.getTime();
            if (Number.isFinite(ms)) return ms / 1000;
        }
        var err = new TypeError(
            'The "' + (name || 'time') + '" argument must be of type number, string, or Date. Received ' + String(time));
        err.code = 'ERR_INVALID_ARG_TYPE';
        throw err;
    }

    function allocBuffer(size) {
        var B = globalThis.Buffer;
        if (typeof B === 'function' && B.alloc) return B.alloc(size);
        return new Uint8Array(size);
    }

    function wrapBuffer(u8) {
        var B = globalThis.Buffer;
        if (typeof B === 'function' && B._wrap) return B._wrap(u8);
        return u8;
    }

    function isBufferLike(v) {
        return Boolean(v && (v._isBuffer || v._u8 || ArrayBuffer.isView(v)));
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

    function openSync(path, flags, mode) {
        void mode;
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

    function decodeBytes(u8, encoding) {
        var B = globalThis.Buffer;
        if (typeof B === 'function' && B._wrap) return B._wrap(u8).toString(encoding);
        return new TextDecoder(encoding || 'utf-8').decode(u8);
    }

    // writeSync(fd, buffer[, offset[, length[, position]]])
    // writeSync(fd, buffer, { offset, length, position })
    // writeSync(fd, string[, position[, encoding]])
    function writeSync(fd, buffer, offset, length, position) {
        if (typeof buffer === 'string') {
            var enc = typeof offset === 'string' ? offset : (typeof length === 'string' ? length : undefined);
            var bytes = stringBytes(buffer, enc);
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

    function ftruncateSync(fd, len) {
        globalThis.__brokit_fs_ftruncateSync(fd, len === undefined ? 0 : len);
    }

    function truncateSync(path, len) {
        globalThis.__brokit_fs_truncateSync(path, len === undefined ? 0 : len);
    }

    function utimesSync(path, atime, mtime) {
        var a = toUnixTimestamp(atime, 'atime');
        var m = toUnixTimestamp(mtime, 'mtime');
        globalThis.__brokit_fs_utimesSync(path, a, m, true);
    }

    function lutimesSync(path, atime, mtime) {
        var a = toUnixTimestamp(atime, 'atime');
        var m = toUnixTimestamp(mtime, 'mtime');
        globalThis.__brokit_fs_utimesSync(path, a, m, false);
    }

    function futimesSync(fd, atime, mtime) {
        var a = toUnixTimestamp(atime, 'atime');
        var m = toUnixTimestamp(mtime, 'mtime');
        globalThis.__brokit_fs_futimesSync(fd, a, m);
    }

    function symlinkSync(target, path, type) {
        globalThis.__brokit_fs_symlinkSync(target, path, type);
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

    // ── fs.promises namespace & FileHandle ────────────────────────────────────
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

    function makeEbadf(syscall) {
        var err = new Error('EBADF: bad file descriptor, ' + syscall);
        err.code = 'EBADF';
        err.syscall = syscall;
        return err;
    }

    // Parse read arguments for FileHandle.read and fs.read:
    //   read()
    //   read(options)
    //   read(buffer, options)
    //   read(buffer, offset, length, position)
    function parseReadArgs(buffer, offset, length, position) {
        var buf = buffer;
        var off = offset;
        var len = length;
        var pos = position;

        if (buf === undefined || buf === null) {
            buf = allocBuffer(16384);
            off = 0;
            len = buf.byteLength;
            pos = null;
        } else if (!isBufferLike(buf) && typeof buf === 'object') {
            var opts = buf;
            buf = opts.buffer ? opts.buffer : allocBuffer(16384);
            off = opts.offset !== undefined && opts.offset !== null ? opts.offset : 0;
            len = opts.length !== undefined && opts.length !== null ? opts.length : (buf.byteLength - off);
            pos = opts.position !== undefined ? opts.position : null;
        } else if (off !== null && typeof off === 'object') {
            var o = off;
            off = o.offset !== undefined && o.offset !== null ? o.offset : 0;
            len = o.length !== undefined && o.length !== null ? o.length : (buf.byteLength - off);
            pos = o.position !== undefined ? o.position : null;
        } else {
            off = off === undefined || off === null ? 0 : off;
            len = len === undefined || len === null ? (buf.byteLength - off) : len;
            pos = pos === undefined ? null : pos;
        }
        return { buffer: buf, offset: off, length: len, position: pos };
    }

    function FileHandle(fd, path, flags) {
        this.fd = fd;
        this._path = path;
        this._flags = flags || 'r';
        this._closed = false;
    }

    FileHandle.prototype.read = function(buffer, offset, length, position) {
        if (this._closed || this.fd < 0) return Promise.reject(makeEbadf('read'));
        try {
            var parsed = parseReadArgs(buffer, offset, length, position);
            var bytesRead = readSync(this.fd, parsed.buffer, parsed.offset, parsed.length, parsed.position);
            return Promise.resolve({ bytesRead: bytesRead, buffer: parsed.buffer });
        } catch (err) {
            return Promise.reject(err);
        }
    };

    FileHandle.prototype.write = function(buffer, offset, length, position) {
        if (this._closed || this.fd < 0) return Promise.reject(makeEbadf('write'));
        try {
            var bytesWritten = writeSync(this.fd, buffer, offset, length, position);
            return Promise.resolve({ bytesWritten: bytesWritten, buffer: buffer });
        } catch (err) {
            return Promise.reject(err);
        }
    };

    FileHandle.prototype.readFile = function(options) {
        if (this._closed || this.fd < 0) return Promise.reject(makeEbadf('read'));
        try {
            var chunks = [];
            var total = 0;
            var pos = 0;
            for (;;) {
                var chunk = new Uint8Array(16384);
                var got = readSync(this.fd, chunk, 0, chunk.byteLength, pos);
                if (got <= 0) break;
                chunks.push(got === chunk.byteLength ? chunk : chunk.subarray(0, got));
                total += got;
                pos += got;
            }
            var out = new Uint8Array(total);
            var dstOff = 0;
            for (var i = 0; i < chunks.length; i++) {
                out.set(chunks[i], dstOff);
                dstOff += chunks[i].byteLength;
            }
            var enc = encodingOf(options);
            if (enc) {
                return Promise.resolve(decodeBytes(out, enc));
            }
            return Promise.resolve(wrapBuffer(out));
        } catch (err) {
            return Promise.reject(err);
        }
    };

    FileHandle.prototype.writeFile = function(data, options) {
        if (this._closed || this.fd < 0) return Promise.reject(makeEbadf('write'));
        try {
            var isAppend = this._flags.indexOf('a') === 0;
            if (!isAppend) {
                ftruncateSync(this.fd, 0);
            }
            if (typeof data === 'string') {
                writeSync(this.fd, data, isAppend ? null : 0, encodingOf(options) || 'utf8');
            } else {
                var b = bytesOf(data);
                writeSync(this.fd, b, 0, b.byteLength, isAppend ? null : 0);
            }
            return Promise.resolve(undefined);
        } catch (err) {
            return Promise.reject(err);
        }
    };

    FileHandle.prototype.appendFile = function(data, options) {
        if (this._closed || this.fd < 0) return Promise.reject(makeEbadf('write'));
        try {
            var st = fstatSync(this.fd);
            var endPos = st.size || 0;
            if (typeof data === 'string') {
                writeSync(this.fd, data, endPos, encodingOf(options) || 'utf8');
            } else {
                var b = bytesOf(data);
                writeSync(this.fd, b, 0, b.byteLength, endPos);
            }
            return Promise.resolve(undefined);
        } catch (err) {
            return Promise.reject(err);
        }
    };

    FileHandle.prototype.stat = function(options) {
        void options;
        if (this._closed || this.fd < 0) return Promise.reject(makeEbadf('fstat'));
        return run('fstat', this.fd).then(wrapStats);
    };

    FileHandle.prototype.truncate = function(len) {
        if (this._closed || this.fd < 0) return Promise.reject(makeEbadf('ftruncate'));
        return run('ftruncate', this.fd, len === undefined ? 0 : len);
    };

    FileHandle.prototype.datasync = function() {
        if (this._closed || this.fd < 0) return Promise.reject(makeEbadf('fdatasync'));
        return run('fdatasync', this.fd);
    };

    FileHandle.prototype.sync = function() {
        if (this._closed || this.fd < 0) return Promise.reject(makeEbadf('fsync'));
        return run('fsync', this.fd);
    };

    FileHandle.prototype.utimes = function(atime, mtime) {
        if (this._closed || this.fd < 0) return Promise.reject(makeEbadf('futime'));
        try {
            var a = toUnixTimestamp(atime, 'atime');
            var m = toUnixTimestamp(mtime, 'mtime');
            return run('futimes', this.fd, a, m);
        } catch (err) {
            return Promise.reject(err);
        }
    };

    FileHandle.prototype.close = function() {
        if (this._closed) return Promise.resolve(undefined);
        var fd = this.fd;
        this._closed = true;
        this.fd = -1;
        try {
            closeSync(fd);
            return Promise.resolve(undefined);
        } catch (err) {
            return Promise.reject(err);
        }
    };

    if (typeof Symbol === 'function') {
        if (!Symbol.asyncDispose) {
            try { Symbol.asyncDispose = Symbol.for('Symbol.asyncDispose'); } catch (e) {}
        }
        if (Symbol.asyncDispose) {
            FileHandle.prototype[Symbol.asyncDispose] = function() {
                return this.close();
            };
        }
    }

    function openFileHandle(path, flags, mode) {
        var f = flags === undefined ? 'r' : flags;
        try {
            var fd = openSync(path, f, mode);
            return Promise.resolve(new FileHandle(fd, path, f));
        } catch (err) {
            return Promise.reject(err);
        }
    }

    var promises = {
        open:       openFileHandle,
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
        truncate:   function(path, len) { return run('truncate', path, len === undefined ? 0 : len); },
        symlink:    function(target, path, type) { return run('symlink', target, path, type); },
        utimes:     function(path, atime, mtime) {
            try {
                var a = toUnixTimestamp(atime, 'atime');
                var m = toUnixTimestamp(mtime, 'mtime');
                return run('utimes', path, a, m);
            } catch (err) {
                return Promise.reject(err);
            }
        },
        lutimes:    function(path, atime, mtime) {
            try {
                var a = toUnixTimestamp(atime, 'atime');
                var m = toUnixTimestamp(mtime, 'mtime');
                return run('lutimes', path, a, m);
            } catch (err) {
                return Promise.reject(err);
            }
        },
        // chmod is a metadata write with nothing to wait on.
        chmod:      function(path, mode) {
            try { return Promise.resolve(chmodSync(path, mode)); } catch (err) { return Promise.reject(err); }
        },
        FileHandle: FileHandle
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

    function openAsync(path, flags, mode) {
        try {
            return Promise.resolve(openSync(path, flags, mode));
        } catch (err) {
            return Promise.reject(err);
        }
    }

    function closeAsync(fd) {
        try {
            return Promise.resolve(closeSync(fd));
        } catch (err) {
            return Promise.reject(err);
        }
    }

    // fs.read(fd, buffer, offset, length, position, callback), (fd, buffer,
    // options, callback), (fd, options, callback), or (fd, callback):
    // callback(err, bytesRead, buffer).
    function read(fd) {
        var args = Array.prototype.slice.call(arguments, 1);
        var callback = typeof args[args.length - 1] === 'function' ? args.pop() : null;
        var p;
        try {
            var parsed = parseReadArgs(args[0], args[1], args[2], args[3]);
            var bytesRead = readSync(fd, parsed.buffer, parsed.offset, parsed.length, parsed.position);
            p = Promise.resolve({ bytesRead: bytesRead, buffer: parsed.buffer });
        } catch (err) {
            p = Promise.reject(err);
        }
        if (!callback) return p;
        p.then(function(r) { callback(null, r.bytesRead, r.buffer); }, function(err) { callback(err); });
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

    // fsync / fdatasync / fstat / ftruncate / futimes: the stream's buffer
    // reaches the OS in the call, the disk operation runs on the fs threads.
    function fsyncAsync(fd) { return run('fsync', fd); }
    function fdatasyncAsync(fd) { return run('fdatasync', fd); }
    function fstatAsync(fd) { return run('fstat', fd).then(wrapStats); }
    function ftruncateAsync(fd, len) { return run('ftruncate', fd, len === undefined ? 0 : len); }
    function futimesAsync(fd, atime, mtime) {
        try {
            var a = toUnixTimestamp(atime, 'atime');
            var m = toUnixTimestamp(mtime, 'mtime');
            return run('futimes', fd, a, m);
        } catch (err) {
            return Promise.reject(err);
        }
    }

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
        utimesSync:      utimesSync,
        lutimesSync:     lutimesSync,
        truncateSync:    truncateSync,
        symlinkSync:     symlinkSync,

        // File descriptors
        openSync:        openSync,
        readSync:        readSync,
        writeSync:       writeSync,
        fstatSync:       fstatSync,
        closeSync:       closeSync,
        fsyncSync:       fsyncSync,
        fdatasyncSync:   fdatasyncSync,
        ftruncateSync:   ftruncateSync,
        futimesSync:     futimesSync,
        mkdtempSync:     mkdtempSync,
        open:            wrapAsync(openAsync),
        close:           wrapAsync(closeAsync),
        read:            read,
        write:           write,
        fstat:           wrapAsync(fstatAsync),
        ftruncate:       wrapAsync(ftruncateAsync),
        futimes:         wrapAsync(futimesAsync),
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
        utimes:      wrapAsync(promises.utimes),
        lutimes:     wrapAsync(promises.lutimes),
        truncate:    wrapAsync(promises.truncate),
        symlink:     wrapAsync(promises.symlink),

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
