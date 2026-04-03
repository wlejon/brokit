// Test: IndexedDB

// ── API existence ────────────────────────────────────────────────────────
assert(typeof indexedDB === 'object', 'indexedDB exists');
assert(typeof indexedDB.open === 'function', 'indexedDB.open');
assert(typeof indexedDB.deleteDatabase === 'function', 'indexedDB.deleteDatabase');
assert(typeof IDBRequest === 'function', 'IDBRequest exists');
assert(typeof IDBOpenDBRequest === 'function', 'IDBOpenDBRequest exists');
assert(typeof IDBDatabase === 'function', 'IDBDatabase exists');
assert(typeof IDBTransaction === 'function', 'IDBTransaction exists');
assert(typeof IDBObjectStore === 'function', 'IDBObjectStore exists');

// ── Clean up any leftover test database ──────────────────────────────────
indexedDB.deleteDatabase('test_brokit_idb');

// ── Open database with upgrade ───────────────────────────────────────────
var upgradeCalled = false;
var openReq = indexedDB.open('test_brokit_idb', 1);
assert(openReq instanceof IDBOpenDBRequest, 'open returns IDBOpenDBRequest');
assertEqual(openReq.readyState, 'pending', 'request starts pending');

openReq.onupgradeneeded = function(event) {
    upgradeCalled = true;
    assertEqual(event.type, 'upgradeneeded', 'upgrade event type');
    assertEqual(event.oldVersion, 0, 'oldVersion is 0');
    assertEqual(event.newVersion, 1, 'newVersion is 1');

    var db = event.target.result;
    assert(db instanceof IDBDatabase, 'target.result is IDBDatabase');
    assertEqual(db.name, 'test_brokit_idb', 'db name');
    assertEqual(db.version, 1, 'db version');

    // Create object stores
    db.createObjectStore('users');
    db.createObjectStore('settings');
};

openReq.onsuccess = function(event) {
    assert(upgradeCalled, 'upgrade was called before success');
    var db = event.target.result;
    assertEqual(db.name, 'test_brokit_idb', 'db name on success');
    assertEqual(db.version, 1, 'db version on success');
    assert(db.objectStoreNames.indexOf('users') !== -1, 'users store exists');
    assert(db.objectStoreNames.indexOf('settings') !== -1, 'settings store exists');

    // ── put / get ────────────────────────────────────────────────────────
    var tx = db.transaction(['users'], 'readwrite');
    assert(tx instanceof IDBTransaction, 'transaction instance');
    assertEqual(tx.mode, 'readwrite', 'transaction mode');

    var store = tx.objectStore('users');
    assert(store instanceof IDBObjectStore, 'objectStore instance');
    assertEqual(store.name, 'users', 'store name');

    var putReq = store.put({ name: 'Alice', age: 30 }, 'alice');
    assert(putReq instanceof IDBRequest, 'put returns IDBRequest');

    putReq.onsuccess = function() {
        assertEqual(putReq.result, 'alice', 'put returns key');

        // put more entries
        store.put({ name: 'Bob', age: 25 }, 'bob');
        store.put({ name: 'Carol', age: 35 }, 'carol');

        // get
        var getReq = store.get('alice');
        getReq.onsuccess = function() {
            var user = getReq.result;
            assertEqual(user.name, 'Alice', 'get returns correct name');
            assertEqual(user.age, 30, 'get returns correct age');

            // get nonexistent
            var getReq2 = store.get('nonexistent');
            getReq2.onsuccess = function() {
                assertEqual(getReq2.result, undefined, 'get nonexistent returns undefined');

                // ── count ────────────────────────────────────────────────
                var countReq = store.count();
                countReq.onsuccess = function() {
                    assertEqual(countReq.result, 3, 'count is 3');

                    // ── getAll ───────────────────────────────────────────
                    var allReq = store.getAll();
                    allReq.onsuccess = function() {
                        assertEqual(allReq.result.length, 3, 'getAll returns 3');
                        // Results are sorted by key
                        assertEqual(allReq.result[0].name, 'Alice', 'getAll[0] is Alice');
                        assertEqual(allReq.result[1].name, 'Bob', 'getAll[1] is Bob');
                        assertEqual(allReq.result[2].name, 'Carol', 'getAll[2] is Carol');

                        // getAll with count limit
                        var allReq2 = store.getAll(null, 2);
                        allReq2.onsuccess = function() {
                            assertEqual(allReq2.result.length, 2, 'getAll(2) returns 2');

                            // ── getAllKeys ────────────────────────────────
                            var keysReq = store.getAllKeys();
                            keysReq.onsuccess = function() {
                                assertEqual(keysReq.result.length, 3, 'getAllKeys returns 3');
                                assertEqual(keysReq.result[0], 'alice', 'key 0');
                                assertEqual(keysReq.result[1], 'bob', 'key 1');
                                assertEqual(keysReq.result[2], 'carol', 'key 2');

                                // ── delete ───────────────────────────────
                                var delReq = store.delete('bob');
                                delReq.onsuccess = function() {
                                    var countReq2 = store.count();
                                    countReq2.onsuccess = function() {
                                        assertEqual(countReq2.result, 2, 'count after delete');

                                        // ── put overwrites ──────────────
                                        store.put({ name: 'Alice Updated', age: 31 }, 'alice');
                                        var getReq3 = store.get('alice');
                                        getReq3.onsuccess = function() {
                                            assertEqual(getReq3.result.name, 'Alice Updated', 'put overwrites');

                                            // ── clear ───────────────────
                                            var clearReq = store.clear();
                                            clearReq.onsuccess = function() {
                                                var countReq3 = store.count();
                                                countReq3.onsuccess = function() {
                                                    assertEqual(countReq3.result, 0, 'count after clear');

                                                    // ── Multiple stores ─
                                                    testMultipleStores(db);
                                                };
                                            };
                                        };
                                    };
                                };
                            };
                        };
                    };
                };
            };
        };
    };
};

