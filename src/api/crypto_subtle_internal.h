#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace brokit::api::subtle_detail {

using Bytes = std::vector<uint8_t>;

enum class Dir { Encrypt, Decrypt };

// The largest key or derived-bits length a script may ask for: 256 MiB, the
// most bronze will hand back in one buffer. Past it the allocation fails (or
// the double-to-size_t cast is undefined) instead of rejecting.
inline constexpr double kMaxKeyBits = 2147483648.0;

bool fillRandom(uint8_t* buf, size_t len);
std::string upper(std::string s);
std::string normalizeName(const std::string& in);
bool isSha(const std::string& n);
bool isAes(const std::string& n);
size_t hashLen(const std::string& h);
size_t hashBlockBits(const std::string& h);

bool digest(const std::string& hash, const uint8_t* data, size_t len, Bytes& out);
bool hmac(const std::string& hash, const Bytes& key, const uint8_t* data, size_t len, Bytes& out);
bool aesGcm(Dir dir, const Bytes& raw, const Bytes& iv, const uint8_t* data, size_t len,
            const Bytes& aad, uint8_t* tag, size_t tagLen, Bytes& out);
bool aesCbc(Dir dir, const Bytes& raw, const Bytes& iv, const uint8_t* data, size_t len, Bytes& out);
bool aesEcb(Dir dir, const Bytes& raw, const uint8_t* data, size_t len, uint8_t* out);
bool pbkdf2(const std::string& hash, const Bytes& password, const Bytes& salt, uint64_t iterations,
            size_t outLen, Bytes& out);
bool hkdf(const std::string& hash, const Bytes& ikm, const Bytes& salt, const Bytes& info, size_t outLen,
          Bytes& out);
bool aesCtr(const Bytes& raw, const Bytes& counter, int lengthBits, const uint8_t* data, size_t len, Bytes& out);
bool aesKwWrap(const Bytes& kek, const Bytes& plain, Bytes& out);
bool aesKwUnwrap(const Bytes& kek, const Bytes& wrapped, Bytes& out);

std::string base64urlEncode(const Bytes& in);
bool base64urlDecode(const std::string& in, Bytes& out);

} // namespace brokit::api::subtle_detail
