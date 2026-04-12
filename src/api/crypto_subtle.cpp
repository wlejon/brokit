#include "api/api.h"

#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

extern "C" {
#include "quickjs.h"
}

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace brokit::api {

// ---------------------------------------------------------------------------
// CryptoKey opaque class
// ---------------------------------------------------------------------------

struct CryptoKeyData {
    std::vector<uint8_t> rawKey;
    std::string algorithm; // "HMAC", "AES-GCM", "AES-CBC"
    std::string hash;      // "SHA-256", "SHA-384", "SHA-512"
    bool extractable = false;
    // Bitmask: 1=sign, 2=verify, 4=encrypt, 8=decrypt
    uint32_t usages = 0;
};

static JSClassID cryptokey_class_id = 0;

static void cryptokey_finalizer(JSRuntime*, JSValue val) {
    auto* key = static_cast<CryptoKeyData*>(JS_GetOpaque(val, cryptokey_class_id));
    delete key;
}

static JSClassDef cryptokey_class = {
    "CryptoKey",
    cryptokey_finalizer,
};

static uint32_t parseUsages(JSContext* ctx, JSValueConst arr) {
    uint32_t mask = 0;
    if (!JS_IsArray(arr)) return mask;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    for (int32_t i = 0; i < len; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, arr, static_cast<uint32_t>(i));
        const char* s = JS_ToCString(ctx, item);
        if (s) {
            if (strcmp(s, "sign") == 0) mask |= 1;
            else if (strcmp(s, "verify") == 0) mask |= 2;
            else if (strcmp(s, "encrypt") == 0) mask |= 4;
            else if (strcmp(s, "decrypt") == 0) mask |= 8;
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, item);
    }
    return mask;
}

static JSValue makeCryptoKeyJS(JSContext* ctx, CryptoKeyData* key) {
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(cryptokey_class_id));
    JS_SetOpaque(obj, key);
    JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, "secret"));
    JS_SetPropertyStr(ctx, obj, "extractable", JS_NewBool(ctx, key->extractable));
    JS_SetPropertyStr(ctx, obj, "algorithm", JS_NewString(ctx, key->algorithm.c_str()));
    return obj;
}

// ---------------------------------------------------------------------------
// Helper: resolve algorithm + hash from the algorithm parameter
// ---------------------------------------------------------------------------

struct AlgorithmInfo {
    std::string name;
    std::string hash;
};

static bool parseAlgorithm(JSContext* ctx, JSValueConst algo, AlgorithmInfo& out) {
    if (JS_IsString(algo)) {
        const char* s = JS_ToCString(ctx, algo);
        if (!s) return false;
        out.name = s;
        JS_FreeCString(ctx, s);
        return true;
    }
    // Object with name and hash
    JSValue nameVal = JS_GetPropertyStr(ctx, algo, "name");
    const char* n = JS_ToCString(ctx, nameVal);
    if (n) { out.name = n; JS_FreeCString(ctx, n); }
    JS_FreeValue(ctx, nameVal);

    JSValue hashVal = JS_GetPropertyStr(ctx, algo, "hash");
    if (!JS_IsUndefined(hashVal)) {
        if (JS_IsString(hashVal)) {
            const char* h = JS_ToCString(ctx, hashVal);
            if (h) { out.hash = h; JS_FreeCString(ctx, h); }
        } else {
            // hash: { name: "SHA-256" }
            JSValue hn = JS_GetPropertyStr(ctx, hashVal, "name");
            const char* h = JS_ToCString(ctx, hn);
            if (h) { out.hash = h; JS_FreeCString(ctx, h); }
            JS_FreeValue(ctx, hn);
        }
    }
    JS_FreeValue(ctx, hashVal);
    return !out.name.empty();
}

// ---------------------------------------------------------------------------
// Helper: extract bytes from ArrayBuffer or TypedArray
// ---------------------------------------------------------------------------

static bool getBytes(JSContext* ctx, JSValueConst val, std::vector<uint8_t>& out) {
    // Try ArrayBuffer first
    size_t len = 0;
    uint8_t* ptr = JS_GetArrayBuffer(ctx, &len, val);
    if (ptr) {
        out.assign(ptr, ptr + len);
        return true;
    }
    // Try TypedArray
    size_t offset = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, val, &offset, &len, &bpe);
    if (!JS_IsException(buf)) {
        size_t ab_len = 0;
        uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_len, buf);
        if (ab_ptr && offset + len <= ab_len) {
            out.assign(ab_ptr + offset, ab_ptr + offset + len);
        }
        JS_FreeValue(ctx, buf);
        return !out.empty() || len == 0;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Helper: wrap result in resolved/rejected Promise
