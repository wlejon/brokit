// crypto.subtle — the WebCrypto SubtleCrypto surface over the platform crypto
// backend (BCrypt on Windows, OpenSSL's libcrypto elsewhere).
//
// Supported:
//   digest       SHA-1, SHA-256, SHA-384, SHA-512
//   sign/verify  HMAC
//   encrypt/decrypt, wrapKey/unwrapKey
//                AES-GCM, AES-CBC, AES-CTR, AES-KW (wrap/unwrap only)
//   deriveBits/deriveKey
//                PBKDF2, HKDF
//   importKey    raw (all), jwk (HMAC, AES-*)
//   exportKey    raw, jwk (HMAC, AES-*)
//   generateKey  HMAC, AES-*
// Asymmetric algorithms (RSA, ECDSA, ECDH, Ed25519, X25519) are not supported
// and reject with NotSupportedError.
//
// GC: every Value this file holds across an allocating embed call lives in an
// ev::Persistent (embed.h's contract). Algorithm parameters are read straight
// off the rooted `args` span and copied into C++ storage before anything
// allocates.

#include "api/api.h"
#include "api/arg_reader.h"
#include "api/host_class.h"
#include "api/object_builder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>
#endif

namespace brokit::api {

namespace {

// ---------------------------------------------------------------------------
// Random
// ---------------------------------------------------------------------------

bool fillRandom(uint8_t* buf, size_t len)
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

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

std::string upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// WebCrypto algorithm names match case-insensitively and normalize to their
// registered spelling. Unknown names are returned upper-cased, which no
// dispatch below matches, so they fall through to NotSupportedError.
std::string normalizeName(const std::string& in)
{
    static const char* const kNames[] = {
        "SHA-1", "SHA-256", "SHA-384", "SHA-512", "HMAC",
        "AES-GCM", "AES-CBC", "AES-CTR", "AES-KW", "PBKDF2", "HKDF",
    };
    std::string u = upper(in);
    for (const char* n : kNames)
        if (u == n) return n;
    return u;
}

bool isSha(const std::string& n)
{
    return n == "SHA-1" || n == "SHA-256" || n == "SHA-384" || n == "SHA-512";
}

bool isAes(const std::string& n)
{
    return n == "AES-GCM" || n == "AES-CBC" || n == "AES-CTR" || n == "AES-KW";
}

size_t hashLen(const std::string& h)
{
    if (h == "SHA-1") return 20;
    if (h == "SHA-256") return 32;
    if (h == "SHA-384") return 48;
    if (h == "SHA-512") return 64;
    return 0;
}

size_t hashBlockBits(const std::string& h)
{
    return (h == "SHA-384" || h == "SHA-512") ? 1024 : 512;
}

// ---------------------------------------------------------------------------
// Usages, in the order KeyUsage lists them (which is the order key.usages
// reports them in).
// ---------------------------------------------------------------------------

enum Usage : uint32_t {
    kEncrypt = 1u << 0,
    kDecrypt = 1u << 1,
    kSign = 1u << 2,
    kVerify = 1u << 3,
    kDeriveKey = 1u << 4,
    kDeriveBits = 1u << 5,
    kWrapKey = 1u << 6,
    kUnwrapKey = 1u << 7,
};

const char* const kUsageNames[] = {
    "encrypt", "decrypt", "sign", "verify", "deriveKey", "deriveBits", "wrapKey", "unwrapKey",
};

uint32_t allowedUsages(const std::string& alg)
{
    if (alg == "HMAC") return kSign | kVerify;
    if (alg == "AES-KW") return kWrapKey | kUnwrapKey;
    if (isAes(alg)) return kEncrypt | kDecrypt | kWrapKey | kUnwrapKey;
    if (alg == "PBKDF2" || alg == "HKDF") return kDeriveKey | kDeriveBits;
    return 0;
}

// ---------------------------------------------------------------------------
// Errors. A SubtleCrypto method never throws: every failure is a rejected
// promise carrying a DOMException of the name the spec gives (or a TypeError
// for argument conversion failures).
// ---------------------------------------------------------------------------

struct Failure {
    const char* name = nullptr;  // DOMException name; nullptr = TypeError
    std::string message;
};

Value makeErrorValue(const Failure& f)
{
    ev::Persistent msg{ev::fromUtf8(f.message)};
    if (f.name) {
        ev::GlobalValue de = ev::globalValue("DOMException");
        if (de.found && ev::isFunction(de.value)) {
            ev::Persistent ctor{de.value};
            ev::Persistent name{ev::fromUtf8(f.name)};
            const Value args[2] = {msg.get(), name.get()};
            ev::CallResult r = ev::construct(ctor.get(), std::span<const Value>(args, 2));
            if (!r.thrown) return r.value;
        }
    }
    ev::GlobalValue ec = ev::globalValue(f.name ? "Error" : "TypeError");
    if (!ec.found || !ev::isFunction(ec.value)) return msg.get();
    ev::Persistent ctor{ec.value};
    const Value args[1] = {msg.get()};
    ev::CallResult r = ev::construct(ctor.get(), std::span<const Value>(args, 1));
    if (r.thrown) return msg.get();
    if (!f.name) return r.value;
    ev::Persistent err{r.value};
    ev::Persistent name{ev::fromUtf8(f.name)};
    err.set(ev::setProperty(err.get(), "name", name.get()));
    return err.get();
}

Value rejected(const Failure& f)
{
    ev::Persistent p{ev::createPromise()};
    ev::Persistent err{makeErrorValue(f)};
    ev::rejectPromise(p.get(), err.get());
    return p.get();
}

Value rejected(const char* name, std::string message)
{
    return rejected(Failure{name, std::move(message)});
}

// `v` must be fresh (nothing allocated since it was produced): it is rooted
// before the promise is.
Value resolved(Value v)
{
    ev::Persistent value{v};
    ev::Persistent p{ev::createPromise()};
    ev::resolvePromise(p.get(), value.get());
    return p.get();
}

Value resolvedBytes(const Bytes& bytes)
{
    return resolved(ev::createArrayBuffer(std::span<const uint8_t>(bytes)));
}

// ---------------------------------------------------------------------------
// Argument reading
// ---------------------------------------------------------------------------

// BufferSource: an ArrayBuffer or any ArrayBufferView (typed array, DataView,
// or brokit's Buffer, which keeps its bytes in `_u8`).
bool getBytes(Value val, Bytes& out)
{
    if (auto info = ev::typedArrayInfo(val)) {
        out.assign(info.data, info.data + info.byteLength);
        return true;
    }
    if (auto info = ev::arrayBufferInfo(val)) {
        out.assign(info.data, info.data + info.byteLength);
        return true;
    }
    if (!ev::isObject(val)) return false;
    ev::Persistent root{val};
    Value u8 = ev::getProperty(root.get(), "_u8");
    if (auto info = ev::typedArrayInfo(u8)) {
        out.assign(info.data, info.data + info.byteLength);
        return true;
    }
    // DataView: a window (byteOffset, byteLength) onto `buffer`.
    Value offV = ev::getProperty(root.get(), "byteOffset");
    Value lenV = ev::getProperty(root.get(), "byteLength");
    if (!ev::isNumber(offV) || !ev::isNumber(lenV)) return false;
    size_t off = static_cast<size_t>(ev::toDouble(offV));
    size_t len = static_cast<size_t>(ev::toDouble(lenV));
    Value buf = ev::getProperty(root.get(), "buffer");
    auto info = ev::arrayBufferInfo(buf);
    if (!info || off + len > info.byteLength) return false;
    out.assign(info.data + off, info.data + off + len);
    return true;
}

bool getBytesProp(Value obj, const char* key, Bytes& out)
{
    if (!ev::isObject(obj)) return false;
    ev::Persistent root{obj};
    Value v = ev::getProperty(root.get(), key);
    return getBytes(v, out);
}

bool hasProp(Value obj, const char* key)
{
    return ev::isObject(obj) && !ev::isUndefined(ev::getProperty(obj, key));
}

bool getNumberProp(Value obj, const char* key, double& out)
{
    if (!ev::isObject(obj)) return false;
    Value v = ev::getProperty(obj, key);
    if (!ev::isNumber(v)) return false;
    out = ev::toDouble(v);
    return true;
}

std::string getStringProp(Value obj, const char* key)
{
    if (!ev::isObject(obj)) return {};
    Value v = ev::getProperty(obj, key);
    return ev::isString(v) ? ev::toUtf8(v) : std::string();
}

// AlgorithmIdentifier: a string, or a dictionary with `name`. `hash` is itself
// a HashAlgorithmIdentifier.
struct Algorithm {
    std::string name;
    std::string hash;
};

bool parseAlgorithm(Value algo, Algorithm& out)
{
    if (ev::isString(algo)) {
        out.name = normalizeName(ev::toUtf8(algo));
        return !out.name.empty();
    }
    if (!ev::isObject(algo)) return false;
    ev::Persistent root{algo};
    out.name = normalizeName(getStringProp(root.get(), "name"));
    Value hashVal = ev::getProperty(root.get(), "hash");
    if (ev::isString(hashVal)) {
        out.hash = normalizeName(ev::toUtf8(hashVal));
    } else if (ev::isObject(hashVal)) {
        out.hash = normalizeName(getStringProp(hashVal, "name"));
    }
    return !out.name.empty();
}

bool parseUsages(Value arr, uint32_t& mask, Failure& err)
{
    mask = 0;
    if (!ev::isObject(arr)) {
        err = {nullptr, "keyUsages must be a sequence of strings"};
        return false;
    }
    ev::Persistent root{arr};
    Value lenVal = ev::getProperty(root.get(), "length");
    if (!ev::isNumber(lenVal)) {
        err = {nullptr, "keyUsages must be a sequence of strings"};
        return false;
    }
    uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));
    for (uint32_t i = 0; i < len; i++) {
        Value item = ev::getElement(root.get(), i);
        std::string s = ev::isString(item) ? ev::toUtf8(item) : std::string();
        bool known = false;
        for (uint32_t b = 0; b < 8; b++) {
            if (s == kUsageNames[b]) { mask |= 1u << b; known = true; }
        }
        if (!known) {
            err = {nullptr, "'" + s + "' is not a valid KeyUsage"};
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Platform primitives
// ---------------------------------------------------------------------------

enum class Dir { Encrypt, Decrypt };

#ifdef _WIN32

LPCWSTR bcryptHashId(const std::string& hash)
{
    if (hash == "SHA-256") return BCRYPT_SHA256_ALGORITHM;
    if (hash == "SHA-384") return BCRYPT_SHA384_ALGORITHM;
    if (hash == "SHA-512") return BCRYPT_SHA512_ALGORITHM;
    if (hash == "SHA-1") return BCRYPT_SHA1_ALGORITHM;
    return nullptr;
}

bool hashOrHmac(const std::string& hash, const uint8_t* key, size_t keyLen, bool hmac,
                const uint8_t* data, size_t dataLen, Bytes& out)
{
    LPCWSTR algId = bcryptHashId(hash);
    if (!algId) return false;
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &hAlg, algId, nullptr, hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0)))
        return false;
    DWORD len = 0, dummy = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&len), sizeof(len), &dummy, 0);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    // BCrypt refuses a null secret for HMAC; an empty key is a 1-byte buffer
    // with length 0.
    uint8_t emptyKey = 0;
    PUCHAR secret = hmac ? const_cast<PUCHAR>(keyLen ? key : &emptyKey) : nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, secret,
                                              hmac ? static_cast<ULONG>(keyLen) : 0, 0));
    if (ok) {
        ok = BCRYPT_SUCCESS(BCryptHashData(hHash, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen), 0));
        out.resize(len);
        if (ok) ok = BCRYPT_SUCCESS(BCryptFinishHash(hHash, out.data(), len, 0));
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

