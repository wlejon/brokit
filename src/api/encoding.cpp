#include "api/api.h"
#include "runtime/runtime.h"

#include <cstring>
#include <string>
#include <vector>

namespace brokit::api {

// Native C++ TextEncoder.encode() for correctness + performance.
static JSValue js_textencoder_encode(JSContext* ctx, JSValueConst,
                                      int argc, JSValueConst* argv)
{
    const char* str = "";
    if (argc > 0) {
        str = JS_ToCString(ctx, argv[0]);
        if (!str) return JS_EXCEPTION;
    }

    size_t len = strlen(str);
    // UTF-8 string is already UTF-8 in QuickJS, so we can just copy the bytes
    JSValue buf = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(str), len);
    if (argc > 0) JS_FreeCString(ctx, str);

    if (JS_IsException(buf)) return buf;

    // Wrap in Uint8Array
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue uint8Ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
    JSValue args[] = { buf };
    JSValue result = JS_CallConstructor(ctx, uint8Ctor, 1, args);
    JS_FreeValue(ctx, buf);
    JS_FreeValue(ctx, uint8Ctor);
    JS_FreeValue(ctx, global);
    return result;
}

// Native C++ TextDecoder.decode() for correctness.
static JSValue js_textdecoder_decode(JSContext* ctx, JSValueConst,
                                      int argc, JSValueConst* argv)
{
    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) {
        return JS_NewString(ctx, "");
    }

    size_t byte_len = 0;
    size_t byte_offset = 0;
    size_t bytes_per_element = 0;

    // Try TypedArray first
    JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_offset, &byte_len, &bytes_per_element);
    uint8_t* ptr = nullptr;
    size_t len = 0;

    if (!JS_IsException(buf)) {
        size_t abLen = 0;
        ptr = JS_GetArrayBuffer(ctx, &abLen, buf);
        if (ptr) {
            ptr += byte_offset;
            len = byte_len;
        }
        JS_FreeValue(ctx, buf);
    } else {
        // Clear exception and try ArrayBuffer directly
        JS_FreeValue(ctx, JS_GetException(ctx));
        size_t abLen = 0;
        ptr = JS_GetArrayBuffer(ctx, &abLen, argv[0]);
        len = abLen;
    }

    if (!ptr) {
        return JS_ThrowTypeError(ctx, "TextDecoder.decode: expected ArrayBuffer or TypedArray");
    }

    // QuickJS strings are UTF-8 internally, and we assume UTF-8 input
    return JS_NewStringLen(ctx, reinterpret_cast<const char*>(ptr), len);
}

void installEncoding(JSContext* ctx)
{
    const char* polyfill = R"JS(
(function() {
    globalThis.TextEncoder = function TextEncoder() {
        this.encoding = 'utf-8';
    };
    TextEncoder.prototype.encode = function(str) {
        return globalThis.__brokit_textencoder_encode(str || '');
    };
    TextEncoder.prototype.encodeInto = function(str, dest) {
        var encoded = this.encode(str);
        var len = Math.min(encoded.length, dest.length);
        for (var i = 0; i < len; i++) dest[i] = encoded[i];
        return { read: str.length, written: len };
    };

    globalThis.TextDecoder = function TextDecoder(encoding) {
        this.encoding = (encoding || 'utf-8').toLowerCase();
        this.fatal = false;
        this.ignoreBOM = false;
    };
    TextDecoder.prototype.decode = function(input) {
        if (!input) return '';
        return globalThis.__brokit_textdecoder_decode(input);
    };
})();
)JS";

    JSValue global = JS_GetGlobalObject(ctx);

    JS_SetPropertyStr(ctx, global, "__brokit_textencoder_encode",
                      JS_NewCFunction(ctx, js_textencoder_encode, "__brokit_textencoder_encode", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_textdecoder_decode",
                      JS_NewCFunction(ctx, js_textdecoder_decode, "__brokit_textdecoder_decode", 1));

    JS_FreeValue(ctx, global);

    JSValue r = JS_Eval(ctx, polyfill, strlen(polyfill), "<encoding>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
