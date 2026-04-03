(function() {
    var _blobRegistry = new Map();
    var _counter = 0;

    URL.createObjectURL = function(blob) {
        var id = 'blob:brokit/' + (++_counter) + '-' + Date.now();
        _blobRegistry.set(id, blob);
        return id;
    };

    URL.revokeObjectURL = function(url) {
        _blobRegistry.delete(url);
    };

    // Internal: retrieve a Blob by its object URL (used by fetch, img loading, etc.)
    globalThis.__brokit_getBlobByURL = function(url) {
        return _blobRegistry.get(url) || null;
    };
})();
