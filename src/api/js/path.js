(function() {
    var isWindows = (typeof process !== 'undefined' && process.platform === 'win32');
    // normalize / join / resolve / dirname / basename / extname are native
    // (path.cpp): the string work is a few hundred nanoseconds there, several
    // microseconds here.
    var N = globalThis.__brokit_path_native;

    function splitPath(filename) {
        // Returns [root, dir, basename, ext]
        var sep = isWindows ? /[/\\]/ : /\//;
        // Handle Windows drive letter
        var root = '';
        if (isWindows && filename.length >= 2 && filename[1] === ':') {
            root = filename.substring(0, 2);
            filename = filename.substring(2);
        }
        if (filename.charAt(0) === '/' || filename.charAt(0) === '\\') {
            root += filename.charAt(0);
            filename = filename.substring(1);
        }

        var parts = filename.split(sep);
        var base = parts.pop() || '';
        var dir = parts.join(isWindows ? '\\' : '/');

        var extIdx = base.lastIndexOf('.');
        var ext = '';
        if (extIdx > 0) {
            ext = base.substring(extIdx);
        }

        return [root, dir, base, ext];
    }

    var path = {};

    path.sep = isWindows ? '\\' : '/';
    path.delimiter = isWindows ? ';' : ':';

    // Windows spells the root '\' however it was written; both separators
    // are read on every platform, the platform's own is written.
    path.normalize = N.normalize;
    // The string arguments, empty ones left out, normalized ('.' for none).
    path.join = N.join;
    // Right to left until a part is absolute, under the working directory
    // when none is.
    path.resolve = N.resolve;

    path.isAbsolute = function(p) {
        if (typeof p !== 'string') return false;
        if (isWindows) {
            return (p.length >= 3 && p[1] === ':' && (p[2] === '/' || p[2] === '\\')) ||
                   p.charAt(0) === '\\';
        }
        return p.charAt(0) === '/';
    };

    // Node's path.relative: the path from `from` to `to`, both resolved
    // first; '' when they are the same. Windows compares case-insensitively
    // and answers the absolute `to` when the drives differ.
    path.relative = function(from, to) {
        from = path.resolve(from);
        to = path.resolve(to);
        if (from === to) return '';
        var fromParts = from.split(/[/\\]/).filter(function(s) { return s.length > 0; });
        var toParts = to.split(/[/\\]/).filter(function(s) { return s.length > 0; });
        var same = function(a, b) { return isWindows ? a.toLowerCase() === b.toLowerCase() : a === b; };
        if (isWindows && fromParts.length && toParts.length && !same(fromParts[0], toParts[0])) return to;
        var n = Math.min(fromParts.length, toParts.length);
        var common = 0;
        while (common < n && same(fromParts[common], toParts[common])) common++;
        var out = [];
        for (var i = common; i < fromParts.length; i++) out.push('..');
        return out.concat(toParts.slice(common)).join(path.sep);
    };

    // The part before the last separator (a trailing one aside); a drive
    // root keeps its drive.
    path.dirname = N.dirname;
    // The last part, trailing separators aside, less `ext` when it ends so;
    // a drive root has no name, as in Node.
    path.basename = N.basename;
    // From the last '.' of the base name; '' for none or a dotfile.
    path.extname = N.extname;

    path.parse = function(p) {
        var parts = splitPath(p || '');
        return {
            root: parts[0],
            dir: parts[0] + parts[1],
            base: parts[2],
            ext: parts[3],
            name: parts[3] ? parts[2].substring(0, parts[2].length - parts[3].length) : parts[2]
        };
    };

    path.format = function(obj) {
        var dir = obj.dir || (obj.root || '');
        var base = obj.base || ((obj.name || '') + (obj.ext || ''));
        if (!dir) return base;
        if (dir === obj.root) return dir + base;
        return dir + path.sep + base;
    };

    globalThis.__brokit_path = path;
})();
