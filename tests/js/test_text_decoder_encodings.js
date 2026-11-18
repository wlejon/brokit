// Test: TextDecoder's labels and encodings (the Encoding Standard, as
// Chromium and Node have it): a label maps to its canonical name, an unknown
// one is a RangeError, UTF-16LE / UTF-16BE and windows-1252 decode as such
// (not as UTF-8), streamed UTF-16 keeps a split code unit or surrogate pair
// for the next chunk, and a leading BOM is dropped unless ignoreBOM. Monaco
// decodes its text buffer with TextDecoder('UTF-16LE'); every character came
// back followed by a NUL.

var bytes = function (arr) { return new Uint8Array(arr); };

assertEqual(new TextDecoder('UTF-16LE').encoding, 'utf-16le', 'utf-16le label');
assertEqual(new TextDecoder('utf-16').encoding, 'utf-16le', 'utf-16 is utf-16le');
assertEqual(new TextDecoder(' latin1 ').encoding, 'windows-1252', 'latin1 is windows-1252, trimmed');
assertEqual(new TextDecoder('utf8').encoding, 'utf-8', 'utf8 label');
var threw = false;
try { new TextDecoder('no-such-encoding'); } catch (e) { threw = e instanceof RangeError; }
assert(threw, 'an unknown label is a RangeError');

assertEqual(new TextDecoder('utf-16le').decode(bytes([104, 0, 105, 0])), 'hi', 'UTF-16LE');
assertEqual(new TextDecoder('utf-16be').decode(bytes([0x68, 0x00, 0x69, 0x00])), '栀椀', 'UTF-16BE');
assertEqual(new TextDecoder('utf-16le').decode(bytes([0x3D, 0xD8, 0x00, 0xDE])), '😀', 'a surrogate pair');
assertEqual(new TextDecoder('utf-16le').decode(bytes([0xFF, 0xFE, 0x41, 0x00])), 'A', 'the BOM is dropped');
assertEqual(new TextDecoder('utf-16le', { ignoreBOM: true }).decode(bytes([0xFF, 0xFE, 0x41, 0x00])), '﻿A', 'ignoreBOM keeps it');
assertEqual(new TextDecoder('utf-16le').decode(bytes([0x41, 0x00, 0x42])), 'A�', 'an odd trailing byte is U+FFFD');

var s = new TextDecoder('utf-16le');
var streamed = s.decode(bytes([0x41, 0x00, 0x3D]), { stream: true }) + s.decode(bytes([0xD8, 0x00]), { stream: true }) +
               s.decode(bytes([0xDE]));
assertEqual(streamed, 'A😀', 'streamed UTF-16 joins split units and pairs');

assertEqual(new TextDecoder('windows-1252').decode(bytes([0x80, 0x41, 0xE9, 0x9F])), '€AéŸ', 'windows-1252');
assertEqual(new TextDecoder().decode(bytes([0xEF, 0xBB, 0xBF, 0x68, 0x69])), 'hi', 'the UTF-8 BOM is dropped');
assertEqual(new TextDecoder('utf-8', { ignoreBOM: true }).decode(bytes([0xEF, 0xBB, 0xBF, 0x68])), '﻿h', 'UTF-8 ignoreBOM');
