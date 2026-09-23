#include "api/api.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"

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

static bronze::Value getRandomValues(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("crypto.getRandomValues: expected TypedArray");
    // The "_u8" read may allocate, so never hold a[0] in a local copy across it.
    bronze::Value arg = a[0];
    if (!ev::typedArrayInfo(a[0]) && ev::isObject(a[0])) {
        bronze::Value u8 = ev::getProperty(a[0], "_u8");
        arg = ev::isObject(u8) ? u8 : a[0];
    }
    auto info = ev::typedArrayInfo(arg);
    if (!info) return ev::throwTypeError("crypto.getRandomValues: expected TypedArray");
    // WebCrypto: only integer arrays (TypeMismatchError), at most 65536 bytes
    // (QuotaExceededError).
    if (info.elementKind == elements::Float32 || info.elementKind == elements::Float64) {
        return throwDOMException("crypto.getRandomValues: the array must be an integer TypedArray",
                                 "TypeMismatchError");
    }
    if (info.byteLength > 65536) {
        return throwDOMException("crypto.getRandomValues: quota exceeded (max 65536 bytes)",
                                 "QuotaExceededError");
    }

    if (info.byteLength > 0 && !fillRandom(info.data, info.byteLength)) {
        return ev::throwTypeError("crypto.getRandomValues: OS RNG failed");
    }

    return a[0];
}

static bronze::Value randomUUID(bronze::Value, std::span<const bronze::Value>)
{
    uint8_t bytes[16];
    if (!fillRandom(bytes, 16)) {
        return ev::throwTypeError("crypto.randomUUID: OS RNG failed");
    }

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

    return ev::fromUtf8(uuid);
}

void installCrypto()
{
    bronze::Value existing = ev::getGlobal("crypto");
    ObjectBuilder b(ev::isObject(existing) ? existing : ev::createObject());
    b.def("getRandomValues", 1, getRandomValues);
    b.def("randomUUID", 0, randomUUID);
    ev::setGlobalValue("crypto", b.get());
}

} // namespace brokit::api
