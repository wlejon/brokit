// Test: fs round-trips non-ASCII file and directory names.
//
// Paths are UTF-8 on the JS side; on Windows each one has to reach the OS as
// UTF-16, not through the ANSI code page, or names like these are mangled
// (written under one name, then not found under the same one).

var fs = globalThis.__brokit_fs;
var os = globalThis.__brokit_os;
var root = os.tmpdir() + '/brokit_fs_unicode_' + Date.now();

var dirName = 'répertoire_日本語';
var fileName = 'naïve_файл_🙂.txt';
var dir = root + '/' + dirName;
var file = dir + '/' + fileName;

fs.mkdirSync(dir, { recursive: true });
assert(fs.existsSync(dir), 'mkdirSync + existsSync with a non-ASCII directory');

fs.writeFileSync(file, 'contenu ✓');
assert(fs.existsSync(file), 'writeFileSync creates the non-ASCII file');
assertEqual(fs.readFileSync(file, 'utf8'), 'contenu ✓', 'readFileSync reads it back');

fs.appendFileSync(file, ' + plus');
assertEqual(fs.readFileSync(file, 'utf8'), 'contenu ✓ + plus', 'appendFileSync appends to it');

var st = fs.statSync(file);
assert(st.isFile(), 'statSync sees a file');
assertEqual(st.size, new TextEncoder().encode('contenu ✓ + plus').length, 'statSync size');
assert(fs.lstatSync(file).isFile(), 'lstatSync sees a file');

var listing = fs.readdirSync(dir);
assert(listing.indexOf(fileName) !== -1, 'readdirSync returns the name as UTF-8: ' + JSON.stringify(listing));
var ents = fs.readdirSync(root, { withFileTypes: true });
assert(ents.length === 1 && ents[0].name === dirName && ents[0].isDirectory(), 'readdirSync withFileTypes names');

var copy = dir + '/copie_ü.txt';
fs.copyFileSync(file, copy);
assertEqual(fs.readFileSync(copy, 'utf8'), 'contenu ✓ + plus', 'copyFileSync to a non-ASCII name');

var moved = dir + '/déplacé_ß.txt';
fs.renameSync(copy, moved);
assert(!fs.existsSync(copy) && fs.existsSync(moved), 'renameSync between non-ASCII names');

var real = fs.realpathSync(moved);
assert(real.indexOf('déplacé_ß.txt') !== -1, 'realpathSync keeps the name: ' + real);

var fd = fs.openSync(moved, 'r');
var buf = new Uint8Array(7);
var got = fs.readSync(fd, buf, 0, 7, 0);
fs.closeSync(fd);
assertEqual(got, 7, 'openSync + readSync on a non-ASCII name');
assertEqual(new TextDecoder().decode(buf), 'contenu', 'readSync bytes');

fs.unlinkSync(moved);
assert(!fs.existsSync(moved), 'unlinkSync removes it');

fs.rmSync(root, { recursive: true, force: true });
assert(!fs.existsSync(root), 'rmSync removes the tree');
