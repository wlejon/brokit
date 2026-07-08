# embed_js.cmake — Convert a .js file into a C++ header with the content as a
# NUL-terminated byte array.
#
# A byte-array initializer is used (rather than a raw string literal) so an
# embedded source file can exceed MSVC's ~16 KB single-string-literal limit
# (error C2026). The generated symbol keeps type `const char[]`, so existing
# consumers using `strlen(js_foo)` / `sizeof` are unaffected.
#
# Usage (from add_custom_command):
#   cmake -DINPUT=abort.js -DOUTPUT=abort.js.h -DVAR_NAME=js_abort -P embed_js.cmake
#
# Produces a header like:
#   // Auto-generated from abort.js — do not edit.
#   #pragma once
#   static const char js_abort[] = {
#   0x2f, 0x2a, ..., 0x00 };

file(READ "${INPUT}" JS_HEX HEX)
get_filename_component(INPUT_NAME "${INPUT}" NAME)

# Turn the flat hex string ("2f2a...") into "0xNN," byte initializers, wrapping
# lines periodically so the generated header stays readable.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," JS_BYTES "${JS_HEX}")
string(REGEX REPLACE "((0x..,){20})" "\\1\n" JS_BYTES "${JS_BYTES}")

file(WRITE "${OUTPUT}"
"// Auto-generated from ${INPUT_NAME} — do not edit.\n\
#pragma once\n\
static const char ${VAR_NAME}[] = {\n\
${JS_BYTES} 0x00 };\n"
)
