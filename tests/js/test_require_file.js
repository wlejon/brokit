// Test: require() of a file on disk — Node-style relative/absolute modules.
//
// Before this, require() resolved only registered built-ins, so a bro app could
// not split its JS across files at all: every helper had to be copy-pasted into
// every script that wanted it. These are the guarantees app code relies on.

const path = require('path');

// --- basic load, and both ways of exporting -------------------------------
const g = require('./fixtures/greet.js');
assertEqual(g.greet('world'), 'hello, world', 'module.exports = {...} works');
assertEqual(g.shout('world'), 'HELLO, WORLD!', 'exports.x = ... works too');

// --- extension is optional ------------------------------------------------
const g2 = require('./fixtures/greet');
assertEqual(g2.greet('again'), 'hello, again', 'resolves without the .js suffix');

// --- a module evaluates exactly once --------------------------------------
// Two requires must return the SAME object. If the loader re-evaluated the file,
// module-level state (a cache, a connection, a counter) would silently fork.
assert(g === g2, 'the same file yields the same exports object');
assertEqual(g.loads(), 1, 'module body ran exactly once');

const c1 = require('./fixtures/counter.js');
c1.n = 7;
const c2 = require('./fixtures/counter.js');
assertEqual(c2.n, 7, 'module state is shared across requires');

// --- relative specifiers resolve against the REQUIRING file ---------------
// greet.js does require('./shout'), which only works if the base directory is
// greet.js's own directory rather than the process working directory. This is the
// bit that makes a module tree possible.
assert(g.shout('x').endsWith('!'), 'a module can require its own sibling');

// --- __dirname / __filename ----------------------------------------------
assert(typeof g.dir === 'string' && g.dir.length > 0, '__dirname is set');
assert(g.file.endsWith('greet.js'), '__filename is the resolved path');
assertEqual(path.dirname(g.file), g.dir, '__dirname is __filename\'s directory');

// --- absolute paths -------------------------------------------------------
const abs = require(g.file);
assert(abs === g, 'an absolute path resolves to the same cached module');

// --- JSON -----------------------------------------------------------------
const data = require('./fixtures/data.json');
assertEqual(data.name, 'brokit', 'JSON module parses');
assertEqual(data.answer, 42, 'JSON numbers survive');
assert(data.nested.ok === true, 'JSON nests');

// --- failure is an error, not a silent undefined --------------------------
let threw = false;
try { require('./fixtures/does_not_exist.js'); } catch (e) { threw = true; }
assert(threw, 'a missing file throws');

// --- built-ins still resolve ----------------------------------------------
// Path specifiers must not shadow the module registry.
assert(typeof require('fs').readFileSync === 'function', 'built-ins still work');
assert(typeof require('node:path').join === 'function', 'node: prefix still works');
