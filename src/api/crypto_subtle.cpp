#include "api/api.h"
#include "api/arg_reader.h"
#include "api/host_class.h"
#include "api/object_builder.h"

#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
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

static HostClass g_cryptoKeyClass;

static void cryptoKeyDtor(void* p) {
    delete static_cast<CryptoKeyData*>(p);
}

static uint32_t parseUsages(bronze::Value arr) {
    uint32_t mask = 0;
    if (!ev::isObject(arr)) return mask;
    bronze::Value lenVal = ev::getProperty(arr, "length");
    if (!ev::isDouble(lenVal)) return mask;
    int len = static_cast<int>(ev::toDouble(lenVal));
    for (int i = 0; i < len; i++) {
        bronze::Value item = ev::getElement(arr, i);
        if (ev::isString(item)) {
            std::string s = ev::toUtf8(item);
            if (s == "sign") mask |= 1;
            else if (s == "verify") mask |= 2;
            else if (s == "encrypt") mask |= 4;
            else if (s == "decrypt") mask |= 8;
        }
    }
    return mask;
}

static bronze::Value makeCryptoKeyJS(CryptoKeyData* key) {
    bronze::Value obj = g_cryptoKeyClass.make(key, cryptoKeyDtor);
    ev::setProperty(obj, "type", ev::fromUtf8("secret"));
    ev::setProperty(obj, "extractable", ev::fromBool(key->extractable));
    ev::setProperty(obj, "algorithm", ev::fromUtf8(key->algorithm));
    return obj;
}

// ---------------------------------------------------------------------------
// Helper: resolve algorithm + hash from the algorithm parameter
// ---------------------------------------------------------------------------

struct AlgorithmInfo {
    std::string name;
    std::string hash;
};

static bool parseAlgorithm(bronze::Value algo, AlgorithmInfo& out) {
    if (ev::isString(algo)) {
        out.name = ev::toUtf8(algo);
        return true;
    }
    if (ev::isObject(algo)) {
        bronze::Value nameVal = ev::getProperty(algo, "name");
        if (ev::isString(nameVal)) out.name = ev::toUtf8(nameVal);

        bronze::Value hashVal = ev::getProperty(algo, "hash");
        if (!ev::isUndefined(hashVal)) {
            if (ev::isString(hashVal)) {
                out.hash = ev::toUtf8(hashVal);
            } else if (ev::isObject(hashVal)) {
                bronze::Value hn = ev::getProperty(hashVal, "name");
                if (ev::isString(hn)) out.hash = ev::toUtf8(hn);
            }
        }
        return !out.name.empty();
    }
    return false;
}

// ---------------------------------------------------------------------------
// Helper: extract bytes from ArrayBuffer or TypedArray
// ---------------------------------------------------------------------------