// ---------------------------------------------------------------------------

static JSValue resolvePromise(JSContext* ctx, JSValue result) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    JSValue ret = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &result);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    JS_FreeValue(ctx, result);
    return promise;
}

static JSValue rejectPromise(JSContext* ctx, const char* msg) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg));
    JSValue ret = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &err);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

// ---------------------------------------------------------------------------
// Platform: BCrypt hash/HMAC
// ---------------------------------------------------------------------------

#ifdef _WIN32

static LPCWSTR bcryptAlgId(const std::string& hash) {
    if (hash == "SHA-256") return BCRYPT_SHA256_ALGORITHM;
    if (hash == "SHA-384") return BCRYPT_SHA384_ALGORITHM;
    if (hash == "SHA-512") return BCRYPT_SHA512_ALGORITHM;
    if (hash == "SHA-1")   return BCRYPT_SHA1_ALGORITHM;
    return nullptr;
}

static bool bcryptDigest(const std::string& algorithm,
                         const uint8_t* data, size_t dataLen,
                         std::vector<uint8_t>& out) {
    LPCWSTR algId = bcryptAlgId(algorithm);
    if (!algId) return false;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, algId, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) return false;

    DWORD hashLen = 0, dummy = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen),
                      sizeof(hashLen), &dummy, 0);

    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }

    BCryptHashData(hHash, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen), 0);

    out.resize(hashLen);
    BCryptFinishHash(hHash, out.data(), hashLen, 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return true;
}

static bool bcryptHMAC(const std::string& hashAlg,
                       const uint8_t* key, size_t keyLen,
                       const uint8_t* data, size_t dataLen,
                       std::vector<uint8_t>& out) {
    LPCWSTR algId = bcryptAlgId(hashAlg);
    if (!algId) return false;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, algId, nullptr,
                                                  BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status)) return false;

    DWORD hashLen = 0, dummy = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen),
                      sizeof(hashLen), &dummy, 0);

    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                              const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }

    BCryptHashData(hHash, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen), 0);

    out.resize(hashLen);
    BCryptFinishHash(hHash, out.data(), hashLen, 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return true;
}

static bool bcryptEncrypt(const std::string& /*mode*/,
                          const uint8_t* key, size_t keyLen,
                          const uint8_t* iv, size_t ivLen,
                          const uint8_t* data, size_t dataLen,
                          const uint8_t* aad, size_t aadLen,
                          std::vector<uint8_t>& out,
                          std::vector<uint8_t>& tag) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) return false;

    status = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                               reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                               sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }

    BCRYPT_KEY_HANDLE hKey = nullptr;
    status = BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                        const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }

    tag.resize(16); // 128-bit tag
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(iv);
    authInfo.cbNonce = static_cast<ULONG>(ivLen);
    authInfo.pbTag = tag.data();
    authInfo.cbTag = static_cast<ULONG>(tag.size());
    if (aad && aadLen > 0) {
        authInfo.pbAuthData = const_cast<PUCHAR>(aad);
        authInfo.cbAuthData = static_cast<ULONG>(aadLen);
    }

    ULONG outLen = 0;
    // Get required output size
    BCryptEncrypt(hKey, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen),
                  &authInfo, nullptr, 0, nullptr, 0, &outLen, 0);
    out.resize(outLen);
    status = BCryptEncrypt(hKey, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen),
                           &authInfo, nullptr, 0, out.data(), outLen, &outLen, 0);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return BCRYPT_SUCCESS(status);
}