bool digest(const std::string& hash, const uint8_t* data, size_t len, Bytes& out)
{
    return hashOrHmac(hash, nullptr, 0, false, data, len, out);
}

bool hmac(const std::string& hash, const Bytes& key, const uint8_t* data, size_t len, Bytes& out)
{
    return hashOrHmac(hash, key.data(), key.size(), true, data, len, out);
}

struct AesKey {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    AesKey(LPCWSTR mode, size_t modeBytes, const Bytes& raw)
    {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0))) {
            alg = nullptr;
            return;
        }
        if (!BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)), static_cast<ULONG>(modeBytes), 0)) ||
            !BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &key, nullptr, 0,
                const_cast<PUCHAR>(raw.data()), static_cast<ULONG>(raw.size()), 0)))
            key = nullptr;
    }
    ~AesKey()
    {
        if (key) BCryptDestroyKey(key);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    explicit operator bool() const { return key != nullptr; }
};

// BCrypt's GCM mode takes only a 96-bit nonce and a 12-16 byte tag, where
// WebCrypto allows any non-empty IV and tags down to 32 bits (OpenSSL takes
// them all, so the other platforms need nothing extra). Outside BCrypt's
// range GCM is computed per SP 800-38D over BCrypt's raw AES block cipher:
// ECB for the counter blocks and E(0), GHASH in software.
bool aesEcb(const Bytes& raw, const uint8_t* in, size_t len, uint8_t* out)
{
    AesKey k(BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), raw);
    if (!k) return false;
    ULONG outLen = 0;
    return BCRYPT_SUCCESS(BCryptEncrypt(k.key, const_cast<PUCHAR>(in), static_cast<ULONG>(len), nullptr,
                                        nullptr, 0, out, static_cast<ULONG>(len), &outLen, 0)) &&
           outLen == len;
}

struct Block128 {
    uint64_t hi = 0, lo = 0;
};

Block128 loadBlock(const uint8_t* p)
{
    Block128 b;
    for (int i = 0; i < 8; ++i) b.hi = (b.hi << 8) | p[i];
    for (int i = 8; i < 16; ++i) b.lo = (b.lo << 8) | p[i];
    return b;
}

void storeBlock(Block128 b, uint8_t* p)
{
    for (int i = 7; i >= 0; --i, b.hi >>= 8) p[i] = static_cast<uint8_t>(b.hi);
    for (int i = 15; i >= 8; --i, b.lo >>= 8) p[i] = static_cast<uint8_t>(b.lo);
}

// X * Y in GF(2^128) with GCM's bit order (SP 800-38D Algorithm 1).
Block128 gfMul(Block128 x, Block128 y)
{
    Block128 z, v = y;
    for (int i = 0; i < 128; ++i) {
        const uint64_t bit = i < 64 ? (x.hi >> (63 - i)) & 1 : (x.lo >> (127 - i)) & 1;
        const uint64_t mask = 0 - bit;
        z.hi ^= v.hi & mask;
        z.lo ^= v.lo & mask;
        const uint64_t lsb = v.lo & 1;
        v.lo = (v.lo >> 1) | (v.hi << 63);
        v.hi = (v.hi >> 1) ^ (0xe100000000000000ull & (0 - lsb));
    }
    return z;
}

struct Ghash {
    Block128 h, y;
    void update(const uint8_t* p, size_t len)  // zero-pads the last partial block
    {
        for (size_t off = 0; off < len; off += 16) {
            uint8_t blk[16] = {};
            std::memcpy(blk, p + off, std::min<size_t>(16, len - off));
            const Block128 b = loadBlock(blk);
            y.hi ^= b.hi;
            y.lo ^= b.lo;
            y = gfMul(y, h);
        }
    }
    void lengths(uint64_t aBytes, uint64_t cBytes)
    {
        y.hi ^= aBytes * 8;
        y.lo ^= cBytes * 8;
        y = gfMul(y, h);
    }
};