static bool getBytes(bronze::Value val, std::vector<uint8_t>& out) {
    if (auto info = ev::typedArrayInfo(val)) {
        out.assign(info.data, info.data + info.byteLength);
        return true;
    }
    if (auto info = ev::arrayBufferInfo(val)) {
        out.assign(info.data, info.data + info.byteLength);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Helper: wrap result in resolved/rejected Promise
// ---------------------------------------------------------------------------

static bronze::Value resolvePromise(bronze::Value result) {
    ev::Persistent p{ev::createPromise()};
    ev::resolvePromise(p.get(), result);
    return p.get();
}

static bronze::Value rejectPromise(const char* msg) {
    ev::Persistent p{ev::createPromise()};
    bronze::Value err = ev::throwTypeError(msg);
    ev::rejectPromise(p.get(), err);
    return p.get();
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

static bool bcryptEncrypt(const std::string&,
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
    BCryptEncrypt(hKey, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen),
                  &authInfo, nullptr, 0, nullptr, 0, &outLen, 0);
    out.resize(outLen);
    status = BCryptEncrypt(hKey, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen),
                           &authInfo, nullptr, 0, out.data(), outLen, &outLen, 0);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return BCRYPT_SUCCESS(status);
}

static bool bcryptDecrypt(const std::string&,
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

#else
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
#endif

// ---------------------------------------------------------------------------
// subtle.digest(algorithm, data)
// ---------------------------------------------------------------------------

static bronze::Value subtleDigest(bronze::Value, std::span<const bronze::Value> a) {
    if (a.size() < 2) return rejectPromise("digest requires algorithm and data");

    AlgorithmInfo algo;
    if (!parseAlgorithm(a[0], algo))
        return rejectPromise("invalid algorithm");

    std::string hashName = algo.name.empty() ? algo.hash : algo.name;
    if (hashName.empty() && !algo.hash.empty()) hashName = algo.hash;

    std::vector<uint8_t> data;
    if (!getBytes(a[1], data))
        return rejectPromise("digest: data must be ArrayBuffer or TypedArray");

    std::vector<uint8_t> result;
    if (!bcryptDigest(hashName, data.data(), data.size(), result))
        return rejectPromise("digest: unsupported algorithm or OS error");

    bronze::Value ab = ev::createArrayBuffer(std::span<const uint8_t>(result));
    return resolvePromise(ab);
}

// ---------------------------------------------------------------------------
// subtle.importKey(format, keyData, algorithm, extractable, keyUsages)
// ---------------------------------------------------------------------------

static bronze::Value subtleImportKey(bronze::Value, std::span<const bronze::Value> a) {
    if (a.size() < 5) return rejectPromise("importKey requires format, keyData, algorithm, extractable, keyUsages");

    if (!ev::isString(a[0])) return rejectPromise("importKey: format must be string");
    std::string format = ev::toUtf8(a[0]);

    std::vector<uint8_t> keyBytes;
    if (!getBytes(a[1], keyBytes))
        return rejectPromise("importKey: keyData must be ArrayBuffer or TypedArray");

    AlgorithmInfo algo;
    if (!parseAlgorithm(a[2], algo))
        return rejectPromise("importKey: invalid algorithm");

    bool extractable = ev::isBool(a[3]) && ev::toBool(a[3]);
    uint32_t usages = parseUsages(a[4]);

    if (format == "raw") {
        auto* key = new CryptoKeyData();
        key->rawKey = std::move(keyBytes);
        key->algorithm = algo.name;
        key->hash = algo.hash;
        key->extractable = extractable;
        key->usages = usages;
        return resolvePromise(makeCryptoKeyJS(key));
    }

    return rejectPromise("importKey: unsupported format (only 'raw' supported)");
}

// ---------------------------------------------------------------------------
// subtle.generateKey(algorithm, extractable, keyUsages)
// ---------------------------------------------------------------------------

static bronze::Value subtleGenerateKey(bronze::Value, std::span<const bronze::Value> a) {
    if (a.size() < 3) return rejectPromise("generateKey requires algorithm, extractable, keyUsages");

    AlgorithmInfo algo;
    if (!parseAlgorithm(a[0], algo))
        return rejectPromise("generateKey: invalid algorithm");

    bool extractable = ev::isBool(a[1]) && ev::toBool(a[1]);
    uint32_t usages = parseUsages(a[2]);

    int keyLenBytes = 0;
    if (algo.name == "HMAC") {
        if (!ev::isObject(a[0])) return rejectPromise("generateKey: HMAC requires length or hash");
        bronze::Value lenVal = ev::getProperty(a[0], "length");
        if (ev::isDouble(lenVal)) {
            keyLenBytes = static_cast<int>(ev::toDouble(lenVal)) / 8;
        } else {
            if (algo.hash == "SHA-256") keyLenBytes = 32;
            else if (algo.hash == "SHA-384") keyLenBytes = 48;
            else if (algo.hash == "SHA-512") keyLenBytes = 64;
            else if (algo.hash == "SHA-1")   keyLenBytes = 20;
            else return rejectPromise("generateKey: HMAC unknown hash length");
        }
    } else if (algo.name == "AES-GCM" || algo.name == "AES-CBC") {
        if (!ev::isObject(a[0])) return rejectPromise("generateKey: AES requires length");
        bronze::Value lenVal = ev::getProperty(a[0], "length");
        if (ev::isDouble(lenVal)) {
            int bits = static_cast<int>(ev::toDouble(lenVal));
            if (bits != 128 && bits != 192 && bits != 256)
                return rejectPromise("generateKey: AES length must be 128, 192, or 256");
            keyLenBytes = bits / 8;
        } else {
            keyLenBytes = 32; // default 256-bit
        }
    } else {
        return rejectPromise("generateKey: unsupported algorithm");
    }

    std::vector<uint8_t> randomBytes(keyLenBytes);
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return rejectPromise("generateKey: OS RNG failed");
    ssize_t n = read(fd, randomBytes.data(), keyLenBytes);
    close(fd);
    if (n < keyLenBytes) return rejectPromise("generateKey: OS RNG read failed");

    auto* key = new CryptoKeyData();
    key->rawKey = std::move(randomBytes);
    key->algorithm = algo.name;
    key->hash = algo.hash;
    key->extractable = extractable;
    key->usages = usages;
    return resolvePromise(makeCryptoKeyJS(key));
}

// ---------------------------------------------------------------------------
// subtle.sign(algorithm, key, data)
// ---------------------------------------------------------------------------

static bronze::Value subtleSign(bronze::Value, std::span<const bronze::Value> a) {
    if (a.size() < 3) return rejectPromise("sign requires algorithm, key, data");

    AlgorithmInfo algo;
    if (!parseAlgorithm(a[0], algo))
        return rejectPromise("sign: invalid algorithm");

    auto* key = static_cast<CryptoKeyData*>(g_cryptoKeyClass.unwrap(a[1]));
    if (!key) return rejectPromise("sign: invalid CryptoKey");
    if (!(key->usages & 1))
        return rejectPromise("sign: key does not have 'sign' usage");

    std::vector<uint8_t> data;
    if (!getBytes(a[2], data))
        return rejectPromise("sign: data must be ArrayBuffer or TypedArray");

    if (algo.name == "HMAC") {
        std::string hashAlg = algo.hash.empty() ? key->hash : algo.hash;
        std::vector<uint8_t> sig;
        if (!bcryptHMAC(hashAlg, key->rawKey.data(), key->rawKey.size(),
                        data.data(), data.size(), sig))
            return rejectPromise("sign: HMAC failed");

        bronze::Value ab = ev::createArrayBuffer(std::span<const uint8_t>(sig));
        return resolvePromise(ab);
    }

    return rejectPromise("sign: unsupported algorithm");
}

// ---------------------------------------------------------------------------
// subtle.verify(algorithm, key, signature, data)
// ---------------------------------------------------------------------------

static bronze::Value subtleVerify(bronze::Value, std::span<const bronze::Value> a) {
    if (a.size() < 4) return rejectPromise("verify requires algorithm, key, signature, data");

    AlgorithmInfo algo;
    if (!parseAlgorithm(a[0], algo))
        return rejectPromise("verify: invalid algorithm");

    auto* key = static_cast<CryptoKeyData*>(g_cryptoKeyClass.unwrap(a[1]));
    if (!key) return rejectPromise("verify: invalid CryptoKey");
    if (!(key->usages & 2))
        return rejectPromise("verify: key does not have 'verify' usage");

    std::vector<uint8_t> sig;
    if (!getBytes(a[2], sig))
        return rejectPromise("verify: signature must be ArrayBuffer or TypedArray");

    std::vector<uint8_t> data;
    if (!getBytes(a[3], data))
        return rejectPromise("verify: data must be ArrayBuffer or TypedArray");

    if (algo.name == "HMAC") {
        std::string hashAlg = algo.hash.empty() ? key->hash : algo.hash;
        std::vector<uint8_t> expectedSig;
        if (!bcryptHMAC(hashAlg, key->rawKey.data(), key->rawKey.size(),
                        data.data(), data.size(), expectedSig))
            return rejectPromise("verify: HMAC failed");

        bool match = (sig.size() == expectedSig.size());
        if (match) {
            uint8_t diff = 0;
            for (size_t i = 0; i < sig.size(); i++) diff |= (sig[i] ^ expectedSig[i]);
            match = (diff == 0);
        }
        return resolvePromise(ev::fromBool(match));
    }

    return rejectPromise("verify: unsupported algorithm");
}

// ---------------------------------------------------------------------------
// subtle.encrypt(algorithm, key, data)
// ---------------------------------------------------------------------------

static bronze::Value subtleEncrypt(bronze::Value, std::span<const bronze::Value> a) {
    if (a.size() < 3) return rejectPromise("encrypt requires algorithm, key, data");

    AlgorithmInfo algo;
    if (!parseAlgorithm(a[0], algo))
        return rejectPromise("encrypt: invalid algorithm");

    auto* key = static_cast<CryptoKeyData*>(g_cryptoKeyClass.unwrap(a[1]));
    if (!key) return rejectPromise("encrypt: invalid CryptoKey");
    if (!(key->usages & 4))
        return rejectPromise("encrypt: key does not have 'encrypt' usage");

    std::vector<uint8_t> data;
    if (!getBytes(a[2], data))
        return rejectPromise("encrypt: data must be ArrayBuffer or TypedArray");

    if (algo.name == "AES-GCM") {
        bronze::Value ivVal = ev::getProperty(a[0], "iv");
        std::vector<uint8_t> iv;
        if (!getBytes(ivVal, iv)) {
            return rejectPromise("encrypt: AES-GCM requires 'iv'");
        }

        std::vector<uint8_t> aad;
        bronze::Value aadVal = ev::getProperty(a[0], "additionalData");
        if (!ev::isUndefined(aadVal)) getBytes(aadVal, aad);

        std::vector<uint8_t> ciphertext, tag;
        if (!bcryptEncrypt("AES-GCM", key->rawKey.data(), key->rawKey.size(),
                           iv.data(), iv.size(), data.data(), data.size(),
                           aad.empty() ? nullptr : aad.data(), aad.size(),
                           ciphertext, tag))
            return rejectPromise("encrypt: AES-GCM failed");

        std::vector<uint8_t> result;
        result.reserve(ciphertext.size() + tag.size());
        result.insert(result.end(), ciphertext.begin(), ciphertext.end());
        result.insert(result.end(), tag.begin(), tag.end());

        bronze::Value ab = ev::createArrayBuffer(std::span<const uint8_t>(result));
        return resolvePromise(ab);
    }

    return rejectPromise("encrypt: unsupported algorithm");
}

// ---------------------------------------------------------------------------
// subtle.decrypt(algorithm, key, data)
// ---------------------------------------------------------------------------

static bronze::Value subtleDecrypt(bronze::Value, std::span<const bronze::Value> a) {
    if (a.size() < 3) return rejectPromise("decrypt requires algorithm, key, data");

    AlgorithmInfo algo;
    if (!parseAlgorithm(a[0], algo))
        return rejectPromise("decrypt: invalid algorithm");

    auto* key = static_cast<CryptoKeyData*>(g_cryptoKeyClass.unwrap(a[1]));
    if (!key) return rejectPromise("decrypt: invalid CryptoKey");
    if (!(key->usages & 8))
        return rejectPromise("decrypt: key does not have 'decrypt' usage");

    std::vector<uint8_t> ciphertextAndTag;
    if (!getBytes(a[2], ciphertextAndTag))
        return rejectPromise("decrypt: data must be ArrayBuffer or TypedArray");

    if (algo.name == "AES-GCM") {
        bronze::Value ivVal = ev::getProperty(a[0], "iv");
        std::vector<uint8_t> iv;
        if (!getBytes(ivVal, iv)) {
            return rejectPromise("decrypt: AES-GCM requires 'iv'");
        }

        int tagLen = 16;
        bronze::Value tlVal = ev::getProperty(a[0], "tagLength");
        if (ev::isDouble(tlVal)) {
            tagLen = static_cast<int>(ev::toDouble(tlVal)) / 8;
        }

        if (static_cast<int>(ciphertextAndTag.size()) < tagLen)
            return rejectPromise("decrypt: ciphertext too short for tag");

        size_t ctLen = ciphertextAndTag.size() - static_cast<size_t>(tagLen);
        const uint8_t* ctData = ciphertextAndTag.data();
        const uint8_t* tagData = ciphertextAndTag.data() + ctLen;

        std::vector<uint8_t> aad;
        bronze::Value aadVal = ev::getProperty(a[0], "additionalData");
        if (!ev::isUndefined(aadVal)) getBytes(aadVal, aad);

        std::vector<uint8_t> plaintext;
        if (!bcryptDecrypt("AES-GCM", key->rawKey.data(), key->rawKey.size(),
                           iv.data(), iv.size(), ctData, ctLen,
                           aad.empty() ? nullptr : aad.data(), aad.size(),
                           tagData, static_cast<size_t>(tagLen), plaintext))
            return rejectPromise("decrypt: AES-GCM failed (bad key or tampered data)");

        bronze::Value ab = ev::createArrayBuffer(std::span<const uint8_t>(plaintext));
        return resolvePromise(ab);
    }

    return rejectPromise("decrypt: unsupported algorithm");
}

// ---------------------------------------------------------------------------
// subtle.exportKey(format, key)
// ---------------------------------------------------------------------------

static bronze::Value subtleExportKey(bronze::Value, std::span<const bronze::Value> a) {
    if (a.size() < 2) return rejectPromise("exportKey requires format and key");

    if (!ev::isString(a[0])) return rejectPromise("exportKey: invalid format");
    std::string format = ev::toUtf8(a[0]);

    auto* key = static_cast<CryptoKeyData*>(g_cryptoKeyClass.unwrap(a[1]));
    if (!key) return rejectPromise("exportKey: invalid CryptoKey");
    if (!key->extractable)
        return rejectPromise("exportKey: key is not extractable");

    if (format == "raw") {
        bronze::Value ab = ev::createArrayBuffer(std::span<const uint8_t>(key->rawKey));
        return resolvePromise(ab);
    }

    return rejectPromise("exportKey: only 'raw' format supported");
}

void installSubtleCrypto() {
    bronze::Value crypto = ev::getGlobal("crypto");
    if (!ev::isObject(crypto)) {
        crypto = ev::createObject();
        ev::setGlobalValue("crypto", crypto);
    }

    ObjectBuilder subtle;
    subtle.def("digest", 2, subtleDigest);
    subtle.def("importKey", 5, subtleImportKey);
    subtle.def("generateKey", 3, subtleGenerateKey);
    subtle.def("sign", 3, subtleSign);
    subtle.def("verify", 4, subtleVerify);
    subtle.def("encrypt", 3, subtleEncrypt);
    subtle.def("decrypt", 3, subtleDecrypt);
    subtle.def("exportKey", 2, subtleExportKey);

    ev::setProperty(crypto, "subtle", subtle.get());
}

} // namespace brokit::api
