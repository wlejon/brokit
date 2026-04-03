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

    // ── Build the child_process object ────────────────────────────────────────

    var child_process = {
        execSync: execSync,
        exec: exec,
        execFileSync: execFileSync,
        execFile: execFile,
        spawnSync: spawnSync
    };

    globalThis.__brokit_child_process = child_process;
})();
