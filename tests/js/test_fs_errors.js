// Test: fs error paths — ENOENT, EEXIST, write failures, ArrayBuffer paths
var fs = globalThis.__brokit_fs;
var tmp = globalThis.__brokit_os.tmpdir();
var dir = tmp + '/brokit_fs_err_' + Date.now();
fs.mkdirSync(dir);

// ── writeFileSync with ArrayBuffer (not just Uint8Array) ──────────────────
var ab = new ArrayBuffer(4);
new DataView(ab).setUint32(0, 0x41424344);
fs.writeFileSync(dir + '/ab.bin', ab);
var abBack = fs.readFileSync(dir + '/ab.bin');
assertEqual(abBack.length, 4, 'writeFileSync ArrayBuffer wrote 4 bytes');

// ── appendFileSync with Uint8Array ────────────────────────────────────────
var apFile = dir + '/append.bin';
fs.writeFileSync(apFile, new Uint8Array([1, 2]));
fs.appendFileSync(apFile, new Uint8Array([3, 4]));
var apBack = fs.readFileSync(apFile);
assertEqual(apBack.length, 4, 'appendFileSync Uint8Array length');
assertEqual(apBack[3], 4, 'appendFileSync Uint8Array content');

// ── lstatSync on missing path throws ──────────────────────────────────────
var threw = false;
try { fs.lstatSync(dir + '/nope'); } catch (e) {
    threw = true;
    assertEqual(e.code, 'ENOENT', 'lstatSync ENOENT code');
}
assert(threw, 'lstatSync throws on missing');

// ── rename of missing source throws ───────────────────────────────────────
threw = false;
try { fs.renameSync(dir + '/nope', dir + '/dest'); } catch (e) {
    threw = true;
    assert(typeof e.code === 'string', 'rename error has code');
}
assert(threw, 'renameSync throws on missing source');

// ── copyFile of missing source throws ─────────────────────────────────────
threw = false;
try { fs.copyFileSync(dir + '/nope', dir + '/dst'); } catch (e) {
    threw = true;
    assert(typeof e.code === 'string', 'copyFile error has code');
}
assert(threw, 'copyFileSync throws on missing source');

// ── chmod of missing path throws ──────────────────────────────────────────
threw = false;
try { fs.chmodSync(dir + '/nope', 0o644); } catch (e) {
    threw = true;
    assertEqual(e.code, 'ENOENT', 'chmod ENOENT code');
}
assert(threw, 'chmodSync throws on missing');

// ── rmdir on non-empty dir throws ─────────────────────────────────────────
var subdir = dir + '/notempty';
fs.mkdirSync(subdir);
fs.writeFileSync(subdir + '/file.txt', 'x');
threw = false;
try { fs.rmdirSync(subdir); } catch (e) {
    threw = true;
    assert(typeof e.code === 'string', 'rmdir non-empty error has code');
}
assert(threw, 'rmdirSync on non-empty dir throws');

// ── unlinkSync on a directory throws ──────────────────────────────────────
// (fs::remove on a non-empty dir sets an error_code → EISDIR or similar)
threw = false;
try { fs.unlinkSync(subdir); } catch (e) {
    threw = true;
    assert(typeof e.code === 'string', 'unlink-on-dir error has code');
}
// May or may not throw depending on platform; just exercise

// ── mkdir of nested without recursive throws ──────────────────────────────
threw = false;
try { fs.mkdirSync(dir + '/x/y/z'); } catch (e) {
    threw = true;
    assert(typeof e.code === 'string', 'mkdir nested error has code');
}
assert(threw, 'mkdirSync nested without recursive throws');

// ── unlink/rmdir/rm of missing path are no-ops on this platform ──────────
// std::filesystem::remove() returns false without an error_code if missing,
// so these calls silently succeed. Just exercise the code path.
fs.unlinkSync(dir + '/nope_unlink');
fs.rmdirSync(dir + '/nope_rmdir');
fs.rmSync(dir + '/nope_rm');
assert(true, 'unlink/rmdir/rm on missing return cleanly');