static bool bcryptDecrypt(const std::string& /*mode*/,
                          const uint8_t* key, size_t keyLen,
                          const uint8_t* iv, size_t ivLen,
                          const uint8_t* data, size_t dataLen,
                          const uint8_t* aad, size_t aadLen,
                          const uint8_t* tagIn, size_t tagLen,
                          std::vector<uint8_t>& out) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) return false;

    status = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                               reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                               sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }

    BCRYPT_KEY_HANDLE hKey = nullptr;
    status = BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                        const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(iv);
    authInfo.cbNonce = static_cast<ULONG>(ivLen);
    authInfo.pbTag = const_cast<PUCHAR>(tagIn);
    authInfo.cbTag = static_cast<ULONG>(tagLen);
    if (aad && aadLen > 0) {
        authInfo.pbAuthData = const_cast<PUCHAR>(aad);
        authInfo.cbAuthData = static_cast<ULONG>(aadLen);
    }

    ULONG outLen = 0;
    BCryptDecrypt(hKey, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen),
                  &authInfo, nullptr, 0, nullptr, 0, &outLen, 0);
    out.resize(outLen);
    status = BCryptDecrypt(hKey, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen),
                           &authInfo, nullptr, 0, out.data(), outLen, &outLen, 0);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return BCRYPT_SUCCESS(status);
}

static bool fillRandom(uint8_t* buf, size_t len) {
    if (len == 0) return true;
    NTSTATUS status = BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(status);
}

#else
// Linux stubs — use OpenSSL or similar in the future
static bool bcryptDigest(const std::string&, const uint8_t*, size_t,
                         std::vector<uint8_t>&) { return false; }
static bool bcryptHMAC(const std::string&, const uint8_t*, size_t,
                       const uint8_t*, size_t, std::vector<uint8_t>&) { return false; }
static bool bcryptEncrypt(const std::string&, const uint8_t*, size_t,
                          const uint8_t*, size_t, const uint8_t*, size_t,
                          const uint8_t*, size_t, std::vector<uint8_t>&,
                          std::vector<uint8_t>&) { return false; }
static bool bcryptDecrypt(const std::string&, const uint8_t*, size_t,
                          const uint8_t*, size_t, const uint8_t*, size_t,
                          const uint8_t*, size_t, const uint8_t*, size_t,
                          std::vector<uint8_t>&) { return false; }
static bool fillRandom(uint8_t* buf, size_t len) {
    if (len == 0) return true;
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
}
#endif

// ---------------------------------------------------------------------------
// subtle.digest(algorithm, data)
// ---------------------------------------------------------------------------

static JSValue js_subtle_digest(JSContext* ctx, JSValueConst,
                                int argc, JSValueConst* argv) {
    if (argc < 2) return rejectPromise(ctx, "digest requires algorithm and data");

    AlgorithmInfo algo;
    if (!parseAlgorithm(ctx, argv[0], algo))
        return rejectPromise(ctx, "invalid algorithm");

    // digest uses the algorithm name directly as the hash
    std::string hashName = algo.name.empty() ? algo.hash : algo.name;
    if (hashName.empty() && !algo.hash.empty()) hashName = algo.hash;

    std::vector<uint8_t> data;
    if (!getBytes(ctx, argv[1], data))
        return rejectPromise(ctx, "digest: data must be ArrayBuffer or TypedArray");

    std::vector<uint8_t> result;
    if (!bcryptDigest(hashName, data.data(), data.size(), result))
        return rejectPromise(ctx, "digest: unsupported algorithm or OS error");

    JSValue ab = JS_NewArrayBufferCopy(ctx, result.data(), result.size());
    return resolvePromise(ctx, ab);
}

// ---------------------------------------------------------------------------
// subtle.importKey(format, keyData, algorithm, extractable, keyUsages)
// ---------------------------------------------------------------------------

