// Test: Node-compatible `sqlite` (`node:sqlite`) module over bundled sqlite3.
// Covers DatabaseSync (:memory: and file WAL), exec, prepare().run/get/all,
// positional and named parameters, transactions with rollback, and blobs.

const sqlite = require('sqlite');
const fs = require('fs');

assertEqual(require('node:sqlite'), sqlite, 'require(node:sqlite) === require(sqlite)');
assert(typeof sqlite.DatabaseSync === 'function', 'DatabaseSync is constructor');
assertEqual(sqlite.Database, sqlite.DatabaseSync, 'Database aliases DatabaseSync');

const db = new sqlite.DatabaseSync(':memory:');
assertEqual(db.isOpen, true, 'db is open');

db.exec(`
    PRAGMA journal_mode = WAL;
    CREATE TABLE issues (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        title TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'new',
        priority INTEGER NOT NULL DEFAULT 0,
        score REAL,
        payload BLOB
    );
`);

const insertPos = db.prepare(
    'INSERT INTO issues (title, status, priority, score, payload) VALUES (?, ?, ?, ?, ?)'
);
const r1 = insertPos.run('First issue', 'new', 2, 1.5, new Uint8Array([10, 20, 30]));
assertEqual(r1.changes, 1, 'insert changes === 1');
assertEqual(r1.lastInsertRowid, 1, 'insert lastInsertRowid === 1');

const insertNamed = db.prepare(
    'INSERT INTO issues (title, status, priority, score, payload) VALUES (:title, :status, :priority, :score, :payload)'
);
const r2 = insertNamed.run({
    title: 'Second issue',
    status: 'ready',
    priority: 5,
    score: null,
    payload: null
});
assertEqual(r2.changes, 1, 'named insert changes === 1');
assertEqual(r2.lastInsertRowid, 2, 'named insert lastInsertRowid === 2');

const getById = db.prepare('SELECT * FROM issues WHERE id = ?');
const row1 = getById.get(1);
assertEqual(row1.id, 1, 'row1.id === 1');
assertEqual(row1.title, 'First issue', 'row1.title');
assertEqual(row1.status, 'new', 'row1.status');
assertEqual(row1.priority, 2, 'row1.priority');
assertEqual(row1.score, 1.5, 'row1.score');
assert(row1.payload instanceof Uint8Array && row1.payload.length === 3, 'row1.payload is Uint8Array(3)');
assertEqual(row1.payload[0], 10, 'blob byte 0');
assertEqual(row1.payload[2], 30, 'blob byte 2');

const row2 = getById.get(2);
assertEqual(row2.title, 'Second issue', 'row2.title');
assertEqual(row2.score, null, 'NULL column maps to null');

const missing = getById.get(999);
assertEqual(missing, undefined, 'missing row returns undefined');

// Transaction commit and rollback
const addBatch = db.transaction((titles) => {
    for (let i = 0; i < titles.length; i++) {
        if (!titles[i]) throw new Error('empty title');
        insertPos.run(titles[i], 'new', 1, 0.0, null);
    }
    return titles.length;
});

assertEqual(addBatch(['Third issue', 'Fourth issue']), 2, 'transaction committed');
assertEqual(db.prepare('SELECT COUNT(*) AS n FROM issues').get().n, 4, '4 rows after commit');

let rolledBack = false;
try {
    addBatch(['Fifth issue', '']);
} catch (e) {
    rolledBack = true;
}
assert(rolledBack, 'failing transaction threw');
assertEqual(db.prepare('SELECT COUNT(*) AS n FROM issues').get().n, 4, 'still 4 rows after rollback');

const allRows = db.prepare('SELECT id, title, status FROM issues ORDER BY id ASC').all();
assertEqual(allRows.length, 4, 'all() returns 4 rows');
assertEqual(allRows[3].title, 'Fourth issue', 'fourth row title');

db.close();
assertEqual(db.isOpen, false, 'db.isOpen false after close');

let threwAfterClose = false;
try {
    db.exec('SELECT 1');
} catch (e) {
    threwAfterClose = true;
}
assert(threwAfterClose, 'exec after close throws');

// On-disk WAL persistence check
const diskFile = 'test_board.sqlite';
try { fs.unlinkSync(diskFile); } catch (_) {}
const diskDb = new sqlite.DatabaseSync(diskFile);
diskDb.exec('PRAGMA journal_mode = WAL; CREATE TABLE kv (k TEXT PRIMARY KEY, v TEXT);');
diskDb.prepare('INSERT INTO kv (k, v) VALUES (?, ?)').run('hello', 'world');
diskDb.close();

const diskDb2 = new sqlite.DatabaseSync(diskFile);
const kvRow = diskDb2.prepare('SELECT v FROM kv WHERE k = ?').get('hello');
assertEqual(kvRow.v, 'world', 'on-disk SQLite persisted across open/close');
diskDb2.close();
try { fs.unlinkSync(diskFile); } catch (_) {}
