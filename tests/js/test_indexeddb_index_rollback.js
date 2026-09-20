// Test: IDBKeyRange, IDBIndex, and Transaction Rollback
(function() {
    'use strict';

    // ── 1. IDBKeyRange tests ───────────────────────────────────────────────────
    assert(typeof IDBKeyRange === 'function', 'IDBKeyRange exists');

    var rOnly = IDBKeyRange.only(5);
    assertEqual(rOnly.lower, 5, 'only lower');
    assertEqual(rOnly.upper, 5, 'only upper');
    assertEqual(rOnly.lowerOpen, false, 'only lowerOpen false');
    assertEqual(rOnly.upperOpen, false, 'only upperOpen false');
    assert(rOnly.includes(5), 'only includes 5');
    assert(!rOnly.includes(4), 'only not includes 4');
    assert(!rOnly.includes(6), 'only not includes 6');

    var rLower = IDBKeyRange.lowerBound(10, true);
    assertEqual(rLower.lower, 10, 'lowerBound lower');
    assertEqual(rLower.lowerOpen, true, 'lowerBound lowerOpen true');
    assert(!rLower.includes(10), 'lowerBound open excludes bound');
    assert(rLower.includes(11), 'lowerBound open includes 11');
    assert(!rLower.includes(9), 'lowerBound open excludes 9');

    var rUpper = IDBKeyRange.upperBound(20, false);
    assertEqual(rUpper.upper, 20, 'upperBound upper');
    assertEqual(rUpper.upperOpen, false, 'upperBound upperOpen false');
    assert(rUpper.includes(20), 'upperBound closed includes bound');
    assert(!rUpper.includes(21), 'upperBound closed excludes 21');

    var rBound = IDBKeyRange.bound(5, 15, false, true);
    assert(rBound.includes(5), 'bound lower closed includes 5');
    assert(!rBound.includes(15), 'bound upper open excludes 15');
    assert(rBound.includes(10), 'bound includes 10');
    assert(!rBound.includes(4), 'bound excludes 4');
    assert(!rBound.includes(16), 'bound excludes 16');

    // ── 2. Database with IDBIndex & Rollback ──────────────────────────────────
    var dbName = 'test_idb_index_rollback';
    indexedDB.deleteDatabase(dbName);

    var openReq = indexedDB.open(dbName, 1);
    openReq.onupgradeneeded = function(e) {
        var db = e.target.result;
        var store = db.createObjectStore('people', { keyPath: 'id' });
        assert(typeof store.createIndex === 'function', 'store.createIndex exists');
        var idx = store.createIndex('by_age', 'age', { unique: false });
        assert(idx instanceof IDBIndex, 'createIndex returns IDBIndex');
        assertEqual(idx.name, 'by_age', 'index name');
        assertEqual(idx.keyPath, 'age', 'index keyPath');
        assert(store.indexNames.indexOf('by_age') !== -1, 'indexNames includes by_age');
    };

    openReq.onsuccess = function(e) {
        var db = e.target.result;
        testIndexOperations(db);
    };

    function testIndexOperations(db) {
        var tx = db.transaction(['people'], 'readwrite');
        var store = tx.objectStore('people');
        store.put({ id: 'p1', name: 'Alice', age: 25 }, 'p1');
        store.put({ id: 'p2', name: 'Bob', age: 30 }, 'p2');
        store.put({ id: 'p3', name: 'Charlie', age: 25 }, 'p3');

        tx.oncomplete = function() {
            var txRead = db.transaction(['people'], 'readonly');
            var s = txRead.objectStore('people');
            var idx = s.index('by_age');
            assert(idx instanceof IDBIndex, 'store.index returns IDBIndex');

            // idx.get
            var getReq = idx.get(30);
            getReq.onsuccess = function() {
                assertEqual(getReq.result.name, 'Bob', 'idx.get matching record');

                // idx.getKey
                var getKeyReq = idx.getKey(30);
                getKeyReq.onsuccess = function() {
                    assertEqual(getKeyReq.result, 'p2', 'idx.getKey matching primary key');

                    // idx.getAll
                    var getAllReq = idx.getAll(25);
                    getAllReq.onsuccess = function() {
                        assertEqual(getAllReq.result.length, 2, 'idx.getAll returns 2 matching records');

                        // idx.getAllKeys
                        var getAllKeysReq = idx.getAllKeys(25);
                        getAllKeysReq.onsuccess = function() {
                            assertEqual(getAllKeysReq.result.length, 2, 'idx.getAllKeys length 2');
                            assert(getAllKeysReq.result.indexOf('p1') !== -1, 'getAllKeys contains p1');
                            assert(getAllKeysReq.result.indexOf('p3') !== -1, 'getAllKeys contains p3');

                            // idx.count
                            var countReq = idx.count(25);
                            countReq.onsuccess = function() {
                                assertEqual(countReq.result, 2, 'idx.count returns 2');

                                // idx with IDBKeyRange
                                var rangeReq = idx.getAll(IDBKeyRange.bound(20, 28));
                                rangeReq.onsuccess = function() {
                                    assertEqual(rangeReq.result.length, 2, 'idx.getAll with IDBKeyRange');
                                    testExplicitAbortRollback(db);
                                };
                            };
                        };
                    };
                };
            };
        };
    }

    // ── 3. Explicit tx.abort() Rollback ──────────────────────────────────────
    function testExplicitAbortRollback(db) {
        var tx = db.transaction(['people'], 'readwrite');
        var store = tx.objectStore('people');
        store.put({ id: 'p4', name: 'Dave', age: 40 }, 'p4');

        var abortFired = false;
        var completeFired = false;
        tx.onabort = function() { abortFired = true; };
        tx.oncomplete = function() { completeFired = true; };

        tx.abort();

        queueMicrotask(function() {
            queueMicrotask(function() {
                assert(abortFired, 'tx.onabort fired after abort()');
                assert(!completeFired, 'tx.oncomplete did not fire after abort()');

                // Verify p4 was rolled back and does not exist in store
                var txCheck = db.transaction(['people'], 'readonly');
                var sCheck = txCheck.objectStore('people');
                var checkReq = sCheck.get('p4');
                checkReq.onsuccess = function() {
                    assertEqual(checkReq.result, undefined, 'p4 rolled back after tx.abort()');
                    testErrorRollback(db);
                };
            });
        });
    }

    // ── 4. Request Error Rollback (ConstraintError on duplicate key) ──────────
    function testErrorRollback(db) {
        var tx = db.transaction(['people'], 'readwrite');
        var store = tx.objectStore('people');

        // Put a new item
        store.put({ id: 'p5', name: 'Eve', age: 50 }, 'p5');

        // Modify an existing item
        store.put({ id: 'p1', name: 'Alice Modified', age: 99 }, 'p1');

        // Trigger error: add() with existing key 'p2'
        var badAdd = store.add({ id: 'p2_dup', name: 'Duplicate', age: 100 }, 'p2');
        var reqErrorFired = false;
        var txErrorFired = false;
        var txAbortFired = false;
        var txCompleteFired = false;

        badAdd.onerror = function(e) {
            reqErrorFired = true;
            assertEqual(badAdd.error.name, 'ConstraintError', 'badAdd error is ConstraintError');
        };

        tx.onerror = function() { txErrorFired = true; };
        tx.onabort = function() { txAbortFired = true; };
        tx.oncomplete = function() { txCompleteFired = true; };

        queueMicrotask(function() {
            queueMicrotask(function() {
                queueMicrotask(function() {
                    assert(reqErrorFired, 'badAdd request onerror fired');
                    assert(txErrorFired, 'transaction onerror fired');
                    assert(txAbortFired, 'transaction onabort fired');
                    assert(!txCompleteFired, 'transaction oncomplete did not fire on error');

                    // Check that both p5 (inserted) and p1 (modified) were rolled back!
                    var txVerify = db.transaction(['people'], 'readonly');
                    var sVerify = txVerify.objectStore('people');

                    var getP5 = sVerify.get('p5');
                    getP5.onsuccess = function() {
                        assertEqual(getP5.result, undefined, 'p5 was rolled back (not present)');

                        var getP1 = sVerify.get('p1');
                        getP1.onsuccess = function() {
                            assertEqual(getP1.result.name, 'Alice', 'p1 modification was rolled back to Alice');
                            assertEqual(getP1.result.age, 25, 'p1 age was rolled back to 25');
                        };
                    };
                });
            });
        });
    }
})();