bool aesGcmGeneric(Dir dir, const Bytes& raw, const Bytes& iv, const uint8_t* data, size_t len,
                   const Bytes& aad, uint8_t* tag, size_t tagLen, Bytes& out)
{
    uint8_t zero[16] = {}, hBytes[16];
    if (!aesEcb(raw, zero, 16, hBytes)) return false;
    const Block128 h = loadBlock(hBytes);

    uint8_t j0[16] = {};
    if (iv.size() == 12) {
        std::memcpy(j0, iv.data(), 12);
        j0[15] = 1;
    } else {
        Ghash g{h, {}};
        g.update(iv.data(), iv.size());
        g.lengths(0, iv.size());
        storeBlock(g.y, j0);
    }

    // Counter blocks: J0 (for the tag) then inc32(J0), inc32^2(J0), ...
    const size_t nBlocks = (len + 15) / 16;
    Bytes ctr((nBlocks + 1) * 16);
    const uint32_t c0 = (uint32_t(j0[12]) << 24) | (uint32_t(j0[13]) << 16) | (uint32_t(j0[14]) << 8) | j0[15];
    for (size_t i = 0; i <= nBlocks; ++i) {
        uint8_t* b = ctr.data() + i * 16;
        std::memcpy(b, j0, 12);
        const uint32_t c = c0 + static_cast<uint32_t>(i);
        b[12] = static_cast<uint8_t>(c >> 24);
        b[13] = static_cast<uint8_t>(c >> 16);
        b[14] = static_cast<uint8_t>(c >> 8);
        b[15] = static_cast<uint8_t>(c);
    }
    Bytes ks(ctr.size());
    if (!aesEcb(raw, ctr.data(), ctr.size(), ks.data())) return false;

    const uint8_t* cipherText = data;
    out.resize(len);
    for (size_t i = 0; i < len; ++i) out[i] = data[i] ^ ks[16 + i];
    if (dir == Dir::Encrypt) cipherText = out.data();

    Ghash g{h, {}};
    g.update(aad.data(), aad.size());
    g.update(cipherText, len);
    g.lengths(aad.size(), len);
    uint8_t full[16];
    storeBlock(g.y, full);
    for (int i = 0; i < 16; ++i) full[i] ^= ks[i];

    if (dir == Dir::Encrypt) {
        std::memcpy(tag, full, tagLen);
        return true;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < tagLen; ++i) diff |= static_cast<uint8_t>(full[i] ^ tag[i]);
    if (diff != 0) {
        std::fill(out.begin(), out.end(), uint8_t(0));
        out.clear();
        return false;
    }
    return true;
}

bool aesGcm(Dir dir, const Bytes& raw, const Bytes& iv, const uint8_t* data, size_t len,
            const Bytes& aad, uint8_t* tag, size_t tagLen, Bytes& out)
{
    if (iv.size() != 12 || tagLen < 12 || tagLen > 16)
        return aesGcmGeneric(dir, raw, iv, data, len, aad, tag, tagLen, out);
    AesKey k(BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), raw);
    if (!k) return false;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(iv.data());
    info.cbNonce = static_cast<ULONG>(iv.size());
    info.pbTag = tag;
    info.cbTag = static_cast<ULONG>(tagLen);
    if (!aad.empty()) {
        info.pbAuthData = const_cast<PUCHAR>(aad.data());
        info.cbAuthData = static_cast<ULONG>(aad.size());
    }
    out.resize(len);
    ULONG outLen = 0;
    uint8_t dummy = 0;
    PUCHAR in = const_cast<PUCHAR>(len ? data : &dummy);
    PUCHAR o = len ? out.data() : &dummy;
    NTSTATUS st = dir == Dir::Encrypt
        ? BCryptEncrypt(k.key, in, static_cast<ULONG>(len), &info, nullptr, 0, o, static_cast<ULONG>(len), &outLen, 0)
        : BCryptDecrypt(k.key, in, static_cast<ULONG>(len), &info, nullptr, 0, o, static_cast<ULONG>(len), &outLen, 0);
    if (!BCRYPT_SUCCESS(st)) return false;
    out.resize(outLen);
    return true;
}

bool aesCbc(Dir dir, const Bytes& raw, const Bytes& iv, const uint8_t* data, size_t len, Bytes& out)
{
    AesKey k(BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), raw);
    if (!k) return false;
    Bytes ivCopy = iv;  // BCrypt updates the IV buffer in place
    uint8_t dummy = 0;
    PUCHAR in = const_cast<PUCHAR>(len ? data : &dummy);
    ULONG outLen = 0;
    NTSTATUS st = dir == Dir::Encrypt
        ? BCryptEncrypt(k.key, in, static_cast<ULONG>(len), nullptr, ivCopy.data(), static_cast<ULONG>(ivCopy.size()),
                        nullptr, 0, &outLen, BCRYPT_BLOCK_PADDING)
        : BCryptDecrypt(k.key, in, static_cast<ULONG>(len), nullptr, ivCopy.data(), static_cast<ULONG>(ivCopy.size()),
                        nullptr, 0, &outLen, BCRYPT_BLOCK_PADDING);
    if (!BCRYPT_SUCCESS(st)) return false;
    out.resize(outLen ? outLen : 1);
    ivCopy = iv;
    st = dir == Dir::Encrypt
        ? BCryptEncrypt(k.key, in, static_cast<ULONG>(len), nullptr, ivCopy.data(), static_cast<ULONG>(ivCopy.size()),
                        out.data(), static_cast<ULONG>(out.size()), &outLen, BCRYPT_BLOCK_PADDING)
        : BCryptDecrypt(k.key, in, static_cast<ULONG>(len), nullptr, ivCopy.data(), static_cast<ULONG>(ivCopy.size()),
                        out.data(), static_cast<ULONG>(out.size()), &outLen, BCRYPT_BLOCK_PADDING);
    if (!BCRYPT_SUCCESS(st)) return false;
    out.resize(outLen);
    return true;
}

// Raw AES over whole 16-byte blocks, no padding: the primitive AES-CTR and
// AES-KW are built on.
bool aesEcb(Dir dir, const Bytes& raw, const uint8_t* data, size_t len, uint8_t* out)
{
    if (len == 0) return true;
    AesKey k(BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), raw);
    if (!k) return false;
    ULONG outLen = 0;
    NTSTATUS st = dir == Dir::Encrypt
        ? BCryptEncrypt(k.key, const_cast<PUCHAR>(data), static_cast<ULONG>(len), nullptr, nullptr, 0,
                        out, static_cast<ULONG>(len), &outLen, 0)
        : BCryptDecrypt(k.key, const_cast<PUCHAR>(data), static_cast<ULONG>(len), nullptr, nullptr, 0,
                        out, static_cast<ULONG>(len), &outLen, 0);
    return BCRYPT_SUCCESS(st) && outLen == len;
}

bool pbkdf2(const std::string& hash, const Bytes& password, const Bytes& salt, uint64_t iterations,
            size_t outLen, Bytes& out)
{
    LPCWSTR algId = bcryptHashId(hash);
    if (!algId) return false;
    BCRYPT_ALG_HANDLE hPrf = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hPrf, algId, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return false;
    out.resize(outLen);
    uint8_t dummy = 0;
    NTSTATUS st = BCryptDeriveKeyPBKDF2(
        hPrf, const_cast<PUCHAR>(password.empty() ? &dummy : password.data()), static_cast<ULONG>(password.size()),
        const_cast<PUCHAR>(salt.empty() ? &dummy : salt.data()), static_cast<ULONG>(salt.size()),
        iterations, out.data(), static_cast<ULONG>(outLen), 0);
    BCryptCloseAlgorithmProvider(hPrf, 0);
    return BCRYPT_SUCCESS(st);
}

#else  // OpenSSL

const EVP_MD* mdFor(const std::string& hash)
{
    if (hash == "SHA-256") return EVP_sha256();
    if (hash == "SHA-384") return EVP_sha384();
    if (hash == "SHA-512") return EVP_sha512();
    if (hash == "SHA-1") return EVP_sha1();
    return nullptr;
}

bool digest(const std::string& hash, const uint8_t* data, size_t len, Bytes& out)
{
    const EVP_MD* md = mdFor(hash);
    if (!md) return false;
    out.resize(static_cast<size_t>(EVP_MD_size(md)));
    unsigned int outLen = 0;
    if (EVP_Digest(data, len, out.data(), &outLen, md, nullptr) != 1) return false;
    out.resize(outLen);
    return true;
}

bool hmac(const std::string& hash, const Bytes& key, const uint8_t* data, size_t len, Bytes& out)
{
    const EVP_MD* md = mdFor(hash);
    if (!md) return false;
    uint8_t emptyKey = 0;
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_HMAC, nullptr,
                                                  key.empty() ? &emptyKey : key.data(), key.size());
    if (!pkey) return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    size_t reqLen = 0;
    bool ok = ctx && EVP_DigestSignInit(ctx, nullptr, md, nullptr, pkey) == 1 &&
              EVP_DigestSignUpdate(ctx, data, len) == 1 &&
              EVP_DigestSignFinal(ctx, nullptr, &reqLen) == 1;
    if (ok) {
        out.resize(reqLen);
        ok = EVP_DigestSignFinal(ctx, out.data(), &reqLen) == 1;
        out.resize(reqLen);
    }
    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

