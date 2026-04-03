// Test: navigator

assert(typeof navigator === 'object', 'navigator exists');
assert(typeof navigator.userAgent === 'string', 'navigator.userAgent is string');
assert(navigator.userAgent.length > 0, 'navigator.userAgent not empty');
assert(typeof navigator.language === 'string', 'navigator.language');
assert(Array.isArray(navigator.languages), 'navigator.languages is array');
assert(navigator.languages.length > 0, 'navigator.languages not empty');
assertEqual(typeof navigator.onLine, 'boolean', 'navigator.onLine is boolean');
assert(navigator.onLine, 'navigator.onLine is true');
assertEqual(typeof navigator.hardwareConcurrency, 'number', 'hardwareConcurrency is number');
assertEqual(typeof navigator.platform, 'string', 'platform is string');
assertEqual(navigator.cookieEnabled, false, 'cookieEnabled is false');
assertEqual(typeof navigator.maxTouchPoints, 'number', 'maxTouchPoints is number');
