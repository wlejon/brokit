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
}

// parse
var parsed = path.parse('/home/user/file.txt');
assertEqual(parsed.base, 'file.txt', 'parse base');
assertEqual(parsed.ext, '.txt', 'parse ext');
assertEqual(parsed.name, 'file', 'parse name');

// format
var formatted = path.format({ dir: '/home/user', base: 'file.txt' });
assert(formatted.indexOf('file.txt') !== -1, 'format includes base');
assert(formatted.indexOf('/home/user') === 0 || formatted.indexOf('\\home\\user') === 0, 'format includes dir');