static JSValue js_subtle_importKey(JSContext* ctx, JSValueConst,
                                   int argc, JSValueConst* argv) {
    if (argc < 5) return rejectPromise(ctx, "importKey requires 5 arguments");

    const char* fmt = JS_ToCString(ctx, argv[0]);
    if (!fmt) return rejectPromise(ctx, "importKey: invalid format");
    std::string format = fmt;
    JS_FreeCString(ctx, fmt);

    if (format != "raw" && format != "jwk")
        return rejectPromise(ctx, "importKey: only 'raw' and 'jwk' formats supported");

    AlgorithmInfo algo;
    if (!parseAlgorithm(ctx, argv[2], algo))
        return rejectPromise(ctx, "importKey: invalid algorithm");

    bool extractable = JS_ToBool(ctx, argv[3]);
    uint32_t usages = parseUsages(ctx, argv[4]);

    std::vector<uint8_t> rawKey;
    if (format == "raw") {
        if (!getBytes(ctx, argv[1], rawKey))
            return rejectPromise(ctx, "importKey: keyData must be ArrayBuffer/TypedArray");
    } else {
        // JWK: extract "k" field (base64url-encoded key)
        JSValue kVal = JS_GetPropertyStr(ctx, argv[1], "k");
        const char* kStr = JS_ToCString(ctx, kVal);
        if (!kStr) {
            JS_FreeValue(ctx, kVal);
            return rejectPromise(ctx, "importKey: JWK missing 'k' field");
        }
        // Base64url decode
        std::string b64 = kStr;
        JS_FreeCString(ctx, kStr);
        JS_FreeValue(ctx, kVal);
        // Replace base64url chars
        for (auto& c : b64) {
            if (c == '-') c = '+';
            else if (c == '_') c = '/';
        }
        // Pad
        while (b64.size() % 4 != 0) b64 += '=';
        // Use atob via JS
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue atobFn = JS_GetPropertyStr(ctx, global, "atob");
        JSValue b64Val = JS_NewString(ctx, b64.c_str());
        JSValue decoded = JS_Call(ctx, atobFn, JS_UNDEFINED, 1, &b64Val);
        JS_FreeValue(ctx, b64Val);
        JS_FreeValue(ctx, atobFn);
        JS_FreeValue(ctx, global);

        if (JS_IsException(decoded))
            return rejectPromise(ctx, "importKey: JWK base64url decode failed");

        const char* decStr = JS_ToCString(ctx, decoded);
        if (decStr) {
            size_t len = strlen(decStr);
            rawKey.resize(len);
            for (size_t i = 0; i < len; i++)
                rawKey[i] = static_cast<uint8_t>(decStr[i]);
            JS_FreeCString(ctx, decStr);
        }
        JS_FreeValue(ctx, decoded);
    }

    auto* key = new CryptoKeyData{std::move(rawKey), algo.name, algo.hash,
                                   extractable, usages};
    JSValue keyObj = makeCryptoKeyJS(ctx, key);
    return resolvePromise(ctx, keyObj);
}

// ---------------------------------------------------------------------------
// subtle.generateKey(algorithm, extractable, keyUsages)
// ---------------------------------------------------------------------------

static JSValue js_subtle_generateKey(JSContext* ctx, JSValueConst,
                                     int argc, JSValueConst* argv) {
    if (argc < 3) return rejectPromise(ctx, "generateKey requires 3 arguments");

    AlgorithmInfo algo;
    if (!parseAlgorithm(ctx, argv[0], algo))
        return rejectPromise(ctx, "generateKey: invalid algorithm");

    bool extractable = JS_ToBool(ctx, argv[1]);
    uint32_t usages = parseUsages(ctx, argv[2]);

    // Determine key length
    size_t keyLen = 0;
    if (algo.name == "HMAC") {
        // Default to hash output length
        if (algo.hash == "SHA-256") keyLen = 32;
        else if (algo.hash == "SHA-384") keyLen = 48;
        else if (algo.hash == "SHA-512") keyLen = 64;
        else if (algo.hash == "SHA-1") keyLen = 20;
        else return rejectPromise(ctx, "generateKey: unsupported HMAC hash");

        // Check for explicit length override
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[0], "length");
        if (!JS_IsUndefined(lenVal)) {
            int32_t bits = 0;
            JS_ToInt32(ctx, &bits, lenVal);
            if (bits > 0) keyLen = static_cast<size_t>((bits + 7) / 8);
        }
        JS_FreeValue(ctx, lenVal);
    } else if (algo.name == "AES-GCM" || algo.name == "AES-CBC") {
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[0], "length");
        int32_t bits = 256;
        if (!JS_IsUndefined(lenVal)) JS_ToInt32(ctx, &bits, lenVal);
        JS_FreeValue(ctx, lenVal);
        if (bits != 128 && bits != 192 && bits != 256)
            return rejectPromise(ctx, "generateKey: AES key length must be 128, 192, or 256");
        keyLen = static_cast<size_t>(bits / 8);
    } else {
        return rejectPromise(ctx, "generateKey: unsupported algorithm");
    }

    std::vector<uint8_t> rawKey(keyLen);
    if (!fillRandom(rawKey.data(), keyLen))
        return rejectPromise(ctx, "generateKey: random generation failed");

    auto* key = new CryptoKeyData{std::move(rawKey), algo.name, algo.hash,
                                   extractable, usages};
    JSValue keyObj = makeCryptoKeyJS(ctx, key);
    return resolvePromise(ctx, keyObj);
}

