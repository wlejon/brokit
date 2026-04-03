// Test: atob / btoa

assert(typeof btoa === 'function', 'btoa exists');
assert(typeof atob === 'function', 'atob exists');

// --- btoa basics ---
assertEqual(btoa(''), '', 'btoa empty');
assertEqual(btoa('f'), 'Zg==', 'btoa single char');
assertEqual(btoa('fo'), 'Zm8=', 'btoa two chars');
assertEqual(btoa('foo'), 'Zm9v', 'btoa three chars');
assertEqual(btoa('foob'), 'Zm9vYg==', 'btoa four chars');
assertEqual(btoa('fooba'), 'Zm9vYmE=', 'btoa five chars');
assertEqual(btoa('foobar'), 'Zm9vYmFy', 'btoa six chars');

// --- btoa with binary data ---
assertEqual(btoa('\x00\x01\x02'), 'AAEC', 'btoa binary');
assertEqual(btoa('\xff\xfe'), '//4=', 'btoa high bytes');
assertEqual(btoa('Hello, World!'), 'SGVsbG8sIFdvcmxkIQ==', 'btoa hello world');

// --- btoa throws on non-Latin1 ---
var threw = false;
try { btoa('\u0100'); } catch(e) { threw = true; }
assert(threw, 'btoa throws on non-Latin1');

// --- atob basics ---
assertEqual(atob(''), '', 'atob empty');
assertEqual(atob('Zg=='), 'f', 'atob single char');
assertEqual(atob('Zm8='), 'fo', 'atob two chars');
assertEqual(atob('Zm9v'), 'foo', 'atob three chars');
assertEqual(atob('Zm9vYmFy'), 'foobar', 'atob six chars');
assertEqual(atob('SGVsbG8sIFdvcmxkIQ=='), 'Hello, World!', 'atob hello world');

// --- roundtrip ---
var testStr = 'The quick brown fox';
assertEqual(atob(btoa(testStr)), testStr, 'roundtrip text');

// Binary roundtrip
var binary = '';
for (var i = 0; i < 256; i++) binary += String.fromCharCode(i);
assertEqual(atob(btoa(binary)), binary, 'roundtrip all bytes');

// --- atob ignores whitespace ---
assertEqual(atob('Zm 9v'), 'foo', 'atob ignores spaces');
assertEqual(atob('Zm\n9v'), 'foo', 'atob ignores newlines');

// --- atob throws on bad length ---
var threw2 = false;
try { atob('A'); } catch(e) { threw2 = true; }
assert(threw2, 'atob throws on bad length');