const EVP_CIPHER* aesCipher(const char* mode, size_t keyLen)
{
    const bool gcm = std::strcmp(mode, "gcm") == 0;
    const bool cbc = std::strcmp(mode, "cbc") == 0;
    switch (keyLen) {
    case 16: return gcm ? EVP_aes_128_gcm() : cbc ? EVP_aes_128_cbc() : EVP_aes_128_ecb();
    case 24: return gcm ? EVP_aes_192_gcm() : cbc ? EVP_aes_192_cbc() : EVP_aes_192_ecb();
    case 32: return gcm ? EVP_aes_256_gcm() : cbc ? EVP_aes_256_cbc() : EVP_aes_256_ecb();
    default: return nullptr;
    }
}

bool aesGcm(Dir dir, const Bytes& raw, const Bytes& iv, const uint8_t* data, size_t len,
            const Bytes& aad, uint8_t* tag, size_t tagLen, Bytes& out)
{
    const EVP_CIPHER* cipher = aesCipher("gcm", raw.size());
    if (!cipher) return false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    const int enc = dir == Dir::Encrypt ? 1 : 0;
    int n = 0;
    bool ok = EVP_CipherInit_ex(ctx, cipher, nullptr, nullptr, nullptr, enc) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) == 1 &&
              EVP_CipherInit_ex(ctx, nullptr, nullptr, raw.data(), iv.data(), enc) == 1;
    if (ok && !aad.empty())
        ok = EVP_CipherUpdate(ctx, nullptr, &n, aad.data(), static_cast<int>(aad.size())) == 1;
    out.resize(len + 16);
    int total = 0;
    if (ok && len > 0) {
        ok = EVP_CipherUpdate(ctx, out.data(), &n, data, static_cast<int>(len)) == 1;
        total = n;
    }
    if (ok && dir == Dir::Decrypt)
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tagLen), tag) == 1;
    if (ok) {
        ok = EVP_CipherFinal_ex(ctx, out.data() + total, &n) == 1;
        total += n;
    }
    if (ok && dir == Dir::Encrypt)
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(tagLen), tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    out.resize(ok ? static_cast<size_t>(total) : 0);
    return ok;
}

bool aesCipherRun(const EVP_CIPHER* cipher, Dir dir, bool padding, const Bytes& raw, const uint8_t* iv,
                  const uint8_t* data, size_t len, Bytes& out)
{
    if (!cipher) return false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int n = 0, total = 0;
    out.resize(len + 16);
    bool ok = EVP_CipherInit_ex(ctx, cipher, nullptr, raw.data(), iv, dir == Dir::Encrypt ? 1 : 0) == 1;
    if (ok) EVP_CIPHER_CTX_set_padding(ctx, padding ? 1 : 0);
    if (ok && len > 0) {
        ok = EVP_CipherUpdate(ctx, out.data(), &n, data, static_cast<int>(len)) == 1;
        total = n;
    }
    if (ok) {
        ok = EVP_CipherFinal_ex(ctx, out.data() + total, &n) == 1;
        total += n;
    }
    EVP_CIPHER_CTX_free(ctx);
    out.resize(ok ? static_cast<size_t>(total) : 0);
    return ok;
}

bool aesCbc(Dir dir, const Bytes& raw, const Bytes& iv, const uint8_t* data, size_t len, Bytes& out)
{
    return aesCipherRun(aesCipher("cbc", raw.size()), dir, true, raw, iv.data(), data, len, out);
}

bool aesEcb(Dir dir, const Bytes& raw, const uint8_t* data, size_t len, uint8_t* out)
{
    if (len == 0) return true;
    Bytes tmp;
    if (!aesCipherRun(aesCipher("ecb", raw.size()), dir, false, raw, nullptr, data, len, tmp) ||
        tmp.size() != len)
        return false;
    std::memcpy(out, tmp.data(), len);
    return true;
}

bool pbkdf2(const std::string& hash, const Bytes& password, const Bytes& salt, uint64_t iterations,
            size_t outLen, Bytes& out)
{
    const EVP_MD* md = mdFor(hash);
    if (!md || iterations > 0x7fffffffULL) return false;
    out.resize(outLen);
    return PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(password.data()), static_cast<int>(password.size()),
                             salt.data(), static_cast<int>(salt.size()), static_cast<int>(iterations), md,
                             static_cast<int>(outLen), out.data()) == 1;
}

#endif

// ---------------------------------------------------------------------------
// Constructions over the primitives
// ---------------------------------------------------------------------------

// HKDF (RFC 5869) over HMAC.
bool hkdf(const std::string& hash, const Bytes& ikm, const Bytes& salt, const Bytes& info, size_t outLen,
          Bytes& out)
{
    const size_t hl = hashLen(hash);
    if (hl == 0 || outLen > 255 * hl) return false;
    Bytes prk;
    Bytes saltKey = salt.empty() ? Bytes(hl, 0) : salt;
    if (!hmac(hash, saltKey, ikm.data(), ikm.size(), prk)) return false;
    out.clear();
    Bytes t;
    for (uint8_t i = 1; out.size() < outLen; i++) {
        Bytes msg = t;
        msg.insert(msg.end(), info.begin(), info.end());
        msg.push_back(i);
        if (!hmac(hash, prk, msg.data(), msg.size(), t)) return false;
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(outLen);
    return true;
}

// Add one to the rightmost `bits` bits of a 16-byte counter block, wrapping
// within them and leaving the nonce bits alone.
void ctrIncrement(uint8_t block[16], int bits)
{
    for (int i = 15; i >= 0 && bits > 0; --i) {
        const int here = bits < 8 ? bits : 8;
        const uint8_t mask = here == 8 ? 0xFF : static_cast<uint8_t>((1u << here) - 1);
        const uint8_t low = static_cast<uint8_t>((block[i] + 1) & mask);
        block[i] = static_cast<uint8_t>((block[i] & ~mask) | low);
        if (low != 0) return;
        bits -= here;
    }
}

bool aesCtr(const Bytes& raw, const Bytes& counter, int lengthBits, const uint8_t* data, size_t len, Bytes& out)
{
    const size_t blocks = (len + 15) / 16;
    if (lengthBits < 64 && blocks > (uint64_t{1} << lengthBits)) return false;  // counter would repeat
    Bytes ctrBlocks(blocks * 16);
    uint8_t ctr[16];
    std::memcpy(ctr, counter.data(), 16);
    for (size_t b = 0; b < blocks; b++) {
        std::memcpy(ctrBlocks.data() + b * 16, ctr, 16);
        ctrIncrement(ctr, lengthBits);
    }
    Bytes stream(ctrBlocks.size());
    if (!aesEcb(Dir::Encrypt, raw, ctrBlocks.data(), ctrBlocks.size(), stream.data())) return false;
    out.resize(len);
    for (size_t i = 0; i < len; i++) out[i] = data[i] ^ stream[i];
    return true;
}

// AES Key Wrap (RFC 3394), default IV.
const uint8_t kKwIv[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};

void xorT(uint8_t a[8], uint64_t t)
{
    for (int k = 7; k >= 0; --k) { a[k] ^= static_cast<uint8_t>(t & 0xFF); t >>= 8; }
}

bool aesKwWrap(const Bytes& kek, const Bytes& plain, Bytes& out)
{
    if (plain.size() < 16 || plain.size() % 8 != 0) return false;
    const size_t n = plain.size() / 8;
    uint8_t a[8];
    std::memcpy(a, kKwIv, 8);
    Bytes r = plain;
    uint8_t b[16], o[16];
    for (int j = 0; j <= 5; j++) {
        for (size_t i = 1; i <= n; i++) {
            std::memcpy(b, a, 8);
            std::memcpy(b + 8, &r[(i - 1) * 8], 8);
            if (!aesEcb(Dir::Encrypt, kek, b, 16, o)) return false;
            std::memcpy(a, o, 8);
            xorT(a, n * j + i);
            std::memcpy(&r[(i - 1) * 8], o + 8, 8);
        }
    }
    out.assign(a, a + 8);
    out.insert(out.end(), r.begin(), r.end());
    return true;
}

bool aesKwUnwrap(const Bytes& kek, const Bytes& wrapped, Bytes& out)
{
    if (wrapped.size() < 24 || wrapped.size() % 8 != 0) return false;
    const size_t n = wrapped.size() / 8 - 1;
    uint8_t a[8];
    std::memcpy(a, wrapped.data(), 8);
    Bytes r(wrapped.begin() + 8, wrapped.end());
    uint8_t b[16], o[16];
    for (int j = 5; j >= 0; j--) {
        for (size_t i = n; i >= 1; i--) {
            xorT(a, n * j + i);
            std::memcpy(b, a, 8);
            std::memcpy(b + 8, &r[(i - 1) * 8], 8);
            if (!aesEcb(Dir::Decrypt, kek, b, 16, o)) return false;
            std::memcpy(a, o, 8);
            std::memcpy(&r[(i - 1) * 8], o + 8, 8);
        }
    }
    uint8_t diff = 0;
    for (int k = 0; k < 8; k++) diff |= static_cast<uint8_t>(a[k] ^ kKwIv[k]);
    if (diff != 0) return false;
    out = std::move(r);
    return true;
}

// ---------------------------------------------------------------------------
// base64url (JWK)
// ---------------------------------------------------------------------------

std::string base64urlEncode(const Bytes& in)
{
    static const char* kAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += kAlpha[(v >> 18) & 63];
        out += kAlpha[(v >> 12) & 63];
        out += kAlpha[(v >> 6) & 63];
        out += kAlpha[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = in[i] << 16;
        out += kAlpha[(v >> 18) & 63];
        out += kAlpha[(v >> 12) & 63];
    } else if (i + 2 == in.size()) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8);
        out += kAlpha[(v >> 18) & 63];
        out += kAlpha[(v >> 12) & 63];
        out += kAlpha[(v >> 6) & 63];
    }
    return out;
}

