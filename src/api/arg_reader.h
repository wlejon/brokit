#pragma once

#include "embed/embed.h"

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace brokit::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

inline double numAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return 0.0;
    Value v = args[i];
    if (ev::isObject(v)) return 0.0;
    double d = ev::toDouble(v);
    return std::isnan(d) ? 0.0 : d;
}

// Script numbers into C++ integers. A double outside the target's range
// (Infinity, 1e300) converted with a plain cast is undefined behaviour, so
// these saturate; NaN is 0.
inline int32_t saturateI32(double d) {
    if (std::isnan(d)) return 0;
    if (d <= static_cast<double>(INT32_MIN)) return INT32_MIN;
    if (d >= static_cast<double>(INT32_MAX)) return INT32_MAX;
    return static_cast<int32_t>(d);
}

// Negatives are 0.
inline uint32_t saturateU32(double d) {
    if (std::isnan(d) || d <= 0) return 0;
    if (d >= static_cast<double>(UINT32_MAX)) return UINT32_MAX;
    return static_cast<uint32_t>(d);
}

inline int64_t saturateI64(double d) {
    if (std::isnan(d)) return 0;
    // 2^63 is exactly representable; every double below it converts.
    if (d <= -9223372036854775808.0) return INT64_MIN;
    if (d >= 9223372036854775808.0) return INT64_MAX;
    return static_cast<int64_t>(d);
}

// Longest array-like a binding walks when the script owns its `length`
// (option lists, key lists, listener lists). Past it the walk would be a hang
// or a huge allocation, not a real list.
constexpr uint32_t kMaxScriptList = 1u << 20;

// ECMAScript ToUint32 / ToInt32: modular, as `x >>> 0` / `x | 0` are, so a
// uint32 seed of 0xFFFFFFFF still reads as -1 rather than colliding with
// every other large value; non-finite is 0.
inline uint32_t toUint32Modular(double d) {
    if (!std::isfinite(d)) return 0;
    double m = std::fmod(std::trunc(d), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return static_cast<uint32_t>(m);
}

inline int32_t toInt32Modular(double d) {
    return static_cast<int32_t>(toUint32Modular(d));
}

inline int32_t i32At(std::span<const Value> args, size_t i) {
    return toInt32Modular(numAt(args, i));
}

inline uint32_t u32At(std::span<const Value> args, size_t i) {
    return toUint32Modular(numAt(args, i));
}

inline int64_t i64At(std::span<const Value> args, size_t i) {
    return saturateI64(numAt(args, i));
}

inline bool boolAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return false;
    return ev::toBool(args[i]);
}

inline std::string strAt(std::span<const Value> args, size_t i) {
    if (i >= args.size() || ev::isUndefined(args[i]) || ev::isNull(args[i]) || ev::isSymbol(args[i])) return "";
    return ev::toUtf8(args[i]);
}

inline Value argAt(std::span<const Value> args, size_t i) {
    return i < args.size() ? args[i] : ev::undefined();
}

inline bool hasArg(std::span<const Value> args, size_t i) {
    return i < args.size() && !ev::isUndefined(args[i]);
}

class ArgReader {
public:
    explicit ArgReader(std::span<const Value> args) : args_(args) {}

    double getDouble(size_t i, double def = 0.0) const {
        return hasArg(args_, i) ? numAt(args_, i) : def;
    }
    int getInt(size_t i, int def = 0) const {
        return hasArg(args_, i) ? i32At(args_, i) : def;
    }
    uint32_t getUint(size_t i, uint32_t def = 0) const {
        return hasArg(args_, i) ? u32At(args_, i) : def;
    }
    bool getBool(size_t i, bool def = false) const {
        return hasArg(args_, i) ? boolAt(args_, i) : def;
    }
    std::string getString(size_t i, const std::string& def = "") const {
        return hasArg(args_, i) ? strAt(args_, i) : def;
    }
    Value get(size_t i) const {
        return argAt(args_, i);
    }
    bool has(size_t i) const {
        return hasArg(args_, i);
    }
    size_t count() const {
        return args_.size();
    }

private:
    std::span<const Value> args_;
};

inline bool bufferBytes(Value v, const uint8_t** outData, size_t* outLen) {
    if (auto info = ev::typedArrayInfo(v)) {
        *outData = info.data;
        *outLen = info.byteLength;
        return true;
    }
    if (auto buf = ev::arrayBufferInfo(v)) {
        *outData = buf.data;
        *outLen = buf.byteLength;
        return true;
    }
    if (ev::isObject(v)) {
        Value inner = ev::getProperty(v, "_u8");
        if (ev::isObject(inner)) {
            if (auto info = ev::typedArrayInfo(inner)) {
                *outData = info.data;
                *outLen = info.byteLength;
                return true;
            }
        }
    }
    return false;
}

inline Value hostArrayOf(size_t count, const std::function<Value(size_t)>& make) {
    ev::GlobalValue arrCtor = ev::globalValue("Array");
    ev::Persistent arr;
    if (arrCtor.found) {
        Value lenV = ev::fromDouble(static_cast<double>(count));
        ev::CallResult r = ev::construct(arrCtor.value, std::span<const Value>(&lenV, 1));
        if (!r.thrown) arr.set(r.value);
    }
    if (ev::isUndefined(arr.get())) {
        arr.set(ev::createObject());
    }
    for (size_t i = 0; i < count; ++i) {
        ev::Persistent item(make(i));
        arr.set(ev::setElement(arr.get(), static_cast<uint32_t>(i), item.get()));
    }
    return arr.get();
}

// `items` must be current at the call (a rooted args span, or values produced
// with no allocation since). They are rooted before the array allocates;
// to COLLECT heap values in a loop, use ArrayBuilder instead.
inline Value hostArrayOf(std::span<const Value> items) {
    std::vector<ev::Persistent> roots;
    roots.reserve(items.size());
    for (Value v : items) roots.emplace_back(v);
    return hostArrayOf(roots.size(), [&](size_t i) { return roots[i].get(); });
}

inline Value hostArrayOf(const std::vector<Value>& items) {
    return hostArrayOf(std::span<const Value>(items.data(), items.size()));
}


template <typename T, typename Convert>
inline bool plainArrayData(Value v, std::vector<T>& storage, Convert convert,
                           const T** outData, size_t* outCount) {
    if (!ev::isObject(v)) return false;
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (ev::isUndefined(lenV) || ev::isObject(lenV)) return false;
    // `length` is the script's: an array-like claiming more than a list's
    // worth is refused rather than sizing the allocation.
    uint32_t n = saturateU32(ev::toDouble(lenV));
    if (n > kMaxScriptList * 64u) return false;
    storage.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        Value e = ev::getElement(root.get(), i);
        double d = ev::isObject(e) ? 0.0 : ev::toDouble(e);
        storage[i] = std::isnan(d) ? T{} : convert(d);
    }
    *outData = storage.data();
    *outCount = n;
    return true;
}

} // namespace brokit::api
