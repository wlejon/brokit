// Test: fs.promises.open and FileHandle
var fs = globalThis.__brokit_fs;
var tmpBase = globalThis.__brokit_os.tmpdir();
var testDir = tmpBase + '/brokit_fs_filehandle_' + Date.now() + '_' + Math.floor(Math.random() * 100000);

fs.mkdirSync(testDir, { recursive: true });

var asyncDone = false;
var asyncError = null;

(async function() {
    try {
        // ── 1. Open w+, write string & buffer, read with explicit & default args ──
        var file1 = testDir + '/rw.txt';
        var fh = await fs.promises.open(file1, 'w+');
        assert(typeof fh.fd === 'number' && fh.fd >= 3, 'FileHandle.fd is a valid descriptor number');

        // write(string[, position[, encoding]])
        var w1 = await fh.write('Hello, ');
        assertEqual(w1.bytesWritten, 7, 'handle.write string bytesWritten');
        assertEqual(w1.buffer, 'Hello, ', 'handle.write string returns string as buffer');

        // write(Buffer/Uint8Array, offset, length, position)
        var rawBytes = new TextEncoder().encode('___FileHandle!___');
        var w2 = await fh.write(rawBytes, 3, 11, 7);
        assertEqual(w2.bytesWritten, 11, 'handle.write buffer slice bytesWritten');
        assertEqual(w2.buffer, rawBytes, 'handle.write returns input buffer');

        // stat()
        var st = await fh.stat();
        assert(st.isFile(), 'handle.stat().isFile()');
        assertEqual(st.size, 18, 'handle.stat().size matches written bytes');

        // read(buffer, offset, length, position)
        var readBuf = new Uint8Array(24);
        var r1 = await fh.read(readBuf, 2, 18, 0);
        assertEqual(r1.bytesRead, 18, 'handle.read positional bytesRead');
        assertEqual(r1.buffer, readBuf, 'handle.read returns target buffer');
        var decoded1 = new TextDecoder().decode(readBuf.subarray(2, 20));
        assertEqual(decoded1, 'Hello, FileHandle!', 'handle.read positional content');

        // read(buffer, { offset, length, position })
        var readBuf2 = new Uint8Array(10);
        var r2 = await fh.read(readBuf2, { offset: 0, length: 10, position: 7 });
        assertEqual(r2.bytesRead, 10, 'handle.read options-second-arg bytesRead');
        assertEqual(new TextDecoder().decode(readBuf2), 'FileHandle', 'handle.read options-second-arg content');

        // read({ buffer, offset, length, position })
        var readBuf3 = new Uint8Array(5);
        var r3 = await fh.read({ buffer: readBuf3, offset: 0, length: 5, position: 0 });
        assertEqual(r3.bytesRead, 5, 'handle.read single options object bytesRead');
        assertEqual(new TextDecoder().decode(readBuf3), 'Hello', 'handle.read single options object content');

        // read() with no arguments allocates default 16384-byte buffer
        // First seek to 0 by doing a 0-length read at position 0, then sequential read()
        await fh.read(new Uint8Array(0), 0, 0, 0);
        var rDefault = await fh.read();
        assertEqual(rDefault.bytesRead, 18, 'handle.read() no-arg bytesRead');
        assertEqual(rDefault.buffer.byteLength, 16384, 'handle.read() default buffer is 16384 bytes');
        assertEqual(new TextDecoder().decode(rDefault.buffer.subarray(0, 18)), 'Hello, FileHandle!', 'handle.read() content');

        // datasync() and sync()
        await fh.datasync();
        await fh.sync();

        // truncate(len)
        await fh.truncate(5);
        var stTrunc = await fh.stat();
        assertEqual(stTrunc.size, 5, 'handle.truncate(5) resizes file to 5 bytes');
        assertEqual(await fh.readFile('utf8'), 'Hello', 'handle.readFile after truncate(5)');

        // utimes(atime, mtime)
        var uAtime = new Date('2023-04-05T06:07:08.000Z');
        var uMtime = new Date('2023-04-06T07:08:09.000Z');
        await fh.utimes(uAtime, uMtime);
        var stUtimes = await fh.stat();
        assert(Math.abs(stUtimes.atimeMs - uAtime.getTime()) <= 5, 'handle.utimes updates atimeMs');
        assert(Math.abs(stUtimes.mtimeMs - uMtime.getTime()) <= 5, 'handle.utimes updates mtimeMs');

        // close() idempotency and post-close EBADF rejection
        await fh.close();
        assertEqual(fh.fd, -1, 'handle.fd is -1 after close()');
        await fh.close(); // second close() must be a no-op

        var postCloseOps = [
            function() { return fh.read(); },
            function() { return fh.write('x'); },
            function() { return fh.readFile(); },
            function() { return fh.writeFile('x'); },
            function() { return fh.appendFile('x'); },
            function() { return fh.stat(); },
            function() { return fh.truncate(0); },
            function() { return fh.datasync(); },
            function() { return fh.sync(); },
            function() { return fh.utimes(1700000000, 1700000000); }
        ];
        for (var i = 0; i < postCloseOps.length; i++) {
            var rejectedEbadf = false;
            try {
                await postCloseOps[i]();
            } catch (e) {
                rejectedEbadf = (e.code === 'EBADF');
            }
            assert(rejectedEbadf, 'operation #' + i + ' after close() rejects with EBADF');
        }

        // ── 2. writeFile, appendFile, readFile on FileHandle ───────────────
        var file2 = testDir + '/file_ops.txt';
        var fh2 = await fs.promises.open(file2, 'w+');
        await fh2.writeFile('First line\n');
        await fh2.appendFile('Second line');
        var text2 = await fh2.readFile('utf8');
        assertEqual(text2, 'First line\nSecond line', 'handle.writeFile + appendFile + readFile(utf8)');

        var raw2 = await fh2.readFile();
        assert(raw2 instanceof Uint8Array, 'handle.readFile() without encoding returns Uint8Array/Buffer');
        assertEqual(raw2.byteLength, 22, 'handle.readFile() byteLength');

        // Overwrite with shorter content via writeFile
        await fh2.writeFile('Short');
        assertEqual(await fh2.readFile({ encoding: 'utf8' }), 'Short', 'handle.writeFile truncates and overwrites');
        await fh2.close();

        // ── 3. Symbol.asyncDispose support ─────────────────────────────────
        var file3 = testDir + '/dispose.txt';
        var fh3 = await fs.promises.open(file3, 'w');
        await fh3.write('disposed');
        assert(typeof fh3[Symbol.asyncDispose] === 'function', 'FileHandle has [Symbol.asyncDispose]');
        await fh3[Symbol.asyncDispose]();
        assertEqual(fh3.fd, -1, '[Symbol.asyncDispose] closes handle and sets fd = -1');
        assertEqual(fs.readFileSync(file3, 'utf8'), 'disposed', 'content persisted before asyncDispose');

        // ── 4. Flags error cases (ENOENT for 'r', EEXIST for 'wx') ─────────
        var rMissingRejected = false;
        try {
            await fs.promises.open(testDir + '/does_not_exist.txt', 'r');
        } catch (e) {
            rMissingRejected = (e.code === 'ENOENT');
        }
        assert(rMissingRejected, 'fs.promises.open(missing, "r") rejects with ENOENT');

        var wxExistingRejected = false;
        try {
            await fs.promises.open(file3, 'wx');
        } catch (e) {
            wxExistingRejected = (e.code === 'EEXIST');
        }
        assert(wxExistingRejected, 'fs.promises.open(existing, "wx") rejects with EEXIST');

        // ── 5. Callback-style fs.open, fs.read, fs.fstat, fs.ftruncate, fs.close ──
        var cbFile = testDir + '/cb_fd.txt';
        fs.writeFileSync(cbFile, '0123456789abcdef');
        var cbfd = await fs.open(cbFile, 'r+');
        assert(typeof cbfd === 'number' && cbfd >= 3, 'fs.open resolves to fd');

        var cbReadRes = await new Promise(function(resolve, reject) {
            var b = new Uint8Array(6);
            fs.read(cbfd, b, 0, 6, 4, function(err, bytesRead, buf) {
                if (err) reject(err);
                else resolve({ bytesRead: bytesRead, str: new TextDecoder().decode(buf) });
            });
        });
        assertEqual(cbReadRes.bytesRead, 6, 'fs.read callback bytesRead');
        assertEqual(cbReadRes.str, '456789', 'fs.read callback buffer content');

        await fs.ftruncate(cbfd, 10);
        var cbSt = await fs.fstat(cbfd);
        assertEqual(cbSt.size, 10, 'fs.ftruncate + fs.fstat size is 10');
        await fs.close(cbfd);

        // fs.truncateSync and fs.promises.truncate
        fs.truncateSync(cbFile, 4);
        assertEqual(fs.statSync(cbFile).size, 4, 'fs.truncateSync resizes to 4');
        await fs.promises.truncate(cbFile, 2);
        assertEqual(fs.readFileSync(cbFile, 'utf8'), '01', 'fs.promises.truncate resizes to 2');

        asyncDone = true;
    } catch (err) {
        asyncError = err;
    }
})();

globalThis.__test_onDone = function() {
    if (asyncError) {
        assert(false, 'async FileHandle test failed: ' + (asyncError.stack || asyncError.message || asyncError));
    }
    assert(asyncDone, 'async FileHandle block completed');
    fs.rmSync(testDir, { recursive: true, force: true });
};
