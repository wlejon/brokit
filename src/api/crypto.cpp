#include "api/api.h"
#include "runtime/runtime.h"

#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace brokit::api {

// Fill buffer with cryptographically secure random bytes.
// Windows: BCryptGenRandom. Linux: /dev/urandom.
static bool fillRandom(uint8_t* buf, size_t len)
{
    if (len == 0) return true;
#ifdef _WIN32
    NTSTATUS status = BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(status);
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, buf + total, len - total);
        if (n <= 0) { close(fd); return false; }
        total += static_cast<size_t>(n);
    }
    close(fd);
    return true;
#endif
}

// crypto.getRandomValues(typedArray)
// Safety: generate into a stack/heap buffer first, then copy into JS memory.
// Never let BCryptGenRandom write directly into QuickJS-managed memory.
static JSValue js_crypto_getRandomValues(JSContext* ctx, JSValueConst,
                                          int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.getRandomValues: expected TypedArray");

    size_t byte_offset = 0;
    size_t byte_len = 0;
    size_t bytes_per_element = 0;

    JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_offset, &byte_len, &bytes_per_element);
    if (JS_IsException(buf)) return buf;
    if (byte_len > 65536) {
        JS_FreeValue(ctx, buf);
        return JS_ThrowRangeError(ctx, "crypto.getRandomValues: quota exceeded (max 65536 bytes)");
    }

    // Generate random bytes into a safe stack buffer
    std::vector<uint8_t> random_buf(byte_len);
    if (byte_len > 0 && !fillRandom(random_buf.data(), byte_len)) {
        JS_FreeValue(ctx, buf);
        return JS_ThrowInternalError(ctx, "crypto.getRandomValues: OS RNG failed");
    }

    // Copy into the ArrayBuffer backing the typed array
    if (byte_len > 0) {
        size_t ab_len = 0;
        uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_len, buf);
        if (ab_ptr) {
            memcpy(ab_ptr + byte_offset, random_buf.data(), byte_len);
        }
    }
    JS_FreeValue(ctx, buf);
    return JS_DupValue(ctx, argv[0]);
}

// crypto.randomUUID() — returns a v4 UUID string
static JSValue js_crypto_randomUUID(JSContext* ctx, JSValueConst,
                                     int, JSValueConst*)
{
    uint8_t bytes[16];
    if (!fillRandom(bytes, 16)) {
        return JS_ThrowInternalError(ctx, "crypto.randomUUID: OS RNG failed");
    }

    // Set version (4) and variant (10xx)
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    char uuid[37];
    snprintf(uuid, sizeof(uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5],
             bytes[6], bytes[7],
             bytes[8], bytes[9],
             bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

    return JS_NewString(ctx, uuid);
}

void installCrypto(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue crypto = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, crypto, "getRandomValues",
                      JS_NewCFunction(ctx, js_crypto_getRandomValues, "getRandomValues", 1));
    JS_SetPropertyStr(ctx, crypto, "randomUUID",
                      JS_NewCFunction(ctx, js_crypto_randomUUID, "randomUUID", 0));

    JS_SetPropertyStr(ctx, global, "crypto", crypto);
    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