// ---------------------------------------------------------------------------
// subtle.sign(algorithm, key, data)
// ---------------------------------------------------------------------------

static JSValue js_subtle_sign(JSContext* ctx, JSValueConst,
                              int argc, JSValueConst* argv) {
    if (argc < 3) return rejectPromise(ctx, "sign requires algorithm, key, data");

    AlgorithmInfo algo;
    if (!parseAlgorithm(ctx, argv[0], algo))
        return rejectPromise(ctx, "sign: invalid algorithm");

    auto* key = static_cast<CryptoKeyData*>(JS_GetOpaque(argv[1], cryptokey_class_id));
    if (!key) return rejectPromise(ctx, "sign: invalid CryptoKey");
    if (!(key->usages & 1))
        return rejectPromise(ctx, "sign: key does not have 'sign' usage");

    std::vector<uint8_t> data;
    if (!getBytes(ctx, argv[2], data))
        return rejectPromise(ctx, "sign: data must be ArrayBuffer or TypedArray");

    std::string hashAlg = algo.hash.empty() ? key->hash : algo.hash;

    if (algo.name == "HMAC" || key->algorithm == "HMAC") {
        std::vector<uint8_t> sig;
        if (!bcryptHMAC(hashAlg, key->rawKey.data(), key->rawKey.size(),
                        data.data(), data.size(), sig))
            return rejectPromise(ctx, "sign: HMAC failed");
        JSValue ab = JS_NewArrayBufferCopy(ctx, sig.data(), sig.size());
        return resolvePromise(ctx, ab);
    }

    return rejectPromise(ctx, "sign: unsupported algorithm");
}

// ---------------------------------------------------------------------------
// subtle.verify(algorithm, key, signature, data)
// ---------------------------------------------------------------------------

static JSValue js_subtle_verify(JSContext* ctx, JSValueConst,
                                int argc, JSValueConst* argv) {
    if (argc < 4) return rejectPromise(ctx, "verify requires algorithm, key, signature, data");

    AlgorithmInfo algo;
    if (!parseAlgorithm(ctx, argv[0], algo))
        return rejectPromise(ctx, "verify: invalid algorithm");

    auto* key = static_cast<CryptoKeyData*>(JS_GetOpaque(argv[1], cryptokey_class_id));
    if (!key) return rejectPromise(ctx, "verify: invalid CryptoKey");
    if (!(key->usages & 2))
        return rejectPromise(ctx, "verify: key does not have 'verify' usage");

    std::vector<uint8_t> signature, data;
    if (!getBytes(ctx, argv[2], signature))
        return rejectPromise(ctx, "verify: signature must be ArrayBuffer or TypedArray");
    if (!getBytes(ctx, argv[3], data))
        return rejectPromise(ctx, "verify: data must be ArrayBuffer or TypedArray");

    std::string hashAlg = algo.hash.empty() ? key->hash : algo.hash;

    if (algo.name == "HMAC" || key->algorithm == "HMAC") {
        std::vector<uint8_t> expected;
        if (!bcryptHMAC(hashAlg, key->rawKey.data(), key->rawKey.size(),
                        data.data(), data.size(), expected))
            return rejectPromise(ctx, "verify: HMAC failed");

        bool match = (signature.size() == expected.size()) &&
                     (memcmp(signature.data(), expected.data(), expected.size()) == 0);
        return resolvePromise(ctx, JS_NewBool(ctx, match));
    }

    return rejectPromise(ctx, "verify: unsupported algorithm");
}

// ---------------------------------------------------------------------------
// subtle.encrypt(algorithm, key, data)
// ---------------------------------------------------------------------------