function testMultipleStores(db) {
    var tx = db.transaction(['settings'], 'readwrite');
    var store = tx.objectStore('settings');

    store.put('dark', 'theme');
    store.put('en-US', 'locale');
    store.put('true', 'notifications');

    var countReq = store.count();
    countReq.onsuccess = function() {
        assertEqual(countReq.result, 3, 'settings count');

        var getReq = store.get('theme');
        getReq.onsuccess = function() {
            assertEqual(getReq.result, 'dark', 'settings get');

            testReopen(db);
        };
    };
}

function testReopen(db) {
    db.close();

    // Reopen same database — should not trigger upgrade
    var upgradeCalled2 = false;
    var openReq2 = indexedDB.open('test_brokit_idb', 1);
    openReq2.onupgradeneeded = function() { upgradeCalled2 = true; };
    openReq2.onsuccess = function(event) {
        assert(!upgradeCalled2, 'no upgrade on reopen with same version');
        var db2 = event.target.result;
        assertEqual(db2.version, 1, 'reopened db version');

        // Data should persist
        var tx = db2.transaction(['settings'], 'readonly');
        var store = tx.objectStore('settings');
        var getReq = store.get('theme');
        getReq.onsuccess = function() {
            assertEqual(getReq.result, 'dark', 'data persists across close/reopen');

            testVersionUpgrade(db2);
        };
    };
}

function testVersionUpgrade(db) {
    db.close();

    var upgrade2Called = false;
    var openReq3 = indexedDB.open('test_brokit_idb', 2);
    openReq3.onupgradeneeded = function(event) {
        upgrade2Called = true;
        assertEqual(event.oldVersion, 1, 'upgrade: oldVersion 1');
        assertEqual(event.newVersion, 2, 'upgrade: newVersion 2');
        var db3 = event.target.result;
        db3.createObjectStore('logs');
    };
    openReq3.onsuccess = function(event) {
        assert(upgrade2Called, 'version upgrade triggered');
        var db3 = event.target.result;
        assertEqual(db3.version, 2, 'upgraded version');
        assert(db3.objectStoreNames.indexOf('logs') !== -1, 'new store exists');

        testDeleteObjectStore(db3);
    };
}

function testDeleteObjectStore(db) {
    db.close();

    var openReq = indexedDB.open('test_brokit_idb', 3);
    openReq.onupgradeneeded = function(event) {
        var db4 = event.target.result;
        db4.deleteObjectStore('logs');
    };
    openReq.onsuccess = function(event) {
        var db4 = event.target.result;
        assert(db4.objectStoreNames.indexOf('logs') === -1, 'deleted store gone');
        db4.close();

        testDeleteDatabase();
    };
}

function testDeleteDatabase() {
    var delReq = indexedDB.deleteDatabase('test_brokit_idb');
    delReq.onsuccess = function() {
        assert(true, 'deleteDatabase succeeded');

        // Reopening should trigger upgrade (version reset)
        var openReq = indexedDB.open('test_brokit_idb', 1);
        openReq.onupgradeneeded = function(event) {
            assertEqual(event.oldVersion, 0, 'fresh db after delete');
        };
        openReq.onsuccess = function(event) {
            event.target.result.close();
            // Final cleanup
            indexedDB.deleteDatabase('test_brokit_idb');
            assert(true, 'full lifecycle complete');
        };
    };
}

// ── Transaction oncomplete ───────────────────────────────────────────────
var txDb = indexedDB.open('test_brokit_tx', 1);
txDb.onupgradeneeded = function(e) {
    e.target.result.createObjectStore('items');
};
txDb.onsuccess = function(e) {
    var db = e.target.result;
    var tx = db.transaction(['items'], 'readwrite');
    var completeFired = false;
    tx.oncomplete = function() {
        completeFired = true;
        assert(true, 'transaction oncomplete fired');
        db.close();
        indexedDB.deleteDatabase('test_brokit_tx');
    };
};

// ── Complex values (objects, arrays, nested) ─────────────────────────────
var complexDb = indexedDB.open('test_brokit_complex', 1);
complexDb.onupgradeneeded = function(e) {
    e.target.result.createObjectStore('data');
};
complexDb.onsuccess = function(e) {
    var db = e.target.result;
    var store = db.transaction(['data'], 'readwrite').objectStore('data');

    var complex = {
        name: 'test',
        nested: { a: 1, b: [2, 3, 4] },
        tags: ['alpha', 'beta'],
        active: true,
        count: 42
    };

    var putReq = store.put(complex, 'complex1');
    putReq.onsuccess = function() {
        var getReq = store.get('complex1');
        getReq.onsuccess = function() {
            var result = getReq.result;
            assertEqual(result.name, 'test', 'complex: name');
            assertEqual(result.nested.a, 1, 'complex: nested.a');
            assertEqual(result.nested.b.length, 3, 'complex: nested.b length');
            assertEqual(result.tags[0], 'alpha', 'complex: tags[0]');
            assertEqual(result.active, true, 'complex: boolean');
            assertEqual(result.count, 42, 'complex: number');

            db.close();
            indexedDB.deleteDatabase('test_brokit_complex');
        };
    };
};