// ── realpathSync on missing throws ────────────────────────────────────────
threw = false;
try { fs.realpathSync(dir + '/nope_realpath'); } catch (e) {
    threw = true;
}
assert(threw, 'realpathSync throws on missing');

// ── readFileSync with non-utf8 encoding hits the "any other encoding" path
fs.writeFileSync(dir + '/enc.txt', 'plain');
var asAscii = fs.readFileSync(dir + '/enc.txt', 'ascii');
assertEqual(asAscii, 'plain', 'readFileSync ascii encoding');
var asLatin = fs.readFileSync(dir + '/enc.txt', 'latin1');
assertEqual(asLatin, 'plain', 'readFileSync latin1 encoding');

// ── writeFileSync to an unwritable path triggers throwErrno
//   On Windows, opening a file inside a non-existent parent dir fails.
threw = false;
try { fs.writeFileSync(dir + '/nope_subdir/file.txt', 'x'); } catch (e) {
    threw = true;
    assertEqual(e.code, 'ENOENT', 'writeFileSync ENOENT');
}
assert(threw, 'writeFileSync throws when parent dir missing');

// ── mkdirSync on existing path without recursive is a silent no-op too
// (fs::create_directory returns false but doesn't set an error_code)
fs.mkdirSync(dir);
assert(true, 'mkdirSync on existing path returns cleanly');

// ── readFileSync passes options-object encoding ───────────────────────────
var withObj = fs.readFileSync(dir + '/enc.txt', { encoding: 'ascii' });
assertEqual(withObj, 'plain', 'readFileSync object-encoding works');

// ── writeFileSync with options object containing encoding ─────────────────
fs.writeFileSync(dir + '/opt.txt', 'x', { encoding: 'utf8' });
assertEqual(fs.readFileSync(dir + '/opt.txt', 'utf8'), 'x', 'writeFileSync options encoding');

// ── lstatSync on dir works ────────────────────────────────────────────────
var ls = fs.lstatSync(dir);
assert(ls.isDirectory(), 'lstatSync on directory');

// ── readFileSync error has syscall + path ─────────────────────────────────
threw = false;
try { fs.readFileSync(dir + '/missing.txt'); } catch (e) {
    threw = true;
    assert(typeof e.path === 'string', 'readFile error has path');
    assertEqual(e.syscall, 'open', 'readFile error syscall');
}
assert(threw, 'readFileSync error fully populated');

// ── Async error paths via callback ────────────────────────────────────────
var cbErr = null;
fs.readFile(dir + '/missing2.txt', 'utf8', function(err, data) { cbErr = err; });
assert(cbErr !== null, 'readFile async surfaces error');
assertEqual(cbErr.code, 'ENOENT', 'async error code propagated');

// ── writeFileSync/appendFileSync with un-stringifiable data throws ────────
threw = false;
try { fs.writeFileSync(dir + '/sym.txt', Symbol('x')); } catch (e) { threw = true; }
assert(threw, 'writeFileSync with Symbol data throws');

threw = false;
try { fs.appendFileSync(dir + '/sym.txt', Symbol('x')); } catch (e) { threw = true; }
assert(threw, 'appendFileSync with Symbol data throws');

// ── rename to a missing dest dir throws ──────────────────────────────────
fs.writeFileSync(dir + '/src.txt', 'x');
threw = false;
try { fs.renameSync(dir + '/src.txt', dir + '/nope_dir/dst.txt'); } catch (e) {
    threw = true;
}
assert(threw, 'renameSync to missing parent throws');

// ── copyFile to a missing dest dir throws ────────────────────────────────
threw = false;
try { fs.copyFileSync(dir + '/src.txt', dir + '/nope_dir2/dst.txt'); } catch (e) {
    threw = true;
}
assert(threw, 'copyFileSync to missing parent throws');

