// Test: path module
var path = globalThis.__brokit_path;
assert(typeof path === 'object', 'path exists');

// sep and delimiter
assert(typeof path.sep === 'string', 'path.sep is string');
assert(path.sep === '/' || path.sep === '\\', 'sep is / or \\');
assert(typeof path.delimiter === 'string', 'path.delimiter is string');
assert(path.delimiter === ':' || path.delimiter === ';', 'delimiter is : or ;');

// basename
assertEqual(path.basename('/foo/bar/baz.txt'), 'baz.txt', 'basename basic');
assertEqual(path.basename('/foo/bar/baz.txt', '.txt'), 'baz', 'basename with ext');
assertEqual(path.basename('/foo/bar/'), 'bar', 'basename trailing slash');

// dirname
assertEqual(path.dirname('/foo/bar/baz.txt'), '/foo/bar', 'dirname basic');
assertEqual(path.dirname('/foo/bar'), '/foo', 'dirname no ext');
assertEqual(path.dirname('/foo'), '/', 'dirname root child');
assertEqual(path.dirname('foo'), '.', 'dirname relative');

// extname
assertEqual(path.extname('file.txt'), '.txt', 'extname basic');
assertEqual(path.extname('file.tar.gz'), '.gz', 'extname double');
assertEqual(path.extname('file'), '', 'extname none');
assertEqual(path.extname('.hidden'), '', 'extname dotfile');

// isAbsolute
if (path.sep === '/') {
    assertEqual(path.isAbsolute('/foo/bar'), true, 'isAbsolute unix abs');
    assertEqual(path.isAbsolute('foo/bar'), false, 'isAbsolute unix rel');
} else {
    assertEqual(path.isAbsolute('C:\\foo'), true, 'isAbsolute win abs');
    assertEqual(path.isAbsolute('foo\\bar'), false, 'isAbsolute win rel');
}

// join
if (path.sep === '/') {
    assertEqual(path.join('/foo', 'bar', 'baz'), '/foo/bar/baz', 'join unix');
    assertEqual(path.join('/foo', '../bar'), '/bar', 'join with ..');
} else {
    assertEqual(path.join('C:\\foo', 'bar', 'baz'), 'C:\\foo\\bar\\baz', 'join win');
}

// normalize
if (path.sep === '/') {
    assertEqual(path.normalize('/foo/bar/../baz'), '/foo/baz', 'normalize ..');
    assertEqual(path.normalize('/foo/./bar'), '/foo/bar', 'normalize .');
} else {
    assertEqual(path.normalize('C:\\foo\\bar\\..\\baz'), 'C:\\foo\\baz', 'normalize win ..');
}

// relative
if (path.sep === '/') {
    assertEqual(path.relative('/data/orandea/test/aaa', '/data/orandea/impl/bbb'), '../../impl/bbb', 'relative up and down');
    assertEqual(path.relative('/a/b', '/a/b/c/d'), 'c/d', 'relative down');
    assertEqual(path.relative('/a/b/c', '/a'), '../..', 'relative up');
    assertEqual(path.relative('/a/b', '/a/b'), '', 'relative same');
    assertEqual(path.relative('/a/b/', '/a/b'), '', 'relative ignores a trailing slash');
} else {
    assertEqual(path.relative('C:\\orandea\\test\\aaa', 'C:\\orandea\\impl\\bbb'), '..\\..\\impl\\bbb', 'relative win');
    assertEqual(path.relative('C:\\a', 'c:\\A\\b'), 'b', 'relative win is case-insensitive');
    assertEqual(path.relative('C:\\a', 'D:\\b'), 'D:\\b', 'relative across drives is the target');
    assertEqual(path.relative('C:\\a\\b', 'C:\\a\\b'), '', 'relative same');
    assertEqual(path.resolve('D:/projects/bro'), 'D:\\projects\\bro', 'resolve spells every separator \\');
    assertEqual(path.normalize('C:/'), 'C:\\', 'normalize of a drive root');
    assertEqual(path.basename('C:\\'), '', 'a drive root has no basename');
}

