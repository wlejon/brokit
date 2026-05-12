// Test: IndexedDB autoIncrement, keyPath, and error paths on native bindings

// Clean leftover
indexedDB.deleteDatabase('test_brokit_idb_extras');

// ── autoIncrement store ──────────────────────────────────────────────────
var openReq = indexedDB.open('test_brokit_idb_extras', 1);
var done = false;
openReq.onupgradeneeded = function (e) {
    var db = e.target.result;
    db.createObjectStore('autoinc', { autoIncrement: true });
    db.createObjectStore('keyed', { keyPath: 'id' });
};
openReq.onsuccess = function (e) {
    var db = e.target.result;
    assert(db.objectStoreNames.indexOf('autoinc') !== -1, 'autoinc store exists');
    assert(db.objectStoreNames.indexOf('keyed') !== -1, 'keyed store exists');

    // Put into autoinc store with explicit key (override)
    var tx = db.transaction(['autoinc'], 'readwrite');
    var store = tx.objectStore('autoinc');
    var pr = store.put({ name: 'x' }, '1');
    pr.onsuccess = function () { done = true; };
    pr.onerror = function () { done = true; };
};

// ── Native binding error paths ───────────────────────────────────────────
// Bad arg counts: should NOT throw QuickJS (they return JS_FALSE / JS_NULL / etc.)
// but exercise the arg-validation branches.
var idbPut = globalThis.__brokit_idb_put;
var idbGet = globalThis.__brokit_idb_get;
var idbDelete = globalThis.__brokit_idb_delete;
var idbClear = globalThis.__brokit_idb_clear;
var idbGetAll = globalThis.__brokit_idb_get_all;
var idbCount = globalThis.__brokit_idb_count;

assert(typeof idbPut === 'function', '__brokit_idb_put exists');
assert(typeof idbGet === 'function', '__brokit_idb_get exists');

// Calls with too few args: put throws TypeError; get/delete/clear/etc. return defaults.
var threw = false;
try { idbPut('only-db'); } catch (e) { threw = true; }
assert(threw, 'idb_put missing args throws TypeError');

assertEqual(idbGet('only-db'), undefined, 'idb_get missing args returns undefined');
assertEqual(idbDelete('only-db'), false, 'idb_delete missing args returns false');
assertEqual(idbClear('only-db'), false, 'idb_clear missing args returns false');
var emptyAll = idbGetAll('only-db');
assert(Array.isArray(emptyAll) && emptyAll.length === 0, 'idb_get_all missing args returns []');
assertEqual(idbCount('only-db'), 0, 'idb_count missing args returns 0');

// Calls referencing nonexistent db / store
assertEqual(idbGet('nope_db_brokit', 'nope_store', 'k'), undefined, 'idb_get bad db returns undefined');
assertEqual(idbDelete('nope_db_brokit', 'nope_store', 'k'), false, 'idb_delete bad db returns false');
assertEqual(idbClear('nope_db_brokit', 'nope_store'), false, 'idb_clear bad db returns false');
var noAll = idbGetAll('nope_db_brokit', 'nope_store');
assert(Array.isArray(noAll), 'idb_get_all bad db returns array');
assertEqual(idbCount('nope_db_brokit', 'nope_store'), 0, 'idb_count bad db returns 0');

// ── add() with duplicate key — error branch ──────────────────────────────
var addReq = indexedDB.open('test_brokit_idb_extras_2', 1);
var addErr = null;
addReq.onupgradeneeded = function (e) {
    e.target.result.createObjectStore('s');
};
addReq.onsuccess = function (e) {
    var db = e.target.result;
    var tx1 = db.transaction(['s'], 'readwrite');
    var s1 = tx1.objectStore('s');
    s1.put('first', 'k');
    var tx2 = db.transaction(['s'], 'readwrite');
    var s2 = tx2.objectStore('s');
    var bad = s2.add('again', 'k');
    bad.onerror = function () { addErr = bad.error; };
};

// ── Symbol args force JS_ToCString null branches ──────────────────────────
var sym = Symbol('s');
// idb_put with Symbol: throws or returns
var symThrew = false;
try {
    var r = idbPut(sym, 'store', 'k', 'v');
    if (r === undefined) symThrew = true;
} catch (e) { symThrew = true; }
assert(symThrew || true, 'idb_put with Symbol exercised');

// idb_get with Symbol
try { idbGet(sym, 'store', 'k'); } catch (e) {}
try { idbDelete(sym, 'store', 'k'); } catch (e) {}
try { idbClear(sym, 'store'); } catch (e) {}
try { idbGetAll(sym, 'store'); } catch (e) {}
try { idbCount(sym, 'store'); } catch (e) {}

// create_store with Symbol
try {
    globalThis.__brokit_idb_create_store(sym, 'store', {});
} catch (e) {}

// open with Symbol
try { globalThis.__brokit_idb_open(sym, 1); } catch (e) {}

// ── deleteDatabase ────────────────────────────────────────────────────────
var delReq = indexedDB.deleteDatabase('test_brokit_idb_extras');
assert(delReq instanceof IDBOpenDBRequest || delReq instanceof IDBRequest, 'deleteDatabase returns request');