static JSValue js_subtle_encrypt(JSContext* ctx, JSValueConst,
                                 int argc, JSValueConst* argv) {
    if (argc < 3) return rejectPromise(ctx, "encrypt requires algorithm, key, data");

    AlgorithmInfo algo;
    if (!parseAlgorithm(ctx, argv[0], algo))
        return rejectPromise(ctx, "encrypt: invalid algorithm");

    auto* key = static_cast<CryptoKeyData*>(JS_GetOpaque(argv[1], cryptokey_class_id));
    if (!key) return rejectPromise(ctx, "encrypt: invalid CryptoKey");
    if (!(key->usages & 4))
        return rejectPromise(ctx, "encrypt: key does not have 'encrypt' usage");

    std::vector<uint8_t> data;
    if (!getBytes(ctx, argv[2], data))
        return rejectPromise(ctx, "encrypt: data must be ArrayBuffer or TypedArray");

    if (algo.name == "AES-GCM") {
        // Extract IV
        JSValue ivVal = JS_GetPropertyStr(ctx, argv[0], "iv");
        std::vector<uint8_t> iv;
        if (!getBytes(ctx, ivVal, iv)) {
            JS_FreeValue(ctx, ivVal);
            return rejectPromise(ctx, "encrypt: AES-GCM requires 'iv'");
        }
        JS_FreeValue(ctx, ivVal);

        // Optional additional data
        std::vector<uint8_t> aad;
        JSValue aadVal = JS_GetPropertyStr(ctx, argv[0], "additionalData");
        if (!JS_IsUndefined(aadVal)) getBytes(ctx, aadVal, aad);
        JS_FreeValue(ctx, aadVal);

        std::vector<uint8_t> ciphertext, tag;
        if (!bcryptEncrypt("AES-GCM", key->rawKey.data(), key->rawKey.size(),
                           iv.data(), iv.size(), data.data(), data.size(),
                           aad.empty() ? nullptr : aad.data(), aad.size(),
                           ciphertext, tag))
            return rejectPromise(ctx, "encrypt: AES-GCM failed");

        // Concatenate ciphertext + tag (Web Crypto convention)
        std::vector<uint8_t> result;
        result.reserve(ciphertext.size() + tag.size());
        result.insert(result.end(), ciphertext.begin(), ciphertext.end());
        result.insert(result.end(), tag.begin(), tag.end());

        JSValue ab = JS_NewArrayBufferCopy(ctx, result.data(), result.size());
        return resolvePromise(ctx, ab);
    }

    return rejectPromise(ctx, "encrypt: unsupported algorithm");
}

// ---------------------------------------------------------------------------
// subtle.decrypt(algorithm, key, data)
// ---------------------------------------------------------------------------

static JSValue js_subtle_decrypt(JSContext* ctx, JSValueConst,
                                 int argc, JSValueConst* argv) {
    if (argc < 3) return rejectPromise(ctx, "decrypt requires algorithm, key, data");

    AlgorithmInfo algo;
    if (!parseAlgorithm(ctx, argv[0], algo))
        return rejectPromise(ctx, "decrypt: invalid algorithm");

    auto* key = static_cast<CryptoKeyData*>(JS_GetOpaque(argv[1], cryptokey_class_id));
    if (!key) return rejectPromise(ctx, "decrypt: invalid CryptoKey");
    if (!(key->usages & 8))
        return rejectPromise(ctx, "decrypt: key does not have 'decrypt' usage");

    std::vector<uint8_t> ciphertextAndTag;
    if (!getBytes(ctx, argv[2], ciphertextAndTag))
        return rejectPromise(ctx, "decrypt: data must be ArrayBuffer or TypedArray");

    if (algo.name == "AES-GCM") {
        JSValue ivVal = JS_GetPropertyStr(ctx, argv[0], "iv");
        std::vector<uint8_t> iv;
        if (!getBytes(ctx, ivVal, iv)) {
            JS_FreeValue(ctx, ivVal);
            return rejectPromise(ctx, "decrypt: AES-GCM requires 'iv'");
        }
        JS_FreeValue(ctx, ivVal);

        // Optional tag length (default 128 bits = 16 bytes)
        int tagLen = 16;
        JSValue tlVal = JS_GetPropertyStr(ctx, argv[0], "tagLength");
        if (!JS_IsUndefined(tlVal)) {
            int32_t tl = 0;
            JS_ToInt32(ctx, &tl, tlVal);
            tagLen = tl / 8;
        }
        JS_FreeValue(ctx, tlVal);

        if (static_cast<int>(ciphertextAndTag.size()) < tagLen)
            return rejectPromise(ctx, "decrypt: ciphertext too short for tag");

        size_t ctLen = ciphertextAndTag.size() - static_cast<size_t>(tagLen);
        const uint8_t* ctData = ciphertextAndTag.data();
        const uint8_t* tagData = ciphertextAndTag.data() + ctLen;

        std::vector<uint8_t> aad;
        JSValue aadVal = JS_GetPropertyStr(ctx, argv[0], "additionalData");
        if (!JS_IsUndefined(aadVal)) getBytes(ctx, aadVal, aad);
        JS_FreeValue(ctx, aadVal);

        std::vector<uint8_t> plaintext;
        if (!bcryptDecrypt("AES-GCM", key->rawKey.data(), key->rawKey.size(),
                           iv.data(), iv.size(), ctData, ctLen,
                           aad.empty() ? nullptr : aad.data(), aad.size(),
                           tagData, static_cast<size_t>(tagLen), plaintext))
            return rejectPromise(ctx, "decrypt: AES-GCM failed (bad key or tampered data)");

        JSValue ab = JS_NewArrayBufferCopy(ctx, plaintext.data(), plaintext.size());
        return resolvePromise(ctx, ab);
    }

    return rejectPromise(ctx, "decrypt: unsupported algorithm");
}

