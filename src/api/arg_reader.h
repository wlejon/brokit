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

inline int32_t i32At(std::span<const Value> args, size_t i) {
    return static_cast<int32_t>(static_cast<int64_t>(numAt(args, i)));
}

inline uint32_t u32At(std::span<const Value> args, size_t i) {
    return static_cast<uint32_t>(static_cast<int64_t>(numAt(args, i)));
}

inline int64_t i64At(std::span<const Value> args, size_t i) {
    return static_cast<int64_t>(numAt(args, i));
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

inline Value hostArrayOf(std::span<const Value> items) {
    return hostArrayOf(items.size(), [&](size_t i) { return items[i]; });
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
    uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
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