bool base64urlDecode(const std::string& in, Bytes& out)
{
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-') v = 62;
        else if (c == '_') v = 63;
        else if (c == '=') break;
        else return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return bits < 6;  // a lone trailing sextet is not a whole byte
}

// ---------------------------------------------------------------------------
// CryptoKey
// ---------------------------------------------------------------------------

struct CryptoKeyData {
    Bytes raw;
    std::string name;  // normalized algorithm name
    std::string hash;  // HMAC only
    bool extractable = false;
    uint32_t usages = 0;
};

HostClass g_cryptoKeyClass;
HostClass g_subtleClass;

// handleData answers for ANY host handle, so a Blob passed where a key is
// expected would otherwise be reinterpreted as a CryptoKeyData. The live set
// is the brand check. Keys are made and finalized on their runtime's thread.
std::unordered_set<const CryptoKeyData*>& liveKeys()
{
    static thread_local auto* s = new std::unordered_set<const CryptoKeyData*>();
    return *s;
}

void cryptoKeyDtor(void* p)
{
    auto* k = static_cast<CryptoKeyData*>(p);
    liveKeys().erase(k);
    delete k;
}

CryptoKeyData* keyOf(Value v)
{
    auto* k = static_cast<CryptoKeyData*>(ev::handleData(v));
    return (k && liveKeys().count(k)) ? k : nullptr;
}

Value makeAlgorithmObject(const CryptoKeyData& k)
{
    ObjectBuilder algo;
    algo.set("name", k.name);
    if (k.name == "HMAC") {
        ObjectBuilder hash;
        hash.set("name", k.hash);
        algo.set("hash", hash.get());
        algo.set("length", static_cast<double>(k.raw.size() * 8));
    } else if (isAes(k.name)) {
        algo.set("length", static_cast<double>(k.raw.size() * 8));
    }
    return algo.get();
}

Value makeUsagesArray(uint32_t usages)
{
    ArrayBuilder arr;
    for (uint32_t b = 0; b < 8; b++)
        if (usages & (1u << b)) arr.push(ev::fromUtf8(kUsageNames[b]));
    return arr.get();
}

Value makeCryptoKey(CryptoKeyData* key)
{
    liveKeys().insert(key);
    ObjectBuilder obj(g_cryptoKeyClass.make(key, cryptoKeyDtor));
    obj.set("type", "secret");
    obj.set("extractable", key->extractable);
    obj.set("algorithm", makeAlgorithmObject(*key));
    obj.set("usages", makeUsagesArray(key->usages));
    return obj.get();
}

// ---------------------------------------------------------------------------
// Shared import / export
// ---------------------------------------------------------------------------

// Validates `usages` for `alg` and the key-material rules every format shares,
// then builds the key. `raw` is consumed.
CryptoKeyData* buildKey(const Algorithm& algo, Bytes raw, bool extractable, uint32_t usages,
                        Failure& err)
{
    const uint32_t allowed = allowedUsages(algo.name);
    if (allowed == 0) {
        err = {"NotSupportedError", "algorithm '" + algo.name + "' is not supported"};
        return nullptr;
    }
    if (usages & ~allowed) {
        err = {"SyntaxError", "key usages are not valid for " + algo.name};
        return nullptr;
    }
    if (usages == 0) {
        err = {"SyntaxError", "a secret key needs at least one usage"};
        return nullptr;
    }
    auto* key = new CryptoKeyData();
    key->name = algo.name;
    if (algo.name == "HMAC") {
        if (!isSha(algo.hash)) {
            delete key;
            err = {algo.hash.empty() ? nullptr : "NotSupportedError", "HMAC needs a supported hash"};
            return nullptr;
        }
        if (raw.empty()) {
            delete key;
            err = {"DataError", "HMAC key data is empty"};
            return nullptr;
        }
        key->hash = algo.hash;
    } else if (isAes(algo.name)) {
        if (raw.size() != 16 && raw.size() != 24 && raw.size() != 32) {
            delete key;
            err = {"DataError", "AES key data must be 128, 192 or 256 bits"};
            return nullptr;
        }
    } else {  // PBKDF2 / HKDF
        if (extractable) {
            delete key;
            err = {"SyntaxError", algo.name + " keys cannot be extractable"};
            return nullptr;
        }
    }
    key->raw = std::move(raw);
    key->extractable = extractable;
    key->usages = usages;
    return key;
}

std::string jwkAlg(const CryptoKeyData& k)
{
    if (k.name == "HMAC") return "HS" + (k.hash == "SHA-1" ? std::string("1") : k.hash.substr(4));
    const std::string bits = std::to_string(k.raw.size() * 8);
    if (k.name == "AES-GCM") return "A" + bits + "GCM";
    if (k.name == "AES-CBC") return "A" + bits + "CBC";
    if (k.name == "AES-CTR") return "A" + bits + "CTR";
    if (k.name == "AES-KW") return "A" + bits + "KW";
    return {};
}

// Import a key from a JWK dictionary.
CryptoKeyData* importJwk(Value jwkIn, const Algorithm& algo, bool extractable, uint32_t usages,
                         Failure& err)
{
    if (!ev::isObject(jwkIn)) {
        err = {nullptr, "JWK keyData must be an object"};
        return nullptr;
    }
    if (algo.name != "HMAC" && !isAes(algo.name)) {
        err = {"NotSupportedError", "jwk import is not supported for " + algo.name};
        return nullptr;
    }
    ev::Persistent jwk{jwkIn};
    if (getStringProp(jwk.get(), "kty") != "oct") {
        err = {"DataError", "JWK kty must be 'oct' for a secret key"};
        return nullptr;
    }
    const std::string k = getStringProp(jwk.get(), "k");
    Bytes raw;
    if (k.empty() || !base64urlDecode(k, raw)) {
        err = {"DataError", "JWK 'k' is missing or not base64url"};
        return nullptr;
    }
    Value extV = ev::getProperty(jwk.get(), "ext");
    if (ev::isBool(extV) && !ev::toBool(extV) && extractable) {
        err = {"DataError", "JWK ext is false but an extractable key was requested"};
        return nullptr;
    }
    const std::string use = getStringProp(jwk.get(), "use");
    if (!use.empty() && use != (algo.name == "HMAC" ? "sig" : "enc")) {
        err = {"DataError", "JWK 'use' does not match the algorithm"};
        return nullptr;
    }
    const std::string alg = getStringProp(jwk.get(), "alg");
    CryptoKeyData* key = buildKey(algo, std::move(raw), extractable, usages, err);
    if (!key) return nullptr;
    if (!alg.empty() && alg != jwkAlg(*key)) {
        err = {"DataError", "JWK alg '" + alg + "' does not match the key"};
        delete key;
        return nullptr;
    }
    return key;
}

