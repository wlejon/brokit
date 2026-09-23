// Test: IndexedDB database and object-store names are data, not SQL or paths.
//
// A store name becomes an SQL identifier and a database name becomes a file
// name, and both are arbitrary strings. Names carrying brackets, quotes,
// semicolons, separators or `..` must round-trip without breaking the SQL or
// escaping the IndexedDB directory. objectStoreNames lists stores only: not
// SQLite's own sqlite_sequence, and not a store whose name merely resembles
// the internal `__idb_` prefix under LIKE wildcards.

var put = globalThis.__brokit_idb_put;
var get = globalThis.__brokit_idb_get;
var count = globalThis.__brokit_idb_count;
var names = globalThis.__brokit_idb_store_names;
var createStore = globalThis.__brokit_idb_create_store;
var deleteStore = globalThis.__brokit_idb_delete_store;
var openNative = globalThis.__brokit_idb_open;
var deleteDb = globalThis.__brokit_idb_delete_db;

var hostileStores = [
    'a]b',
    'x] (key TEXT); DROP TABLE [victim',
    'quote"inside',
    "single'quote",
    'semi;colon',
    '[bracketed]',
    'xxidbXstore'
];

var dbName = 'test_brokit_idb_names';
deleteDb(dbName);
openNative(dbName, 1);
createStore(dbName, 'victim', {});
put(dbName, 'victim', 'k', '"alive"');

for (var i = 0; i < hostileStores.length; i++) {
    var s = hostileStores[i];
    var made = false;
    try { made = createStore(dbName, s, {}); } catch (e) { made = false; }
    assert(made === true, 'create_store accepts ' + JSON.stringify(s));
    put(dbName, s, 'key' + i, JSON.stringify(s));
    assertEqual(get(dbName, s, 'key' + i), JSON.stringify(s), 'value round-trips in ' + JSON.stringify(s));
    assertEqual(count(dbName, s), 1, 'count is 1 in ' + JSON.stringify(s));
}

assertEqual(get(dbName, 'victim', 'k'), '"alive"', 'injection-shaped store names leave other stores intact');

// An autoIncrement store makes SQLite create sqlite_sequence.
createStore(dbName, 'auto', { autoIncrement: true });
var listed = names(dbName);
assert(listed.indexOf('sqlite_sequence') === -1, 'sqlite_sequence is not listed as a store');
for (var j = 0; j < hostileStores.length; j++) {
    assert(listed.indexOf(hostileStores[j]) !== -1, 'store_names lists ' + JSON.stringify(hostileStores[j]));
}
assert(listed.indexOf('victim') !== -1 && listed.indexOf('auto') !== -1, 'plain stores listed');

deleteStore(dbName, 'a]b');
assert(names(dbName).indexOf('a]b') === -1, 'delete_store removes a bracketed name');
deleteDb(dbName);

// Database names with separators or `..` stay inside the IndexedDB directory,
// and two names that differ only in such characters do not share a file.
var trickyDbs = ['../escape_brokit_idb', 'sub/dir_brokit_idb', 'a:b*c?_brokit_idb', '.hidden_brokit_idb'];
for (var k = 0; k < trickyDbs.length; k++) {
    var d = trickyDbs[k];
    deleteDb(d);
    var opened = openNative(d, 1);
    assert(opened && opened.name === d, 'open keeps the database name ' + JSON.stringify(d));
    createStore(d, 'st', {});
    put(d, 'st', 'k', JSON.stringify(d));
    assertEqual(get(d, 'st', 'k'), JSON.stringify(d), 'value round-trips in database ' + JSON.stringify(d));
}
assertEqual(get('escape_brokit_idb', 'st', 'k'), undefined, "'../x' does not alias 'x'");
var fs = globalThis.__brokit_fs;
if (fs && fs.existsSync) {
    assert(!fs.existsSync('../escape_brokit_idb.idb'), 'no database file is written outside the directory');
}
for (var m = 0; m < trickyDbs.length; m++) deleteDb(trickyDbs[m]);
deleteDb('escape_brokit_idb');