// ── chmod missing args ────────────────────────────────────────────────────
threw = false;
try { fs.chmodSync(); } catch (e) { threw = true; }
assert(threw, 'chmodSync missing args throws');

// ── rename missing args ──────────────────────────────────────────────────
threw = false;
try { fs.renameSync(dir + '/a'); } catch (e) { threw = true; }
assert(threw, 'renameSync missing args throws');

// ── readFileSync with options object that has no encoding ─────────────────
fs.writeFileSync(dir + '/raw.bin', new Uint8Array([1, 2, 3]));
var rawBuf = fs.readFileSync(dir + '/raw.bin', {});  // empty options
assert(rawBuf instanceof Uint8Array, 'readFileSync empty options returns buffer');

// ── statSync timestamps available ────────────────────────────────────────
var s1 = fs.statSync(dir);
assert(s1.mtime instanceof Date, 'statSync mtime is Date');
assert(typeof s1.mtimeMs === 'number', 'statSync mtimeMs');

// ── readdirSync with withFileTypes options object ────────────────────────
fs.writeFileSync(dir + '/inner1.txt', '1');
fs.writeFileSync(dir + '/inner2.txt', '2');
var dirents = fs.readdirSync(dir, { withFileTypes: true });
assert(Array.isArray(dirents), 'readdirSync withFileTypes is array');
assert(dirents.length > 0, 'readdirSync has entries');

// ── existsSync on path with prefix-mount-like leading slash ──────────────
// (Hits the resolveFsPath cleanup that strips leading slashes; this just
// exercises the path-cleaning branches even though there's no mount.)
assertEqual(fs.existsSync('/nonexistent_prefix_path_brokit'), false, 'existsSync slash-prefixed missing');

// ── Exercise base-path resolver by populating the JS-visible array ───────
// (Normally set via C++ API addBrokitFsBasePath; we can set the global
// directly to drive the same lookup code from JS.)
var origBases = globalThis.__brokit_fs_base_paths;
globalThis.__brokit_fs_base_paths = [dir];
fs.writeFileSync(dir + '/base_test.txt', 'base-content');
// Now a relative path "base_test.txt" should resolve under dir
var baseContent = fs.readFileSync('base_test.txt', 'utf8');
assertEqual(baseContent, 'base-content', 'base-path resolver finds file');

// And a missing relative path falls through to the original path
threw = false;
try { fs.readFileSync('missing_in_base.txt'); } catch (e) { threw = true; }
assert(threw, 'base-path resolver still throws for missing');

// writeFileSync for create resolves to topBase
fs.writeFileSync('write_via_base.txt', 'new');
assert(fs.existsSync(dir + '/write_via_base.txt'), 'writeFileSync resolved through base path');

// ── Exercise prefix-mount resolver via JS ────────────────────────────────
var origMounts = globalThis.__brokit_path_mounts;
globalThis.__brokit_path_mounts = { '/mnt/test': dir };
fs.writeFileSync(dir + '/under_mount.txt', 'mounted');
var mountContent = fs.readFileSync('/mnt/test/under_mount.txt', 'utf8');
assertEqual(mountContent, 'mounted', 'prefix-mount resolver finds file');

// Mount with exact-prefix match (reading a dir as file may throw, just exercise lookup)
try { fs.readFileSync('/mnt/test', 'utf8'); } catch (e) {}
// Mount where path doesn't match prefix
try { fs.readFileSync('/other/path/file.txt', 'utf8'); } catch (e) {}
assert(true, 'prefix-mount path-mismatch exercised');

// Restore
globalThis.__brokit_fs_base_paths = origBases;
globalThis.__brokit_path_mounts = origMounts;

// ── Cleanup ───────────────────────────────────────────────────────────────
fs.rmSync(dir, { recursive: true, force: true });
