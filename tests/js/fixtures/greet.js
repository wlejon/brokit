// Fixture for test_require_file.js. Exports via `module.exports = {...}`, and
// requires a sibling with a bare relative specifier — which must resolve against
// THIS file's directory, not the process working directory.
const { LOUD } = require('./shout');

let loads = 0;
loads++;

module.exports = {
  greet: (who) => `hello, ${who}`,
  shout: (who) => LOUD(`hello, ${who}`),
  loads: () => loads,
  dir: __dirname,
  file: __filename,
};