Value exportJwk(const CryptoKeyData& k)
{
    ObjectBuilder jwk;
    jwk.set("kty", "oct");
    jwk.set("k", base64urlEncode(k.raw));
    jwk.set("alg", jwkAlg(k));
    jwk.set("ext", k.extractable);
    jwk.set("key_ops", makeUsagesArray(k.usages));
    return jwk.get();
}

// Export `key` in `format` as raw bytes (a jwk is serialized as JSON, the form
// wrapKey encrypts).
bool exportBytes(const std::string& format, const CryptoKeyData& key, Bytes& out, Failure& err)
{
    if (!key.extractable) {
        err = {"InvalidAccessError", "key is not extractable"};
        return false;
    }
    if (format == "raw") {
        out = key.raw;
        return true;
    }
    if (format == "jwk") {
        ev::Persistent obj{exportJwk(key)};
        ev::GlobalValue json = ev::globalValue("JSON");
        if (!json.found) { err = {"OperationError", "JSON is unavailable"}; return false; }
        ev::Persistent jsonNs{json.value};
        ev::Persistent stringify{ev::getProperty(jsonNs.get(), "stringify")};
        const Value args[1] = {obj.get()};
        ev::CallResult r = ev::call(stringify.get(), jsonNs.get(), std::span<const Value>(args, 1));
        if (r.thrown || !ev::isString(r.value)) { err = {"OperationError", "JWK serialization failed"}; return false; }
        const std::string s = ev::toUtf8(r.value);
        out.assign(s.begin(), s.end());
        return true;
    }
    err = {"NotSupportedError", "unsupported key format '" + format + "'"};
    return false;
}

// ---------------------------------------------------------------------------
// Cipher dispatch shared by encrypt/decrypt/wrapKey/unwrapKey
// ---------------------------------------------------------------------------

bool runCipher(Dir dir, Value params, const Algorithm& algo, const CryptoKeyData& key, const Bytes& data,
               Bytes& out, Failure& err)
{
    if (algo.name != key.name) {
        err = {"InvalidAccessError", "algorithm does not match the key's algorithm (" + key.name + ")"};
        return false;
    }
    ev::Persistent p{params};
    if (algo.name == "AES-GCM") {
        Bytes iv, aad;
        if (!getBytesProp(p.get(), "iv", iv)) { err = {nullptr, "AES-GCM requires 'iv'"}; return false; }
        if (iv.empty()) { err = {"OperationError", "AES-GCM iv must not be empty"}; return false; }
        if (hasProp(p.get(), "additionalData") && !getBytesProp(p.get(), "additionalData", aad)) {
            err = {nullptr, "additionalData must be a BufferSource"};
            return false;
        }
        double tagBits = 128;
        getNumberProp(p.get(), "tagLength", tagBits);
        static const int kTagBits[] = {32, 64, 96, 104, 112, 120, 128};
        if (std::find(std::begin(kTagBits), std::end(kTagBits), static_cast<int>(tagBits)) == std::end(kTagBits)) {
            err = {"OperationError", "invalid AES-GCM tagLength"};
            return false;
        }
        const size_t tagLen = static_cast<size_t>(tagBits) / 8;
        if (dir == Dir::Encrypt) {
            uint8_t tag[16];
            if (!aesGcm(dir, key.raw, iv, data.data(), data.size(), aad, tag, 16, out)) {
                err = {"OperationError", "AES-GCM encryption failed"};
                return false;
            }
            // A truncated GCM tag is the leading bytes of the full one.
            out.insert(out.end(), tag, tag + tagLen);
            return true;
        }
        if (data.size() < tagLen) { err = {"OperationError", "ciphertext is shorter than the tag"}; return false; }
        const size_t ctLen = data.size() - tagLen;
        uint8_t tag[16];
        std::memcpy(tag, data.data() + ctLen, tagLen);
        if (!aesGcm(dir, key.raw, iv, data.data(), ctLen, aad, tag, tagLen, out)) {
            err = {"OperationError", "AES-GCM decryption failed (wrong key, iv, or tampered data)"};
            return false;
        }
        return true;
    }
    if (algo.name == "AES-CBC") {
        Bytes iv;
        if (!getBytesProp(p.get(), "iv", iv)) { err = {nullptr, "AES-CBC requires 'iv'"}; return false; }
        if (iv.size() != 16) { err = {"OperationError", "AES-CBC iv must be 16 bytes"}; return false; }
        if (dir == Dir::Decrypt && (data.empty() || data.size() % 16 != 0)) {
            err = {"OperationError", "AES-CBC ciphertext must be a whole number of blocks"};
            return false;
        }
        if (!aesCbc(dir, key.raw, iv, data.data(), data.size(), out)) {
            err = {"OperationError", dir == Dir::Encrypt ? "AES-CBC encryption failed"
                                                         : "AES-CBC decryption failed (bad padding)"};
            return false;
        }
        return true;
    }
    if (algo.name == "AES-CTR") {
        Bytes counter;
        if (!getBytesProp(p.get(), "counter", counter)) { err = {nullptr, "AES-CTR requires 'counter'"}; return false; }
        if (counter.size() != 16) { err = {"OperationError", "AES-CTR counter must be 16 bytes"}; return false; }
        double length = 0;
        if (!getNumberProp(p.get(), "length", length)) { err = {nullptr, "AES-CTR requires 'length'"}; return false; }
        if (length < 1 || length > 128) { err = {"OperationError", "AES-CTR length must be 1..128"}; return false; }
        if (!aesCtr(key.raw, counter, static_cast<int>(length), data.data(), data.size(), out)) {
            err = {"OperationError", "AES-CTR failed (counter space exhausted?)"};
            return false;
        }
        return true;
    }
    err = {"NotSupportedError", "cipher '" + algo.name + "' is not supported"};
    return false;
}

// ---------------------------------------------------------------------------
// deriveBits core (PBKDF2 / HKDF)
// ---------------------------------------------------------------------------

bool deriveBitsCore(Value params, const Algorithm& algo, const CryptoKeyData& base, double lengthBits,
                    Bytes& out, Failure& err)
{
    if (algo.name != base.name) {
        err = {"InvalidAccessError", "algorithm does not match the base key's algorithm (" + base.name + ")"};
        return false;
    }
    if (!(lengthBits > 0) || std::fmod(lengthBits, 8.0) != 0) {
        err = {"OperationError", "length must be a positive multiple of 8"};
        return false;
    }
    const size_t outLen = static_cast<size_t>(lengthBits / 8);
    if (!isSha(algo.hash)) {
        err = {algo.hash.empty() ? nullptr : "NotSupportedError", algo.name + " needs a supported hash"};
        return false;
    }
    ev::Persistent p{params};
    Bytes salt;
    if (!getBytesProp(p.get(), "salt", salt)) { err = {nullptr, algo.name + " requires 'salt'"}; return false; }
    if (algo.name == "PBKDF2") {
        double iterations = 0;
        if (!getNumberProp(p.get(), "iterations", iterations)) {
            err = {nullptr, "PBKDF2 requires 'iterations'"};
            return false;
        }
        if (iterations < 1) { err = {"OperationError", "PBKDF2 iterations must be at least 1"}; return false; }
        if (!pbkdf2(algo.hash, base.raw, salt, static_cast<uint64_t>(iterations), outLen, out)) {
            err = {"OperationError", "PBKDF2 failed"};
            return false;
        }
        return true;
    }
    if (algo.name == "HKDF") {
        Bytes info;
        if (!getBytesProp(p.get(), "info", info)) { err = {nullptr, "HKDF requires 'info'"}; return false; }
        if (!hkdf(algo.hash, base.raw, salt, info, outLen, out)) {
            err = {"OperationError", "HKDF failed (length too long for the hash?)"};
            return false;
        }
        return true;
    }
    err = {"NotSupportedError", "deriveBits is not supported for " + algo.name};
    return false;
}

