// Test: localStorage and sessionStorage

// ── API existence ────────────────────────────────────────────────────────
assert(typeof localStorage === 'object', 'localStorage exists');
assert(typeof sessionStorage === 'object', 'sessionStorage exists');

// ── localStorage API ─────────────────────────────────────────────────────
assert(typeof localStorage.getItem === 'function', 'ls.getItem');
assert(typeof localStorage.setItem === 'function', 'ls.setItem');
assert(typeof localStorage.removeItem === 'function', 'ls.removeItem');
assert(typeof localStorage.clear === 'function', 'ls.clear');
assert(typeof localStorage.key === 'function', 'ls.key');
assertEqual(typeof localStorage.length, 'number', 'ls.length is number');

// ── sessionStorage API ───────────────────────────────────────────────────
assert(typeof sessionStorage.getItem === 'function', 'ss.getItem');
assert(typeof sessionStorage.setItem === 'function', 'ss.setItem');
assert(typeof sessionStorage.removeItem === 'function', 'ss.removeItem');
assert(typeof sessionStorage.clear === 'function', 'ss.clear');
assert(typeof sessionStorage.key === 'function', 'ss.key');
assertEqual(typeof sessionStorage.length, 'number', 'ss.length is number');

// ── Test localStorage operations ─────────────────────────────────────────
localStorage.clear();
assertEqual(localStorage.length, 0, 'ls empty after clear');

// getItem returns null for missing key
assertEqual(localStorage.getItem('missing'), null, 'ls.getItem missing returns null');

// setItem / getItem
localStorage.setItem('key1', 'value1');
assertEqual(localStorage.getItem('key1'), 'value1', 'ls.getItem after setItem');
assertEqual(localStorage.length, 1, 'ls.length is 1');

localStorage.setItem('key2', 'value2');
assertEqual(localStorage.getItem('key2'), 'value2', 'ls.getItem key2');
assertEqual(localStorage.length, 2, 'ls.length is 2');

// setItem overwrites
localStorage.setItem('key1', 'updated');
assertEqual(localStorage.getItem('key1'), 'updated', 'ls.setItem overwrites');
assertEqual(localStorage.length, 2, 'ls.length still 2 after overwrite');

// key()
var k0 = localStorage.key(0);
var k1 = localStorage.key(1);
assert(k0 !== null, 'ls.key(0) not null');
assert(k1 !== null, 'ls.key(1) not null');
assert(k0 !== k1, 'ls.key(0) !== ls.key(1)');
assertEqual(localStorage.key(2), null, 'ls.key(2) out of bounds');
assertEqual(localStorage.key(-1), null, 'ls.key(-1) out of bounds');

// removeItem
localStorage.removeItem('key1');
assertEqual(localStorage.getItem('key1'), null, 'ls.getItem after remove');
assertEqual(localStorage.length, 1, 'ls.length after remove');

// removeItem nonexistent — no error
localStorage.removeItem('nonexistent');
assertEqual(localStorage.length, 1, 'ls.length unchanged after removing nonexistent');

// clear
localStorage.setItem('a', '1');
localStorage.setItem('b', '2');
localStorage.clear();
assertEqual(localStorage.length, 0, 'ls.length 0 after clear');
assertEqual(localStorage.getItem('a'), null, 'ls.getItem null after clear');

// ── Values are coerced to strings ────────────────────────────────────────
localStorage.setItem('num', '42');
assertEqual(localStorage.getItem('num'), '42', 'ls stores string values');

// ── Special characters ───────────────────────────────────────────────────
localStorage.setItem('special', 'hello\nworld\t!');
assertEqual(localStorage.getItem('special'), 'hello\nworld\t!', 'ls handles special chars');

localStorage.setItem('quotes', 'say "hello"');
assertEqual(localStorage.getItem('quotes'), 'say "hello"', 'ls handles quotes');

localStorage.setItem('backslash', 'path\\to\\file');
assertEqual(localStorage.getItem('backslash'), 'path\\to\\file', 'ls handles backslashes');

localStorage.clear();

// ── Test sessionStorage operations ───────────────────────────────────────
sessionStorage.clear();
assertEqual(sessionStorage.length, 0, 'ss empty after clear');

assertEqual(sessionStorage.getItem('missing'), null, 'ss.getItem missing');

sessionStorage.setItem('s1', 'v1');
assertEqual(sessionStorage.getItem('s1'), 'v1', 'ss.getItem');
assertEqual(sessionStorage.length, 1, 'ss.length 1');

sessionStorage.setItem('s2', 'v2');
sessionStorage.setItem('s3', 'v3');
assertEqual(sessionStorage.length, 3, 'ss.length 3');

sessionStorage.removeItem('s2');
assertEqual(sessionStorage.getItem('s2'), null, 'ss.getItem after remove');
assertEqual(sessionStorage.length, 2, 'ss.length after remove');

assertEqual(sessionStorage.key(0), 's1', 'ss.key(0)');

sessionStorage.clear();
assertEqual(sessionStorage.length, 0, 'ss.length 0 after clear');

// ── localStorage and sessionStorage are independent ──────────────────────
localStorage.setItem('shared', 'local');
sessionStorage.setItem('shared', 'session');
assertEqual(localStorage.getItem('shared'), 'local', 'ls independent from ss');
assertEqual(sessionStorage.getItem('shared'), 'session', 'ss independent from ls');
localStorage.clear();
sessionStorage.clear();
