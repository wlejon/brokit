// Platform crypto primitives and constructions for crypto.subtle
// (BCrypt on Windows, OpenSSL's libcrypto elsewhere).

#include "api/crypto_subtle_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>
#endif

namespace brokit::api::subtle_detail {

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
// Platform primitives
// ---------------------------------------------------------------------------

#ifdef _WIN32

namespace {

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
bool aesEcbEncrypt(const Bytes& raw, const uint8_t* in, size_t len, uint8_t* out)
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
    if (!aesEcbEncrypt(raw, zero, 16, hBytes)) return false;
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
    if (!aesEcbEncrypt(raw, ctr.data(), ctr.size(), ks.data())) return false;

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

} // namespace

bool digest(const std::string& hash, const uint8_t* data, size_t len, Bytes& out)
{
    return hashOrHmac(hash, nullptr, 0, false, data, len, out);
}

bool hmac(const std::string& hash, const Bytes& key, const uint8_t* data, size_t len, Bytes& out)
{
    return hashOrHmac(hash, key.data(), key.size(), true, data, len, out);
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

namespace {

const EVP_MD* mdFor(const std::string& hash)
{
    if (hash == "SHA-256") return EVP_sha256();
    if (hash == "SHA-384") return EVP_sha384();
    if (hash == "SHA-512") return EVP_sha512();
    if (hash == "SHA-1") return EVP_sha1();
    return nullptr;
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

} // namespace

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

namespace {

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

// AES Key Wrap (RFC 3394), default IV.
const uint8_t kKwIv[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};

void xorT(uint8_t a[8], uint64_t t)
{
    for (int k = 7; k >= 0; --k) { a[k] ^= static_cast<uint8_t>(t & 0xFF); t >>= 8; }
}

} // namespace

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

} // namespace brokit::api::subtle_detail