// The key length a derivedKeyType asks for, per its "get key length" operation.
bool derivedKeyLength(Value type, const Algorithm& algo, double& bits, Failure& err)
{
    if (isAes(algo.name)) {
        if (!getNumberProp(type, "length", bits)) { err = {nullptr, "AES derivedKeyType requires 'length'"}; return false; }
        if (bits != 128 && bits != 192 && bits != 256) {
            err = {"OperationError", "AES length must be 128, 192 or 256"};
            return false;
        }
        return true;
    }
    if (algo.name == "HMAC") {
        if (!isSha(algo.hash)) { err = {nullptr, "HMAC derivedKeyType needs a supported hash"}; return false; }
        if (!getNumberProp(type, "length", bits)) bits = static_cast<double>(hashBlockBits(algo.hash));
        if (bits <= 0) { err = {nullptr, "HMAC length must be positive"}; return false; }
        return true;
    }
    err = {"NotSupportedError", "cannot derive a key for " + algo.name};
    return false;
}

// ---------------------------------------------------------------------------
// SubtleCrypto methods
// ---------------------------------------------------------------------------

Value argOr(std::span<const Value> a, size_t i)
{
    return i < a.size() ? a[i] : ev::undefined();
}

Value subtleDigest(Value, std::span<const Value> a)
{
    Algorithm algo;
    if (!parseAlgorithm(argOr(a, 0), algo)) return rejected(nullptr, "digest: invalid algorithm");
    if (!isSha(algo.name)) return rejected("NotSupportedError", "digest: unsupported algorithm '" + algo.name + "'");
    Bytes data;
    if (!getBytes(argOr(a, 1), data)) return rejected(nullptr, "digest: data must be a BufferSource");
    Bytes out;
    if (!digest(algo.name, data.data(), data.size(), out)) return rejected("OperationError", "digest failed");
    return resolvedBytes(out);
}

Value subtleImportKey(Value, std::span<const Value> a)
{
    if (!ev::isString(argOr(a, 0))) return rejected(nullptr, "importKey: format must be a string");
    const std::string format = ev::toUtf8(a[0]);
    Algorithm algo;
    if (!parseAlgorithm(argOr(a, 2), algo)) return rejected(nullptr, "importKey: invalid algorithm");
    const bool extractable = ev::toBool(argOr(a, 3));
    uint32_t usages = 0;
    Failure err;
    if (!parseUsages(argOr(a, 4), usages, err)) return rejected(err);

    CryptoKeyData* key = nullptr;
    if (format == "raw") {
        Bytes raw;
        if (!getBytes(argOr(a, 1), raw)) return rejected(nullptr, "importKey: keyData must be a BufferSource");
        key = buildKey(algo, std::move(raw), extractable, usages, err);
    } else if (format == "jwk") {
        key = importJwk(argOr(a, 1), algo, extractable, usages, err);
    } else {
        return rejected("NotSupportedError", "importKey: unsupported format '" + format + "'");
    }
    if (!key) return rejected(err);
    return resolved(makeCryptoKey(key));
}

Value subtleExportKey(Value, std::span<const Value> a)
{
    if (!ev::isString(argOr(a, 0))) return rejected(nullptr, "exportKey: format must be a string");
    const std::string format = ev::toUtf8(a[0]);
    CryptoKeyData* key = keyOf(argOr(a, 1));
    if (!key) return rejected(nullptr, "exportKey: key is not a CryptoKey");
    if (!key->extractable) return rejected("InvalidAccessError", "exportKey: key is not extractable");
    if (format == "raw") return resolvedBytes(key->raw);
    if (format == "jwk") {
        if (key->name != "HMAC" && !isAes(key->name))
            return rejected("NotSupportedError", "exportKey: jwk is not supported for " + key->name);
        return resolved(exportJwk(*key));
    }
    return rejected("NotSupportedError", "exportKey: unsupported format '" + format + "'");
}

Value subtleGenerateKey(Value, std::span<const Value> a)
{
    Algorithm algo;
    if (!parseAlgorithm(argOr(a, 0), algo)) return rejected(nullptr, "generateKey: invalid algorithm");
    const bool extractable = ev::toBool(argOr(a, 1));
    uint32_t usages = 0;
    Failure err;
    if (!parseUsages(argOr(a, 2), usages, err)) return rejected(err);

    double bits = 0;
    if (algo.name == "HMAC") {
        if (!isSha(algo.hash))
            return rejected(algo.hash.empty() ? nullptr : "NotSupportedError", "generateKey: HMAC needs a supported hash");
        if (!getNumberProp(a[0], "length", bits)) bits = static_cast<double>(hashBlockBits(algo.hash));
        if (bits <= 0) return rejected("OperationError", "generateKey: HMAC length must be positive");
    } else if (isAes(algo.name)) {
        if (!getNumberProp(argOr(a, 0), "length", bits)) return rejected(nullptr, "generateKey: AES requires 'length'");
        if (bits != 128 && bits != 192 && bits != 256)
            return rejected("OperationError", "generateKey: AES length must be 128, 192, or 256");
    } else {
        return rejected("NotSupportedError", "generateKey: unsupported algorithm '" + algo.name + "'");
    }
    Bytes raw((static_cast<size_t>(bits) + 7) / 8);
    if (!fillRandom(raw.data(), raw.size())) return rejected("OperationError", "generateKey: OS RNG failed");
    CryptoKeyData* key = buildKey(algo, std::move(raw), extractable, usages, err);
    if (!key) return rejected(err);
    return resolved(makeCryptoKey(key));
}

Value signOrVerify(bool verify, std::span<const Value> a)
{
    const char* op = verify ? "verify" : "sign";
    Algorithm algo;
    if (!parseAlgorithm(argOr(a, 0), algo)) return rejected(nullptr, std::string(op) + ": invalid algorithm");
    CryptoKeyData* key = keyOf(argOr(a, 1));
    if (!key) return rejected(nullptr, std::string(op) + ": key is not a CryptoKey");
    Bytes sig, data;
    if (verify && !getBytes(argOr(a, 2), sig)) return rejected(nullptr, "verify: signature must be a BufferSource");
    if (!getBytes(argOr(a, verify ? 3 : 2), data)) return rejected(nullptr, std::string(op) + ": data must be a BufferSource");
    if (algo.name != key->name)
        return rejected("InvalidAccessError", std::string(op) + ": algorithm does not match the key");
    if (!(key->usages & (verify ? kVerify : kSign)))
        return rejected("InvalidAccessError", std::string(op) + ": key does not have '" + op + "' usage");
    if (algo.name != "HMAC") return rejected("NotSupportedError", std::string(op) + ": unsupported algorithm");
    Bytes mac;
    if (!hmac(key->hash, key->raw, data.data(), data.size(), mac)) return rejected("OperationError", "HMAC failed");
    if (!verify) return resolvedBytes(mac);
    bool match = sig.size() == mac.size();
    uint8_t diff = 0;
    for (size_t i = 0; match && i < sig.size(); i++) diff |= static_cast<uint8_t>(sig[i] ^ mac[i]);
    return resolved(ev::fromBool(match && diff == 0));
}

Value subtleSign(Value, std::span<const Value> a) { return signOrVerify(false, a); }
Value subtleVerify(Value, std::span<const Value> a) { return signOrVerify(true, a); }

Value encryptOrDecrypt(Dir dir, std::span<const Value> a)
{
    const char* op = dir == Dir::Encrypt ? "encrypt" : "decrypt";
    Algorithm algo;
    if (!parseAlgorithm(argOr(a, 0), algo)) return rejected(nullptr, std::string(op) + ": invalid algorithm");
    CryptoKeyData* key = keyOf(argOr(a, 1));
    if (!key) return rejected(nullptr, std::string(op) + ": key is not a CryptoKey");
    Bytes data;
    if (!getBytes(argOr(a, 2), data)) return rejected(nullptr, std::string(op) + ": data must be a BufferSource");
    if (algo.name == "AES-KW")
        return rejected("NotSupportedError", std::string(op) + ": AES-KW only wraps keys");
    if (!(key->usages & (dir == Dir::Encrypt ? kEncrypt : kDecrypt)))
        return rejected("InvalidAccessError", std::string(op) + ": key does not have '" + op + "' usage");
    Bytes out;
    Failure err;
    if (!runCipher(dir, argOr(a, 0), algo, *key, data, out, err)) return rejected(err);
    return resolvedBytes(out);
}

Value subtleEncrypt(Value, std::span<const Value> a) { return encryptOrDecrypt(Dir::Encrypt, a); }
Value subtleDecrypt(Value, std::span<const Value> a) { return encryptOrDecrypt(Dir::Decrypt, a); }