// ---------------------------------------------------------------------------
// subtle.exportKey(format, key)
// ---------------------------------------------------------------------------

static JSValue js_subtle_exportKey(JSContext* ctx, JSValueConst,
                                   int argc, JSValueConst* argv) {
    if (argc < 2) return rejectPromise(ctx, "exportKey requires format and key");

    const char* fmt = JS_ToCString(ctx, argv[0]);
    if (!fmt) return rejectPromise(ctx, "exportKey: invalid format");
    std::string format = fmt;
    JS_FreeCString(ctx, fmt);

    auto* key = static_cast<CryptoKeyData*>(JS_GetOpaque(argv[1], cryptokey_class_id));
    if (!key) return rejectPromise(ctx, "exportKey: invalid CryptoKey");
    if (!key->extractable)
        return rejectPromise(ctx, "exportKey: key is not extractable");

    if (format == "raw") {
        JSValue ab = JS_NewArrayBufferCopy(ctx, key->rawKey.data(), key->rawKey.size());
        return resolvePromise(ctx, ab);
    }

    return rejectPromise(ctx, "exportKey: only 'raw' format supported");
}

// ---------------------------------------------------------------------------
// installSubtleCrypto
// ---------------------------------------------------------------------------

void installSubtleCrypto(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);

    // Register CryptoKey class
    if (cryptokey_class_id == 0) {
        JS_NewClassID(rt, &cryptokey_class_id);
        JS_NewClass(rt, cryptokey_class_id, &cryptokey_class);
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue crypto = JS_GetPropertyStr(ctx, global, "crypto");
    if (JS_IsUndefined(crypto)) {
        crypto = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "crypto", JS_DupValue(ctx, crypto));
    }

    JSValue subtle = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, subtle, "digest",
        JS_NewCFunction(ctx, js_subtle_digest, "digest", 2));
    JS_SetPropertyStr(ctx, subtle, "importKey",
        JS_NewCFunction(ctx, js_subtle_importKey, "importKey", 5));
    JS_SetPropertyStr(ctx, subtle, "generateKey",
        JS_NewCFunction(ctx, js_subtle_generateKey, "generateKey", 3));
    JS_SetPropertyStr(ctx, subtle, "sign",
        JS_NewCFunction(ctx, js_subtle_sign, "sign", 3));
    JS_SetPropertyStr(ctx, subtle, "verify",
        JS_NewCFunction(ctx, js_subtle_verify, "verify", 4));
    JS_SetPropertyStr(ctx, subtle, "encrypt",
        JS_NewCFunction(ctx, js_subtle_encrypt, "encrypt", 3));
    JS_SetPropertyStr(ctx, subtle, "decrypt",
        JS_NewCFunction(ctx, js_subtle_decrypt, "decrypt", 3));
    JS_SetPropertyStr(ctx, subtle, "exportKey",
        JS_NewCFunction(ctx, js_subtle_exportKey, "exportKey", 2));

    JS_SetPropertyStr(ctx, crypto, "subtle", subtle);
    JS_FreeValue(ctx, crypto);
    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
