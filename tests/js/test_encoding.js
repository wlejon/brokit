// Test: TextEncoder / TextDecoder
assert(typeof TextEncoder === 'function', 'TextEncoder exists');
assert(typeof TextDecoder === 'function', 'TextDecoder exists');

var enc = new TextEncoder();
assertEqual(enc.encoding, 'utf-8', 'TextEncoder.encoding');

// Encode ASCII
var encoded = enc.encode('hello');
assert(encoded instanceof Uint8Array, 'encode returns Uint8Array');
assertEqual(encoded.length, 5, 'ASCII encode length');
assertEqual(encoded[0], 104, 'h = 104');
assertEqual(encoded[1], 101, 'e = 101');
assertEqual(encoded[4], 111, 'o = 111');

// Decode ASCII
var dec = new TextDecoder();
assertEqual(dec.encoding, 'utf-8', 'TextDecoder.encoding');

var decoded = dec.decode(encoded);
assertEqual(decoded, 'hello', 'decode ASCII roundtrip');

// Empty string
assertEqual(enc.encode('').length, 0, 'encode empty string');
assertEqual(dec.decode(new Uint8Array(0)), '', 'decode empty array');
assertEqual(dec.decode(), '', 'decode undefined');
assertEqual(dec.decode(null), '', 'decode null');

// UTF-8 multibyte
var utf8 = enc.encode('\u00e9');  // é (2 bytes in UTF-8)
assertEqual(utf8.length, 2, 'é is 2 bytes');
assertEqual(utf8[0], 0xC3, 'é byte 0');
assertEqual(utf8[1], 0xA9, 'é byte 1');
assertEqual(dec.decode(utf8), '\u00e9', 'decode é roundtrip');

// 3-byte UTF-8 (CJK)
var cjk = enc.encode('\u4e16');  // 世
assertEqual(cjk.length, 3, '世 is 3 bytes');
assertEqual(dec.decode(cjk), '\u4e16', 'decode 世 roundtrip');

// Longer string roundtrip
var phrase = 'Hello, 世界! 🌍';
var rt = dec.decode(enc.encode(phrase));
assertEqual(rt, phrase, 'full roundtrip with mixed chars');

// Decode from ArrayBuffer
var ab = new ArrayBuffer(5);
var view = new Uint8Array(ab);
view[0] = 72; view[1] = 101; view[2] = 108; view[3] = 108; view[4] = 111;
assertEqual(dec.decode(ab), 'Hello', 'decode ArrayBuffer');

// Test encodeInto
var dest = new Uint8Array(10);
var res = enc.encodeInto('hello', dest);
assertEqual(res.read, 5, 'encodeInto ASCII read');
assertEqual(res.written, 5, 'encodeInto ASCII written');
assertEqual(dest[0], 104, 'encodeInto ASCII byte 0');

// encodeInto truncation: dest too small for 3-byte char
var destSmall = new Uint8Array(2);
res = enc.encodeInto('\u4e16', destSmall); // 世 requires 3 bytes
assertEqual(res.read, 0, 'encodeInto does not consume char if bytes cannot fit');
assertEqual(res.written, 0, 'encodeInto does not write partial bytes');
assertEqual(destSmall[0], 0, 'destSmall remained untouched');

// encodeInto with surrogate pair (emoji 4 bytes)
var destEmoji = new Uint8Array(3);
res = enc.encodeInto('🌍', destEmoji); // 4 bytes, 2 UTF-16 code units
assertEqual(res.read, 0, 'encodeInto emoji does not consume 2 code units when < 4 bytes');
assertEqual(res.written, 0, 'encodeInto emoji written 0');

var destEmoji4 = new Uint8Array(4);
res = enc.encodeInto('🌍', destEmoji4);
assertEqual(res.read, 2, 'encodeInto emoji read 2 code units');
assertEqual(res.written, 4, 'encodeInto emoji written 4 bytes');
assertEqual(dec.decode(destEmoji4), '🌍', 'encodeInto emoji roundtrip');

