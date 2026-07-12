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
    // options: { cwd, env, stdoutFile, stderrFile }
    //   env         — REPLACES the child environment (Node semantics); spread
    //                 process.env in yourself to extend rather than replace
    //   stdoutFile / stderrFile — redirect child output to files (truncate on
    //                 open; pass the same path for a combined log). Without
    //                 them the child inherits no stdio pipes.
    // Starts a detached child process. Returns a ChildProcess with:
    //   .pid          — OS process id
    //   .killed       — boolean, true after .kill() is called
    //   .exitCode     — null until the child exits, then the integer code
    //   .signal       — null unless terminated by signal (POSIX)
    //   .kill([sig])  — request termination (SIGTERM default, 'SIGKILL' to force)
    //   .on(event, fn) / .once(event, fn) / .off(event, fn)
    //       events: 'exit' (code, signal), 'close' (fires right after 'exit')
    //
    // Polling cadence: 100ms. The caller doesn't need to do anything — the
    // polling loop stops itself as soon as the child exits.

    function ChildProcess(handle) {
        this._id = handle.id;
        this.pid = handle.pid;
        this.killed = false;
        this.exitCode = null;
        this.signal = null;
        this._listeners = { exit: [], close: [] };

        var self = this;
        this._poll = setInterval(function () {
            try {
                var r = globalThis.__brokit_cp_childPoll(self._id);
                if (r === null) return;
                clearInterval(self._poll);
                self._poll = null;
                self.exitCode = r.exitCode;
                self.signal = r.signal;
                var listeners = self._listeners.exit.slice();
                for (var i = 0; i < listeners.length; i++) {
                    try { listeners[i](r.exitCode, r.signal); } catch (e) { /* swallow */ }
                }
                var closeListeners = self._listeners.close.slice();
                for (var j = 0; j < closeListeners.length; j++) {
                    try { closeListeners[j](r.exitCode, r.signal); } catch (e) { /* swallow */ }
                }
            } catch (e) {
                clearInterval(self._poll);
                self._poll = null;
            }
        }, 100);
    }

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
        return new ChildProcess(handle);
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