Value subtleDeriveBits(Value, std::span<const Value> a)
{
    Algorithm algo;
    if (!parseAlgorithm(argOr(a, 0), algo)) return rejected(nullptr, "deriveBits: invalid algorithm");
    CryptoKeyData* base = keyOf(argOr(a, 1));
    if (!base) return rejected(nullptr, "deriveBits: baseKey is not a CryptoKey");
    if (!(base->usages & kDeriveBits))
        return rejected("InvalidAccessError", "deriveBits: key does not have 'deriveBits' usage");
    Value lenV = argOr(a, 2);
    if (!ev::isNumber(lenV)) return rejected("OperationError", "deriveBits: length is required");
    Bytes out;
    Failure err;
    if (!deriveBitsCore(argOr(a, 0), algo, *base, ev::toDouble(lenV), out, err)) return rejected(err);
    return resolvedBytes(out);
}

Value subtleDeriveKey(Value, std::span<const Value> a)
{
    Algorithm algo, derivedAlgo;
    if (!parseAlgorithm(argOr(a, 0), algo)) return rejected(nullptr, "deriveKey: invalid algorithm");
    CryptoKeyData* base = keyOf(argOr(a, 1));
    if (!base) return rejected(nullptr, "deriveKey: baseKey is not a CryptoKey");
    if (!parseAlgorithm(argOr(a, 2), derivedAlgo)) return rejected(nullptr, "deriveKey: invalid derivedKeyType");
    const bool extractable = ev::toBool(argOr(a, 3));
    uint32_t usages = 0;
    Failure err;
    if (!parseUsages(argOr(a, 4), usages, err)) return rejected(err);
    if (!(base->usages & kDeriveKey))
        return rejected("InvalidAccessError", "deriveKey: key does not have 'deriveKey' usage");
    double bits = 0;
    if (!derivedKeyLength(argOr(a, 2), derivedAlgo, bits, err)) return rejected(err);
    Bytes raw;
    if (!deriveBitsCore(argOr(a, 0), algo, *base, bits, raw, err)) return rejected(err);
    CryptoKeyData* key = buildKey(derivedAlgo, std::move(raw), extractable, usages, err);
    if (!key) return rejected(err);
    return resolved(makeCryptoKey(key));
}

// wrapKey(format, key, wrappingKey, wrapAlgorithm)
Value subtleWrapKey(Value, std::span<const Value> a)
{
    if (!ev::isString(argOr(a, 0))) return rejected(nullptr, "wrapKey: format must be a string");
    const std::string format = ev::toUtf8(a[0]);
    CryptoKeyData* key = keyOf(argOr(a, 1));
    CryptoKeyData* wrapping = keyOf(argOr(a, 2));
    if (!key || !wrapping) return rejected(nullptr, "wrapKey: key and wrappingKey must be CryptoKeys");
    Algorithm algo;
    if (!parseAlgorithm(argOr(a, 3), algo)) return rejected(nullptr, "wrapKey: invalid wrapAlgorithm");
    if (!(wrapping->usages & kWrapKey))
        return rejected("InvalidAccessError", "wrapKey: wrappingKey does not have 'wrapKey' usage");
    Failure err;
    Bytes plain, out;
    if (!exportBytes(format, *key, plain, err)) return rejected(err);
    if (algo.name == "AES-KW") {
        if (algo.name != wrapping->name) return rejected("InvalidAccessError", "wrapKey: algorithm does not match the key");
        if (!aesKwWrap(wrapping->raw, plain, out))
            return rejected("OperationError", "wrapKey: AES-KW needs key data that is a multiple of 8 bytes (16+)");
        return resolvedBytes(out);
    }
    if (!runCipher(Dir::Encrypt, argOr(a, 3), algo, *wrapping, plain, out, err)) return rejected(err);
    return resolvedBytes(out);
}

// unwrapKey(format, wrappedKey, unwrappingKey, unwrapAlgorithm,
//           unwrappedKeyAlgorithm, extractable, keyUsages)
Value subtleUnwrapKey(Value, std::span<const Value> a)
{
    if (!ev::isString(argOr(a, 0))) return rejected(nullptr, "unwrapKey: format must be a string");
    const std::string format = ev::toUtf8(a[0]);
    Bytes wrapped;
    if (!getBytes(argOr(a, 1), wrapped)) return rejected(nullptr, "unwrapKey: wrappedKey must be a BufferSource");
    CryptoKeyData* unwrapping = keyOf(argOr(a, 2));
    if (!unwrapping) return rejected(nullptr, "unwrapKey: unwrappingKey is not a CryptoKey");
    Algorithm algo, keyAlgo;
    if (!parseAlgorithm(argOr(a, 3), algo)) return rejected(nullptr, "unwrapKey: invalid unwrapAlgorithm");
    if (!parseAlgorithm(argOr(a, 4), keyAlgo)) return rejected(nullptr, "unwrapKey: invalid unwrappedKeyAlgorithm");
    const bool extractable = ev::toBool(argOr(a, 5));
    uint32_t usages = 0;
    Failure err;
    if (!parseUsages(argOr(a, 6), usages, err)) return rejected(err);
    if (!(unwrapping->usages & kUnwrapKey))
        return rejected("InvalidAccessError", "unwrapKey: key does not have 'unwrapKey' usage");

    Bytes plain;
    if (algo.name == "AES-KW") {
        if (algo.name != unwrapping->name) return rejected("InvalidAccessError", "unwrapKey: algorithm does not match the key");
        if (!aesKwUnwrap(unwrapping->raw, wrapped, plain))
            return rejected("OperationError", "unwrapKey: AES-KW integrity check failed");
    } else if (!runCipher(Dir::Decrypt, argOr(a, 3), algo, *unwrapping, wrapped, plain, err)) {
        return rejected(err);
    }

    CryptoKeyData* key = nullptr;
    if (format == "raw") {
        key = buildKey(keyAlgo, std::move(plain), extractable, usages, err);
    } else if (format == "jwk") {
        ev::GlobalValue json = ev::globalValue("JSON");
        if (!json.found) return rejected("OperationError", "unwrapKey: JSON is unavailable");
        ev::Persistent jsonNs{json.value};
        ev::Persistent parse{ev::getProperty(jsonNs.get(), "parse")};
        ev::Persistent text{ev::fromUtf8(std::string_view(reinterpret_cast<const char*>(plain.data()), plain.size()))};
        const Value args[1] = {text.get()};
        ev::CallResult r = ev::call(parse.get(), jsonNs.get(), std::span<const Value>(args, 1));
        if (r.thrown || !ev::isObject(r.value)) return rejected("DataError", "unwrapKey: unwrapped data is not a JWK");
        key = importJwk(r.value, keyAlgo, extractable, usages, err);
    } else {
        return rejected("NotSupportedError", "unwrapKey: unsupported format '" + format + "'");
    }
    if (!key) return rejected(err);
    return resolved(makeCryptoKey(key));
}

} // namespace

void installSubtleCrypto()
{
    // CryptoKey and SubtleCrypto are interface objects only: neither is
    // constructible from script, but both are globals (the manifest names
    // them) and instanceof works against them.
    g_cryptoKeyClass.install("CryptoKey", 0, nullptr);
    g_subtleClass.install("SubtleCrypto", 0, nullptr, [](ObjectBuilder& proto) {
        proto.def("digest", 2, subtleDigest);
        proto.def("importKey", 5, subtleImportKey);
        proto.def("exportKey", 2, subtleExportKey);
        proto.def("generateKey", 3, subtleGenerateKey);
        proto.def("sign", 3, subtleSign);
        proto.def("verify", 4, subtleVerify);
        proto.def("encrypt", 3, subtleEncrypt);
        proto.def("decrypt", 3, subtleDecrypt);
        proto.def("deriveBits", 3, subtleDeriveBits);
        proto.def("deriveKey", 5, subtleDeriveKey);
        proto.def("wrapKey", 4, subtleWrapKey);
        proto.def("unwrapKey", 7, subtleUnwrapKey);
    });

    ev::Persistent subtle{g_subtleClass.make(nullptr, [](void*) {})};
    ev::Persistent crypto{ev::getGlobal("crypto")};
    if (!ev::isObject(crypto.get())) {
        crypto.set(ev::createObject());
        ev::setGlobalValue("crypto", crypto.get());
    }
    crypto.set(ev::setProperty(crypto.get(), "subtle", subtle.get()));
}

} // namespace brokit::api