// The native versions keep the script versions' rules
assertEqual(path.join(), '.', 'join of nothing');
assertEqual(path.join('', ''), '.', 'join of empty strings');
assertEqual(path.join('a', 5, 'b'), 'a' + path.sep + 'b', 'join leaves out a non-string');
assertEqual(path.normalize(''), '.', 'normalize of empty');
assertEqual(path.normalize('./'), '.', 'normalize of ./');
assertEqual(path.normalize('a/../..'), '..', 'normalize keeps .. above a relative root');
assertEqual(path.normalize('../a/../../b'), '..' + path.sep + '..' + path.sep + 'b', 'normalize leading ..s');
assertEqual(path.dirname(''), '.', 'dirname of empty');
assertEqual(path.dirname('a/'), '.', 'dirname of a name with a trailing separator');
assertEqual(path.basename('a/b.txt', 'b.txt'), '', 'basename less the whole name');
assertEqual(path.basename(42), '', 'basename of a non-string');
assertEqual(path.extname('a/b.c/d'), '', 'extname looks at the base name only');
assertEqual(path.extname('x.tar.gz/'), '.gz', 'extname ignores a trailing separator');
if (path.sep === '/') {
    assertEqual(path.normalize('/../a'), '/a', 'normalize does not climb above /');
    assertEqual(path.normalize('//a//b/'), '/a/b', 'normalize collapses separators');
    assertEqual(path.join('/a/', '/b/'), '/a/b', 'join drops doubled separators');
    assertEqual(path.dirname('/'), '/', 'dirname of /');
    assertEqual(path.resolve('/a', 'b', '../c'), '/a/c', 'resolve');
    assertEqual(path.resolve('/a', '/b', 'c'), '/b/c', 'resolve restarts at an absolute part');
} else {
    assertEqual(path.normalize('C:\\..\\a'), 'C:\\a', 'normalize does not climb above a drive root');
    assertEqual(path.normalize('C:/a//b/'), 'C:\\a\\b', 'normalize collapses separators');
    assertEqual(path.normalize('C:a\\..\\..\\b'), 'C:..\\b', 'a drive-relative path keeps its ..');
    assertEqual(path.join('C:\\a\\', '\\b\\'), 'C:\\a\\b', 'join drops doubled separators');
    assertEqual(path.dirname('C:\\'), 'C:\\', 'dirname of a drive root');
    assertEqual(path.dirname('C:\\a'), 'C:\\', 'dirname of a drive child');
    assertEqual(path.dirname('C:a'), 'C:', 'dirname of a drive-relative name');
    assertEqual(path.resolve('C:\\a', 'b', '..\\c'), 'C:\\a\\c', 'resolve');
    assertEqual(path.resolve('C:\\a', 'D:\\b', 'c'), 'D:\\b\\c', 'resolve restarts at an absolute part');
}
assertEqual(path.resolve('x'), path.join(process.cwd(), 'x'), 'resolve of a relative path is under the cwd');
var nonAscii = path.join('caf\u00e9', '\u65e5\u672c', 'x.txt');
assertEqual(nonAscii, 'caf\u00e9' + path.sep + '\u65e5\u672c' + path.sep + 'x.txt', 'join keeps non-ASCII names');

// Speed: joining is native, well under a microsecond (it was ~7 us in script).
(function() {
    var dir = path.sep === '/' ? '/home/someone/Pictures/Holiday 2024' : 'C:\\Users\\someone\\Pictures\\Holiday 2024';
    var names = [];
    for (var i = 0; i < 400; i++) names.push('IMG_' + (1000 + i) + '.jpg');
    for (var w = 0; w < 5; w++) for (var j = 0; j < names.length; j++) path.join(dir, names[j]);
    var best = Infinity;
    for (var round = 0; round < 5; round++) {
        var t0 = Date.now();
        for (var r = 0; r < 25; r++) for (var k = 0; k < names.length; k++) path.join(dir, names[k]);
        best = Math.min(best, (Date.now() - t0) * 1000 / (25 * names.length));
    }
    assert(best < 1.5, 'path.join costs ' + best.toFixed(2) + ' us a call (limit 1.5)');
})();

// parse
var parsed = path.parse('/home/user/file.txt');
assertEqual(parsed.base, 'file.txt', 'parse base');
assertEqual(parsed.ext, '.txt', 'parse ext');
assertEqual(parsed.name, 'file', 'parse name');

// format
var formatted = path.format({ dir: '/home/user', base: 'file.txt' });
assert(formatted.indexOf('file.txt') !== -1, 'format includes base');
assert(formatted.indexOf('/home/user') === 0 || formatted.indexOf('\\home\\user') === 0, 'format includes dir');
