(function() {
    // ── execSync ──────────────────────────────────────────────────────────────
    // Already implemented natively as __brokit_cp_execSync.
    // Throws on non-zero exit code with { status, stdout, stderr }.

    function execSync(command, options) {
        return globalThis.__brokit_cp_execSync(command, options || {});
    }

    // ── exec ──────────────────────────────────────────────────────────────────
    // exec(command[, options], callback?)
    // Callback style: callback(error, stdout, stderr)
    // Promise style: returns Promise<{ stdout, stderr }>

    function exec(command, options, callback) {
        if (typeof options === 'function') {
            callback = options;
            options = {};
        }
        options = options || {};

        if (callback) {
            try {
                var result = globalThis.__brokit_cp_exec(command, options);
                if (result.error || result.exitCode !== 0) {
                    var err = new Error(result.error || 'Command failed: ' + command);
                    err.code = result.exitCode;
                    err.killed = result.timedOut || false;
                    err.stdout = result.stdout;
                    err.stderr = result.stderr;
                    callback(err, result.stdout, result.stderr);
                } else {
                    callback(null, result.stdout, result.stderr);
                }
            } catch (e) {
                callback(e, '', '');
            }
            return;
        }

        // Promise style
        try {
            var result = globalThis.__brokit_cp_exec(command, options);
            if (result.error || result.exitCode !== 0) {
                var err = new Error(result.error || 'Command failed: ' + command);
                err.code = result.exitCode;
                err.killed = result.timedOut || false;
                err.stdout = result.stdout;
                err.stderr = result.stderr;
                return Promise.reject(err);
            }
            return Promise.resolve({ stdout: result.stdout, stderr: result.stderr });
        } catch (e) {
            return Promise.reject(e);
        }
    }

    // ── execFileSync ──────────────────────────────────────────────────────────
    // execFileSync(file, args?, options?)
    // Like execSync but takes file + args array

    function execFileSync(file, args, options) {
        if (args && !Array.isArray(args)) {
            options = args;
            args = [];
        }
        args = args || [];
        options = options || {};

        var command = file;
        for (var i = 0; i < args.length; i++) {
            var arg = args[i];
            if (arg.indexOf(' ') !== -1 || arg.indexOf('\t') !== -1) {
                command += ' "' + arg + '"';
            } else {
                command += ' ' + arg;
            }
        }

        return execSync(command, options);
    }

    // ── execFile ──────────────────────────────────────────────────────────────
    // execFile(file, args?, options?, callback?)

    function execFile(file, args, options, callback) {
        if (typeof args === 'function') {
            callback = args;
            args = [];
            options = {};
        } else if (typeof options === 'function') {
            callback = options;
            if (Array.isArray(args)) {
                options = {};
            } else {
                options = args;
                args = [];
            }
        }
        args = Array.isArray(args) ? args : [];
        options = options || {};

        var command = file;
        for (var i = 0; i < args.length; i++) {
            var arg = args[i];
            if (arg.indexOf(' ') !== -1 || arg.indexOf('\t') !== -1) {
                command += ' "' + arg + '"';
            } else {
                command += ' ' + arg;
            }
        }

        return exec(command, options, callback);
    }

    // ── spawnSync ─────────────────────────────────────────────────────────────

    function spawnSync(command, args, options) {
        if (args && !Array.isArray(args)) {
            options = args;
            args = [];
        }
        return globalThis.__brokit_cp_spawnSync(command, args || [], options || {});
    }

    // ── spawn (async) ─────────────────────────────────────────────────────────
    // spawn(file[, args[, options]])
    // options: { cwd, env, stdio, encoding, highWaterMark, stdoutFile, stderrFile }
    //   env         — REPLACES the child environment (Node semantics); spread
    //                 process.env in yourself to extend rather than replace
    //   stdio       — 'pipe' to stream the child's output and write its stdin.
    //                 DEFAULT is 'ignore' (no pipes at all), which diverges
    //                 from Node: an existing caller that never reads must not
    //                 silently start buffering, and a GUI child keeps its own
    //                 window. Pass 'pipe' explicitly to opt in.
    //   encoding    — with stdio:'pipe', 'utf8' delivers stdout/stderr as
    //                 decoded strings (UTF-8 safe across chunk boundaries).
    //                 Default is binary: chunks arrive as Uint8Array, which is
    //                 what raw pixel/audio streams need.
    //   highWaterMark — per-stream buffer cap in bytes (default 8 MB). When a
    //                 stream is full the reader stops, the pipe fills, and the
    //                 child blocks in write(). That is the backpressure: it is
    //                 released by draining, which happens automatically on the
    //                 poll tick.
    //   stdoutFile / stderrFile — redirect child output to files (truncate on
    //                 open; pass the same path for a combined log). Ignored
    //                 when stdio:'pipe' is set.
    // Starts a child process. Returns a ChildProcess with:
    //   .pid          — OS process id
    //   .killed       — boolean, true after .kill() is called
    //   .exitCode     — null until the child exits, then the integer code
    //   .signal       — null unless terminated by signal (POSIX)
    //   .kill([sig])  — request termination (SIGTERM default, 'SIGKILL' to force)
    //   .on(event, fn) / .once(event, fn) / .off(event, fn)
    //       events: 'exit' (code, signal), 'close' (code, signal), 'error' (err)
    //   with stdio:'pipe' also:
    //   .stdout / .stderr — .on('data', chunk) / .on('end') / .setEncoding(enc)
    //   .stdin            — .write(stringOrBytes) → bytes written; .end()
    //
    // 'exit' fires when the process is gone; 'close' fires once the process is
    // gone AND both pipes have hit EOF and been drained — so every byte the
    // child wrote has been delivered before 'close'. Without pipes the two are
    // back to back, as before.
    //
    // Polling cadence adapts: ~4 ms while bytes are flowing, 25 ms idle with
    // pipes, 100 ms without. The loop stops itself once the child is done.

    // Grace period after exit for both pipes to reach EOF. A grandchild that
    // inherited a write end can hold one open indefinitely; rather than leak
    // the handle we give up and close out.
    var EOF_GRACE_MS = 2000;

    // Liveness hook, mirroring __brokit_net_has_pending and friends. Embedders
    // that drive timers from their own frame loop (bro does) never need this;
    // it exists so a bare pump loop knows a spawned child is still outstanding
    // and keeps firing timers until it finishes.
    var liveChildren = 0;
    globalThis.__brokit_cp_has_pending = function () { return liveChildren > 0; };

    function emitAll(listeners, args, what) {
        if (!listeners || !listeners.length) return;
        var copy = listeners.slice();
        for (var i = 0; i < copy.length; i++) {
            try {
                copy[i].apply(null, args);
            } catch (e) {
                // One bad listener must not break the poll loop, but silently
                // eating it makes app bugs invisible — report and continue.
                try { console.error('child_process: ' + what + ' listener threw:', e); }
                catch (e2) { /* console unavailable */ }
            }
        }
    }

    // ── ChildStream: the readable half of a piped stdout/stderr ──────────────

    function ChildStream(encoding) {
        this._listeners = { data: [], end: [] };
        this._decoder = null;
        this.readable = true;
        if (encoding && encoding !== 'buffer') this.setEncoding(encoding);
    }
    ChildStream.prototype.on = function (event, fn) {
        if (this._listeners[event]) this._listeners[event].push(fn);
        return this;
    };
    ChildStream.prototype.once = function (event, fn) {
        var self = this;
        var wrap = function () { self.off(event, wrap); fn.apply(null, arguments); };
        return this.on(event, wrap);
    };
    ChildStream.prototype.off = function (event, fn) {
        var arr = this._listeners[event];
        if (!arr) return this;
        var idx = arr.indexOf(fn);
        if (idx >= 0) arr.splice(idx, 1);
        return this;
    };
    // Streaming decode: a multi-byte codepoint split across two pipe reads must
    // not turn into two replacement characters, so the decoder is persistent.
    ChildStream.prototype.setEncoding = function (enc) {
        if (!enc || enc === 'buffer') { this._decoder = null; return this; }
        try { this._decoder = new TextDecoder(enc); }
        catch (e) { this._decoder = new TextDecoder(); }
        return this;
    };
    ChildStream.prototype._push = function (bytes) {
        var chunk = this._decoder ? this._decoder.decode(bytes, { stream: true }) : bytes;
        if (chunk === '') return;
        emitAll(this._listeners.data, [chunk], 'data');
    };
    ChildStream.prototype._end = function () {
        if (!this.readable) return;
        this.readable = false;
        if (this._decoder) {
            // Flush any trailing partial sequence.
            var tail = this._decoder.decode(new Uint8Array(0));
            if (tail) emitAll(this._listeners.data, [tail], 'data');
        }
        emitAll(this._listeners.end, [], 'end');
    };

    // ── ChildStdin: the writable half ────────────────────────────────────────

    function ChildStdin(proc) {
        this._proc = proc;
        this.writable = true;
    }
    // Blocking: a child that stops reading stalls the JS thread once the pipe
    // buffer fills. Stream large input in chunks rather than one huge write.
    ChildStdin.prototype.write = function (data) {
        if (!this.writable || this._proc._released) return -1;
        try { return globalThis.__brokit_cp_childWrite(this._proc._id, data); }
        catch (e) { return -1; }
    };
    ChildStdin.prototype.end = function (data) {
        if (data !== undefined && data !== null) this.write(data);
        if (!this.writable) return;
        this.writable = false;
        try { globalThis.__brokit_cp_childCloseStdin(this._proc._id); } catch (e) { /* already gone */ }
    };

    // ── ChildProcess ─────────────────────────────────────────────────────────

    function ChildProcess(handle, options) {
        this._id = handle.id;
        this.pid = handle.pid;
        this.killed = false;
        this.exitCode = null;
        this.signal = null;
        this._listeners = { exit: [], close: [], error: [] };
        this._piped = !!handle.piped;
        this._released = false;
        this._exited = false;
        this._outEof = false;
        this._errEof = false;
        this._exitAt = 0;

        if (this._piped) {
            var enc = options && options.encoding;
            this.stdout = new ChildStream(enc);
            this.stderr = new ChildStream(enc);
            this.stdin = new ChildStdin(this);
        } else {
            this.stdout = null;
            this.stderr = null;
            this.stdin = null;
        }

        liveChildren++;

        var self = this;
        this._tick = function () { self._pollOnce(); };
        this._timer = setTimeout(this._tick, this._piped ? 8 : 100);
    }

    ChildProcess.prototype._finish = function () {
        if (this._released) return;
        this._released = true;
        liveChildren--;
        if (this._timer !== null) { clearTimeout(this._timer); this._timer = null; }
        if (this._piped) {
            this.stdout._end();
            this.stderr._end();
            if (this.stdin.writable) this.stdin.writable = false;
            try { globalThis.__brokit_cp_childRelease(this._id); } catch (e) { /* already gone */ }
        }
        emitAll(this._listeners.close, [this.exitCode, this.signal], 'close');
    };

    ChildProcess.prototype._pollOnce = function () {
        if (this._released) return;
        this._timer = null;

        var gotData = false;

        if (this._piped) {
            var r = null;
            try {
                r = globalThis.__brokit_cp_childRead(this._id);
            } catch (e) {
                this._finish();
                return;
            }
            if (r) {
                if (r.stdout) { gotData = true; this.stdout._push(r.stdout); }
                if (r.stderr) { gotData = true; this.stderr._push(r.stderr); }
                this._outEof = r.stdoutEof;
                this._errEof = r.stderrEof;
            }
        }

        var exit = null;
        try {
            exit = globalThis.__brokit_cp_childPoll(this._id);
        } catch (e) {
            this._finish();
            return;
        }

        if (exit !== null && !this._exited) {
            this._exited = true;
            this.exitCode = exit.exitCode;
            this.signal = exit.signal;
            this._exitAt = Date.now();
            emitAll(this._listeners.exit, [exit.exitCode, exit.signal], 'exit');
            // Without pipes the native side already released the handle, and
            // there is nothing left to drain.
            if (!this._piped) { this._finish(); return; }
        }

        if (this._piped && this._exited) {
            var drained = this._outEof && this._errEof;
            if (drained || (Date.now() - this._exitAt) > EOF_GRACE_MS) {
                this._finish();
                return;
            }
        }

        var delay = gotData ? 4 : (this._piped ? 25 : 100);
        this._timer = setTimeout(this._tick, delay);
    };

    ChildProcess.prototype.on = function (event, fn) {
        if (this._listeners[event]) this._listeners[event].push(fn);
        return this;
    };
    ChildProcess.prototype.once = function (event, fn) {
        var self = this;
        var wrap = function () {
            self.off(event, wrap);
            fn.apply(null, arguments);
        };
        return this.on(event, wrap);
    };
    ChildProcess.prototype.off = function (event, fn) {
        var arr = this._listeners[event];
        if (!arr) return this;
        var idx = arr.indexOf(fn);
        if (idx >= 0) arr.splice(idx, 1);
        return this;
    };
    ChildProcess.prototype.kill = function (signal) {
        if (this.killed) return false;
        var ok = globalThis.__brokit_cp_childKill(this._id, signal || 'SIGTERM');
        if (ok) this.killed = true;
        return ok;
    };

    function spawn(file, args, options) {
        if (args && !Array.isArray(args)) { options = args; args = []; }
        args = args || [];
        options = options || {};
        var handle = globalThis.__brokit_cp_spawnAsync(file, args, options);
        return new ChildProcess(handle, options);
    }

    // ── Build the child_process object ────────────────────────────────────────

    var child_process = {
        execSync: execSync,
        exec: exec,
        execFileSync: execFileSync,
        execFile: execFile,
        spawn: spawn,
        spawnSync: spawnSync
    };

    globalThis.__brokit_child_process = child_process;
})();
