// Fixture: proves a module is evaluated ONCE. Two requires of this file must hand
// back the same object, not two objects — otherwise module state is a lie.
module.exports = { n: 0 };
