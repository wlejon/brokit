(function() {
    var isWindows = (typeof process !== 'undefined' && process.platform === 'win32');

    function normalizeArray(parts, allowAboveRoot) {
        var res = [];
        for (var i = 0; i < parts.length; i++) {
            var p = parts[i];
            if (!p || p === '.') continue;
            if (p === '..') {
                if (res.length && res[res.length - 1] !== '..') {
                    res.pop();
                } else if (allowAboveRoot) {
                    res.push('..');
                }
            } else {
                res.push(p);
            }
        }
        return res;
    }

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

    path.normalize = function(p) {
        if (typeof p !== 'string') p = String(p);
        if (p.length === 0) return '.';

        var isAbsolute = false;
        var root = '';

        if (isWindows && p.length >= 2 && p[1] === ':') {
            root = p.substring(0, 2);
            p = p.substring(2);
        }
        if (p.charAt(0) === '/' || p.charAt(0) === '\\') {
            isAbsolute = true;
            root += p.charAt(0);
            p = p.substring(1);
        }

        var parts = p.split(/[/\\]/);
        var normalized = normalizeArray(parts, !isAbsolute);
        var result = root + normalized.join(path.sep);

        if (!result) return isAbsolute ? path.sep : '.';
        return result;
    };

    path.join = function() {
        var paths = [];
        for (var i = 0; i < arguments.length; i++) {
            if (typeof arguments[i] !== 'string') continue;
            if (arguments[i].length > 0) paths.push(arguments[i]);
        }
        if (paths.length === 0) return '.';
        return path.normalize(paths.join(path.sep));
    };

    path.resolve = function() {
        var resolved = '';
        var resolvedAbsolute = false;

        for (var i = arguments.length - 1; i >= 0 && !resolvedAbsolute; i--) {
            var p = arguments[i];
            if (typeof p !== 'string' || p.length === 0) continue;

            resolved = p + path.sep + resolved;

            // Check if absolute
            if (isWindows) {
                if (p.length >= 3 && p[1] === ':' && (p[2] === '/' || p[2] === '\\')) {
                    resolvedAbsolute = true;
                }
            } else {
                if (p.charAt(0) === '/') {
                    resolvedAbsolute = true;
                }
            }
        }

        if (!resolvedAbsolute && typeof process !== 'undefined' && typeof process.cwd === 'function') {
            resolved = process.cwd() + path.sep + resolved;
        }

        return path.normalize(resolved);
    };

    path.isAbsolute = function(p) {
        if (typeof p !== 'string') return false;
        if (isWindows) {
            return (p.length >= 3 && p[1] === ':' && (p[2] === '/' || p[2] === '\\')) ||
                   p.charAt(0) === '\\';
        }
        return p.charAt(0) === '/';
    };

    path.dirname = function(p) {
        if (typeof p !== 'string' || p.length === 0) return '.';

        var root = '';
        var idx = 0;
        if (isWindows && p.length >= 2 && p[1] === ':') {
            root = p.substring(0, 2);
            idx = 2;
        }

        var lastSep = -1;
        for (var i = p.length - 1; i >= idx; i--) {
            if (p[i] === '/' || p[i] === '\\') {
                if (i !== p.length - 1) {
                    lastSep = i;
                    break;
                }
            }
        }

        if (lastSep === -1) {
            if (root) return root;
            return '.';
        }
        if (lastSep === idx) return root + p[idx];
        return p.substring(0, lastSep);
    };

    path.basename = function(p, ext) {
        if (typeof p !== 'string') return '';
        // Strip trailing separators
        while (p.length > 0 && (p[p.length - 1] === '/' || p[p.length - 1] === '\\')) {
            p = p.substring(0, p.length - 1);
        }
        var lastSep = Math.max(p.lastIndexOf('/'), p.lastIndexOf('\\'));
        var base = p.substring(lastSep + 1);
        if (ext && base.length >= ext.length && base.substring(base.length - ext.length) === ext) {
            base = base.substring(0, base.length - ext.length);
        }
        return base;
    };

    path.extname = function(p) {
        if (typeof p !== 'string') return '';
        var base = path.basename(p);
        var idx = base.lastIndexOf('.');
        if (idx <= 0) return '';
        return base.substring(idx);
    };

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
