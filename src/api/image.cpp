// bro.image — composable typed-array kernels.
//
// Six verbs (reduce, map, combine, lookup, stencil, resample) + builders
// (gradient, alloc) operate on whole TypedArray buffers from C++. JS stays
// out of the per-pixel inner loop. Op behavior is a struct (enum + numeric
// params), never a JS callback. Caller-supplied dst buffers — kernels never
// allocate output.
//
// Mounted as `bro.image.*`. The `bro` global is reused if present (bro host
// already created it), or created here for standalone brokit.

#include "api/api.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"

#include "broimage/kernels.h"
#include "broimage/geometric.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <span>

namespace brokit::api {

// ---------------------------------------------------------------------------
// Buffer extraction helpers
// ---------------------------------------------------------------------------

// `data` points into the MOVING heap (embed.h's pointer contract): it is dead
// after any allocating embed call, and reading an options object with
// getProperty allocates. Each kernel therefore calls refresh() on its views
// after its last option read and before it touches the bytes.
struct TypedArrayView {
    uint8_t* data = nullptr;
    size_t   byte_len = 0;
    size_t   bpe = 0;
    ev::Persistent root;

    void refresh() { data = ev::typedArrayInfo(root.get()).data; }
};

static bool unpack_typed_array(bronze::Value val, const char* name, TypedArrayView* out)
{
    auto info = ev::typedArrayInfo(val);
    if (!info.data) {
        ev::throwTypeError(std::string(name) + " must be a TypedArray");
        return false;
    }
    out->data = info.data;
    out->byte_len = info.byteLength;
    out->bpe = info.bytesPerElement;
    out->root.set(val);  // may allocate a root slot; data is refreshed before use
    return true;
}

static bool get_prop_f64(bronze::Value obj, const char* key, double* out, double def_val)
{
    bronze::Value v = ev::getProperty(obj, key);
    if (ev::isUndefined(v) || ev::isNull(v)) {
        *out = def_val;
        return true;
    }
    if (ev::isDouble(v)) {
        *out = ev::toDouble(v);
        return true;
    }
    return false;
}

static bool get_prop_i32(bronze::Value obj, const char* key, int32_t* out, int32_t def_val)
{
    bronze::Value v = ev::getProperty(obj, key);
    if (ev::isUndefined(v) || ev::isNull(v)) {
        *out = def_val;
        return true;
    }
    if (ev::isDouble(v)) {
        *out = static_cast<int32_t>(ev::toDouble(v));
        return true;
    }
    return false;
}

static bool get_prop_str(bronze::Value obj, const char* key, std::string* out)
{
    bronze::Value v = ev::getProperty(obj, key);
    if (ev::isUndefined(v) || ev::isNull(v)) {
        out->clear();
        return true;
    }
    if (ev::isString(v)) {
        *out = ev::toUtf8(v);
        return true;
    }
    return false;
}

static inline float read_scalar(const uint8_t* p, size_t bpe, bool is_float, bool is_signed) {
    if (is_float) {
        if (bpe == 4) return *reinterpret_cast<const float*>(p);
        return static_cast<float>(*reinterpret_cast<const double*>(p));
    }
    if (is_signed) {
        if (bpe == 1) return static_cast<float>(*reinterpret_cast<const int8_t*>(p));
        if (bpe == 2) return static_cast<float>(*reinterpret_cast<const int16_t*>(p));
        return static_cast<float>(*reinterpret_cast<const int32_t*>(p));
    }
    if (bpe == 1) return static_cast<float>(*p);
    if (bpe == 2) return static_cast<float>(*reinterpret_cast<const uint16_t*>(p));
    return static_cast<float>(*reinterpret_cast<const uint32_t*>(p));
}

struct ScalarKind { bool is_float; bool is_signed; };

static bool probe_scalar_kind(bronze::Value val, ScalarKind* out)
{
    auto info = ev::typedArrayInfo(val);
    if (!info.data) return false;
    out->is_float = (info.elementKind == elements::Float32 || info.elementKind == elements::Float64);
    out->is_signed = (info.elementKind == elements::Int8 || info.elementKind == elements::Int16 || info.elementKind == elements::Int32);
    return true;
}

// ---------------------------------------------------------------------------
// gradient(stops, n=256) -> Uint8Array (RGBA8, 4*n bytes)
// ---------------------------------------------------------------------------

static bronze::Value image_gradient(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty() || !ev::isObject(args[0]))
        return ev::throwTypeError("gradient(stops, n=256): stops must be an array");

    ArgReader reader(args);
    int32_t n = reader.getInt(1, 256);
    if (n < 2) return ev::throwRangeError("gradient: n must be >= 2");

    // args[0] is read from the rooted span each time and each stop is rooted:
    // the "length" reads may allocate (property-key interning).
    bronze::Value lenVal = ev::getProperty(args[0], "length");
    if (!ev::isDouble(lenVal)) return ev::throwTypeError("gradient: stops must be an array");
    uint32_t stop_count = static_cast<uint32_t>(ev::toDouble(lenVal));
    if (stop_count < 2) return ev::throwTypeError("gradient: need at least 2 stops");

    std::vector<broimage::GradientStop> stops(stop_count);
    for (uint32_t i = 0; i < stop_count; i++) {
        ev::Persistent stop{ev::getElement(args[0], i)};
        const bronze::Value s = stop.get();
        if (!ev::isObject(s))
            return ev::throwTypeError("gradient: stop must be an array");

        bronze::Value lv = ev::getProperty(stop.get(), "length");
        uint32_t slen = ev::isDouble(lv) ? static_cast<uint32_t>(ev::toDouble(lv)) : 0;
        if (slen < 4)
            return ev::throwTypeError("gradient: stop must be [t, r, g, b, a?]");

        double t = ev::toDouble(ev::getElement(stop.get(), 0));
        double r = ev::toDouble(ev::getElement(stop.get(), 1));
        double g = ev::toDouble(ev::getElement(stop.get(), 2));
        double b = ev::toDouble(ev::getElement(stop.get(), 3));
        double a = -1.0;
        if (slen >= 5) {
            a = ev::toDouble(ev::getElement(stop.get(), 4));
        }

        stops[i].t = static_cast<float>(t);
        stops[i].r = static_cast<float>(r);
        stops[i].g = static_cast<float>(g);
        stops[i].b = static_cast<float>(b);
        stops[i].a = static_cast<float>(a);
    }

    std::vector<uint8_t> outBytes;
    broimage::gradient(stops.data(), static_cast<int>(stop_count), n, outBytes);

    bronze::Value ab = ev::createArrayBuffer(
        std::span<const uint8_t>(outBytes.data(), outBytes.size()));
    return ev::createTypedArrayView(elements::Uint8, ab, 0, static_cast<uint32_t>(outBytes.size()));
}

// ---------------------------------------------------------------------------
// alloc(w, h, channels, dtype='float32') -> TypedArray
// ---------------------------------------------------------------------------

static bronze::Value image_alloc(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 3) return ev::throwTypeError("alloc(w, h, channels, dtype='float32')");
    ArgReader reader(args);
    int32_t w = reader.getInt(0, 0);
    int32_t h = reader.getInt(1, 0);
    int32_t channels = reader.getInt(2, 0);
    if (w <= 0 || h <= 0 || channels <= 0)
        return ev::throwRangeError("alloc: dimensions must be positive");

    std::string dtype = reader.getString(3, "float32");

    size_t count = static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(channels);
    bronze::ElementKind kind;
    if (dtype == "float32")      { kind = elements::Float32;      }
    else if (dtype == "float64") { kind = elements::Float64;      }
    else if (dtype == "uint8")   { kind = elements::Uint8;        }
    else if (dtype == "uint8c")  { kind = elements::Uint8Clamped; }
    else if (dtype == "int16")   { kind = elements::Int16;        }
    else if (dtype == "int32")   { kind = elements::Int32;        }
    else if (dtype == "uint16")  { kind = elements::Uint16;       }
    else if (dtype == "uint32")  { kind = elements::Uint32;       }
    else return ev::throwTypeError("alloc: unknown dtype '" + dtype + "'");

    return ev::createTypedArray(kind, static_cast<uint32_t>(count));
}

// ---------------------------------------------------------------------------
// lookup(dst, src, lut, {lo, hi, edge?})
// ---------------------------------------------------------------------------

static bronze::Value image_lookup(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 4) return ev::throwTypeError("lookup(dst, src, lut, {lo, hi, edge?})");

    TypedArrayView dst, src, lut;
    if (!unpack_typed_array(args[0], "dst", &dst)) return ev::undefined();
    if (!unpack_typed_array(args[1], "src", &src)) return ev::undefined();
    if (!unpack_typed_array(args[2], "lut", &lut)) return ev::undefined();

    if (dst.bpe != 1) return ev::throwTypeError("lookup: dst must be Uint8Array/Uint8ClampedArray");
    if (lut.bpe != 1) return ev::throwTypeError("lookup: lut must be Uint8Array (RGBA8)");
    if (lut.byte_len % 4 != 0) return ev::throwTypeError("lookup: lut length must be a multiple of 4");
    size_t lut_n = lut.byte_len / 4;
    if (lut_n < 2) return ev::throwRangeError("lookup: lut must have >= 2 entries");

    size_t n = src.byte_len / src.bpe;
    if (dst.byte_len < n * 4)
        return ev::throwRangeError("lookup: dst too small");

    double lo = 0, hi = 1;
    if (!get_prop_f64(args[3], "lo", &lo, 0)) return ev::undefined();
    if (!get_prop_f64(args[3], "hi", &hi, 1)) return ev::undefined();
    std::string edge;
    if (!get_prop_str(args[3], "edge", &edge)) return ev::undefined();
    const broimage::LookupEdge be =
        (edge == "wrap") ? broimage::LookupEdge::Wrap : broimage::LookupEdge::Clamp;

    ScalarKind kind{};
    if (!probe_scalar_kind(args[1], &kind)) return ev::undefined();

    dst.refresh(); src.refresh(); lut.refresh();
    if (kind.is_float && src.bpe == 4) {
        broimage::lookup_f32(
            reinterpret_cast<const float*>(src.data), static_cast<int>(n),
            lut.data, static_cast<int>(lut_n),
            dst.data,
            static_cast<float>(lo), static_cast<float>(hi),
            be);
        return ev::undefined();
    }

    const float lo_f = static_cast<float>(lo);
    const float inv_span = (hi > lo) ? (1.0f / static_cast<float>(hi - lo)) : 0.0f;
    const float idx_max = static_cast<float>(lut_n - 1);
    const bool wrap = (be == broimage::LookupEdge::Wrap);

    const uint8_t* sp = src.data;
    uint8_t* dp = dst.data;
    for (size_t i = 0; i < n; i++) {
        float v = read_scalar(sp + i * src.bpe, src.bpe, kind.is_float, kind.is_signed);
        float t = (v - lo_f) * inv_span;
        float fi = t * idx_max;
        int idx;
        if (wrap) {
            float lf = static_cast<float>(lut_n);
            fi = std::fmod(fi, lf);
            if (fi < 0) fi += lf;
            idx = static_cast<int>(fi);
            if (idx >= static_cast<int>(lut_n)) idx = static_cast<int>(lut_n) - 1;
        } else {
            if (fi < 0) fi = 0;
            if (fi > idx_max) fi = idx_max;
            idx = static_cast<int>(fi);
        }
        const uint8_t* lp = lut.data + idx * 4;
        dp[i * 4 + 0] = lp[0];
        dp[i * 4 + 1] = lp[1];
        dp[i * 4 + 2] = lp[2];
        dp[i * 4 + 3] = lp[3];
    }
    return ev::undefined();
}

// ---------------------------------------------------------------------------
// reduce(src, op, params?) -> scalar | object
// ---------------------------------------------------------------------------

static bronze::Value image_reduce(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 2) return ev::throwTypeError("reduce(src, op, params?)");

    TypedArrayView src;
    if (!unpack_typed_array(args[0], "src", &src)) return ev::undefined();
    if (!ev::isString(args[1])) return ev::throwTypeError("reduce: op must be a string");
    std::string op = ev::toUtf8(args[1]);

    ScalarKind kind{};
    if (!probe_scalar_kind(args[0], &kind)) return ev::undefined();

    size_t n = src.byte_len / src.bpe;
    if (n == 0) return ev::throwRangeError("reduce: src is empty");

    int32_t stride = 1;
    if (args.size() >= 3 && ev::isObject(args[2])) {
        if (!get_prop_i32(args[2], "stride", &stride, 1)) return ev::undefined();
        if (stride < 1) return ev::throwRangeError("reduce: stride must be >= 1");
    }
    src.refresh();
    const size_t step = static_cast<size_t>(stride);
    const bool f32_path = (kind.is_float && src.bpe == 4);
    auto read_at = [&](size_t i) -> float {
        return read_scalar(src.data + i * src.bpe, src.bpe, kind.is_float, kind.is_signed);
    };

    if (op == "minmax") {
        float mn, mx;
        if (f32_path) {
            broimage::MinMax mm = broimage::reduce_minmax_f32(
                reinterpret_cast<const float*>(src.data),
                static_cast<int>(n), stride);
            mn = mm.min; mx = mm.max;
        } else {
            mn = read_at(0); mx = mn;
            for (size_t i = step; i < n; i += step) {
                float v = read_at(i);
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
        }
        ObjectBuilder obj;
        obj.set("min", static_cast<double>(mn));
        obj.set("max", static_cast<double>(mx));
        return obj.build();
    }
    if (op == "sum" || op == "mean") {
        double r;
        if (f32_path) {
            r = (op == "sum")
                ? broimage::reduce_sum_f32(reinterpret_cast<const float*>(src.data),
                                           static_cast<int>(n), stride)
                : broimage::reduce_mean_f32(reinterpret_cast<const float*>(src.data),
                                            static_cast<int>(n), stride);
        } else {
            double sum = 0;
            size_t count = 0;
            for (size_t i = 0; i < n; i += step) { sum += read_at(i); count++; }
            r = (op == "mean") ? (sum / static_cast<double>(count)) : sum;
        }
        return ev::fromDouble(r);
    }
    if (op == "histogram") {
        if (args.size() < 3 || !ev::isObject(args[2]))
            return ev::throwTypeError("reduce histogram requires {bins, lo, hi}");
        int32_t bins = 256;
        double lo = 0, hi = 1;
        if (!get_prop_i32(args[2], "bins", &bins, 256)) return ev::undefined();
        if (!get_prop_f64(args[2], "lo", &lo, 0)) return ev::undefined();
        if (!get_prop_f64(args[2], "hi", &hi, 1)) return ev::undefined();
        if (bins < 1) return ev::throwRangeError("histogram: bins must be >= 1");
        if (hi <= lo) return ev::throwRangeError("histogram: hi must be > lo");
        src.refresh();

        std::vector<uint32_t> counts(static_cast<size_t>(bins), 0);
        if (f32_path) {
            broimage::reduce_histogram_f32(
                reinterpret_cast<const float*>(src.data), static_cast<int>(n),
                bins, static_cast<float>(lo), static_cast<float>(hi), counts.data(), stride);
        } else {
            const float lo_f = static_cast<float>(lo);
            const float inv_span = 1.0f / static_cast<float>(hi - lo);
            for (size_t i = 0; i < n; i += step) {
                float v = read_at(i);
                float t = (v - lo_f) * inv_span;
                int idx = static_cast<int>(t * static_cast<float>(bins));
                if (idx < 0 || idx >= bins) continue;
                counts[idx]++;
            }
        }
        bronze::Value ab = ev::createArrayBuffer(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(counts.data()), counts.size() * sizeof(uint32_t)));
        return ev::createTypedArrayView(elements::Uint32, ab, 0, static_cast<uint32_t>(counts.size()));
    }
    return ev::throwTypeError("reduce: unknown op '" + op + "'");
}

// ---------------------------------------------------------------------------
// map(dst, src, opSpec) — element-wise unary (Float32)
// ---------------------------------------------------------------------------

static bronze::Value image_map(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 3) return ev::throwTypeError("map(dst, src, opSpec)");
    TypedArrayView dst, src;
    if (!unpack_typed_array(args[0], "dst", &dst)) return ev::undefined();
    if (!unpack_typed_array(args[1], "src", &src)) return ev::undefined();
    if (dst.bpe != 4 || src.bpe != 4)
        return ev::throwTypeError("map: dst and src must be Float32Array");
    int n = static_cast<int>(src.byte_len / 4);
    if (dst.byte_len < static_cast<size_t>(n) * 4)
        return ev::throwRangeError("map: dst too small");

    std::string op;
    if (!get_prop_str(args[2], "op", &op)) return ev::undefined();
    if (op.empty()) return ev::throwTypeError("map: opSpec.op required");

    const float* sp = nullptr;
    float* dp = nullptr;
    auto bytes = [&] {
        src.refresh(); dst.refresh();
        sp = reinterpret_cast<const float*>(src.data);
        dp = reinterpret_cast<float*>(dst.data);
    };
    bytes();

    if (op == "affine") {
        double a = 1, b = 0;
        if (!get_prop_f64(args[2], "a", &a, 1)) return ev::undefined();
        if (!get_prop_f64(args[2], "b", &b, 0)) return ev::undefined();
        bronze::Value cv = ev::getProperty(args[2], "clamp");
        bool clamp = ev::isObject(cv);
        float clo = 0, chi = 0;
        if (clamp) {
            clo = static_cast<float>(ev::toDouble(ev::getElement(cv, 0)));
            chi = static_cast<float>(ev::toDouble(ev::getElement(cv, 1)));
            bytes();
            broimage::map_affine_clamp_f32(sp, dp, n, static_cast<float>(a), static_cast<float>(b), clo, chi);
        } else {
            bytes();
            broimage::map_affine_f32(sp, dp, n, static_cast<float>(a), static_cast<float>(b));
        }
        return ev::undefined();
    }
    if (op == "abs")  { broimage::map_abs_f32(sp, dp, n);  return ev::undefined(); }
    if (op == "log")  { broimage::map_log_f32(sp, dp, n);  return ev::undefined(); }
    if (op == "sqrt") { broimage::map_sqrt_f32(sp, dp, n); return ev::undefined(); }
    if (op == "exp")  { broimage::map_exp_f32(sp, dp, n);  return ev::undefined(); }
    if (op == "pow") {
        double e = 1;
        if (!get_prop_f64(args[2], "exp", &e, 1)) return ev::undefined();
        bytes();
        broimage::map_pow_f32(sp, dp, n, static_cast<float>(e));
        return ev::undefined();
    }
    return ev::throwTypeError("map: unknown op '" + op + "'");
}

// ---------------------------------------------------------------------------
// combine(dst, a, b, opSpec) — element-wise binary (Float32)
// ---------------------------------------------------------------------------

static bronze::Value image_combine(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 4) return ev::throwTypeError("combine(dst, a, b, opSpec)");
    TypedArrayView dst, va, vb;
    if (!unpack_typed_array(args[0], "dst", &dst)) return ev::undefined();
    if (!unpack_typed_array(args[1], "a",   &va))  return ev::undefined();
    if (!unpack_typed_array(args[2], "b",   &vb))  return ev::undefined();
    if (dst.bpe != 4 || va.bpe != 4 || vb.bpe != 4)
        return ev::throwTypeError("combine: all buffers must be Float32Array");
    int n = static_cast<int>(va.byte_len / 4);
    if (vb.byte_len / 4 != static_cast<size_t>(n))
        return ev::throwRangeError("combine: a and b must have equal length");
    if (dst.byte_len < static_cast<size_t>(n) * 4)
        return ev::throwRangeError("combine: dst too small");

    std::string op;
    if (!get_prop_str(args[3], "op", &op)) return ev::undefined();
    if (op.empty()) return ev::throwTypeError("combine: opSpec.op required");

    const float* ap = nullptr;
    const float* bp = nullptr;
    float* dp = nullptr;
    auto bytes = [&] {
        va.refresh(); vb.refresh(); dst.refresh();
        ap = reinterpret_cast<const float*>(va.data);
        bp = reinterpret_cast<const float*>(vb.data);
        dp = reinterpret_cast<float*>(dst.data);
    };
    bytes();

    if (op == "add") { broimage::combine_add_f32(ap, bp, dp, n); return ev::undefined(); }
    if (op == "sub") { broimage::combine_sub_f32(ap, bp, dp, n); return ev::undefined(); }
    if (op == "mul") { broimage::combine_mul_f32(ap, bp, dp, n); return ev::undefined(); }
    if (op == "min") { broimage::combine_min_f32(ap, bp, dp, n); return ev::undefined(); }
    if (op == "max") { broimage::combine_max_f32(ap, bp, dp, n); return ev::undefined(); }
    if (op == "lerp") {
        double t = 0;
        if (!get_prop_f64(args[3], "t", &t, 0)) return ev::undefined();
        bytes();
        broimage::combine_lerp_f32(ap, bp, dp, n, static_cast<float>(t));
        return ev::undefined();
    }
    if (op == "wsum") {
        double wa = 1, wb = 1;
        if (!get_prop_f64(args[3], "wa", &wa, 1)) return ev::undefined();
        if (!get_prop_f64(args[3], "wb", &wb, 1)) return ev::undefined();
        bytes();
        broimage::combine_wsum_f32(ap, bp, dp, n, static_cast<float>(wa), static_cast<float>(wb));
        return ev::undefined();
    }
    return ev::throwTypeError("combine: unknown op '" + op + "'");
}

// ---------------------------------------------------------------------------
// stencil(dst, src, kernel, params) — single-channel 2D conv (Float32)
// ---------------------------------------------------------------------------

static bronze::Value image_stencil(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 4) return ev::throwTypeError("stencil(dst, src, kernel, params)");
    TypedArrayView dst, src;
    if (!unpack_typed_array(args[0], "dst", &dst)) return ev::undefined();
    if (!unpack_typed_array(args[1], "src", &src)) return ev::undefined();
    if (dst.bpe != 4 || src.bpe != 4)
        return ev::throwTypeError("stencil: dst and src must be Float32Array");

    bronze::Value kdata_v = ev::getProperty(args[2], "data");
    int32_t kw = 0, kh = 0;
    if (!get_prop_i32(args[2], "w", &kw, 0)) return ev::undefined();
    if (!get_prop_i32(args[2], "h", &kh, 0)) return ev::undefined();
    TypedArrayView kdata;
    if (!unpack_typed_array(kdata_v, "kernel.data", &kdata)) return ev::undefined();
    if (kdata.bpe != 4) return ev::throwTypeError("stencil: kernel.data must be Float32Array");
    if (kw <= 0 || kh <= 0) return ev::throwRangeError("stencil: kernel w/h must be positive");
    if ((kw & 1) == 0 || (kh & 1) == 0)
        return ev::throwRangeError("stencil: kernel w/h must be odd");
    if (kdata.byte_len < static_cast<size_t>(kw) * static_cast<size_t>(kh) * 4)
        return ev::throwRangeError("stencil: kernel.data too small for w*h");

    int32_t srcW = 0, srcH = 0;
    if (!get_prop_i32(args[3], "srcW", &srcW, 0)) return ev::undefined();
    if (!get_prop_i32(args[3], "srcH", &srcH, 0)) return ev::undefined();
    if (srcW <= 0 || srcH <= 0)
        return ev::throwRangeError("stencil: srcW/srcH required and positive");
    if (src.byte_len < static_cast<size_t>(srcW) * static_cast<size_t>(srcH) * 4)
        return ev::throwRangeError("stencil: src too small for srcW*srcH");
    if (dst.byte_len < static_cast<size_t>(srcW) * static_cast<size_t>(srcH) * 4)
        return ev::throwRangeError("stencil: dst too small for srcW*srcH");

    std::string edge;
    if (!get_prop_str(args[3], "edge", &edge)) return ev::undefined();
    broimage::StencilEdge be = broimage::StencilEdge::Clamp;
    if (edge == "wrap") be = broimage::StencilEdge::Wrap;
    else if (edge == "zero") be = broimage::StencilEdge::Zero;
    else if (!edge.empty() && edge != "clamp")
        return ev::throwTypeError("stencil: edge must be 'clamp'|'wrap'|'zero'");

    double divisor = 1, bias = 0;
    if (!get_prop_f64(args[3], "divisor", &divisor, 1)) return ev::undefined();
    if (!get_prop_f64(args[3], "bias",    &bias,    0)) return ev::undefined();

    src.refresh(); dst.refresh(); kdata.refresh();
    broimage::stencil_f32(
        reinterpret_cast<const float*>(src.data),
        reinterpret_cast<float*>(dst.data),
        srcW, srcH,
        reinterpret_cast<const float*>(kdata.data), kw, kh,
        static_cast<float>(divisor), static_cast<float>(bias), be);
    return ev::undefined();
}

// ---------------------------------------------------------------------------
// resample(dst, src, params) — nearest | bilinear (Float32 HWC)
// ---------------------------------------------------------------------------

static bronze::Value image_resample(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 3) return ev::throwTypeError("resample(dst, src, params)");
    TypedArrayView dst, src;
    if (!unpack_typed_array(args[0], "dst", &dst)) return ev::undefined();
    if (!unpack_typed_array(args[1], "src", &src)) return ev::undefined();
    if (dst.bpe != 4 || src.bpe != 4)
        return ev::throwTypeError("resample: dst and src must be Float32Array");

    int32_t srcW, srcH, dstW, dstH, channels;
    if (!get_prop_i32(args[2], "srcW", &srcW, 0)) return ev::undefined();
    if (!get_prop_i32(args[2], "srcH", &srcH, 0)) return ev::undefined();
    if (!get_prop_i32(args[2], "dstW", &dstW, 0)) return ev::undefined();
    if (!get_prop_i32(args[2], "dstH", &dstH, 0)) return ev::undefined();
    if (!get_prop_i32(args[2], "channels", &channels, 1)) return ev::undefined();
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0 || channels <= 0)
        return ev::throwRangeError("resample: all dims/channels must be positive");
    size_t need_src = static_cast<size_t>(srcW) * srcH * channels * 4;
    size_t need_dst = static_cast<size_t>(dstW) * dstH * channels * 4;
    if (src.byte_len < need_src) return ev::throwRangeError("resample: src too small");
    if (dst.byte_len < need_dst) return ev::throwRangeError("resample: dst too small");

    std::string filter;
    if (!get_prop_str(args[2], "filter", &filter)) return ev::undefined();
    if (filter.empty()) filter = "bilinear";

    broimage::Filter f;
    if      (filter == "nearest")  f = broimage::Filter::Nearest;
    else if (filter == "bilinear") f = broimage::Filter::Bilinear;
    else return ev::throwTypeError("resample: filter must be 'nearest'|'bilinear'");

    src.refresh(); dst.refresh();
    broimage::resample_f32(
        reinterpret_cast<const float*>(src.data), srcW, srcH,
        reinterpret_cast<float*>(dst.data),       dstW, dstH,
        channels, f);
    return ev::undefined();
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installImage()
{
    ev::Persistent bro{ev::getGlobal("bro")};
    if (!ev::isObject(bro.get())) {
        bro.set(ev::createObject());
        ev::setGlobalValue("bro", bro.get());
    }

    ObjectBuilder img;
    img.def("gradient", 2, image_gradient);
    img.def("alloc", 4, image_alloc);
    img.def("lookup", 4, image_lookup);
    img.def("reduce", 3, image_reduce);
    img.def("map", 3, image_map);
    img.def("combine", 4, image_combine);
    img.def("stencil", 4, image_stencil);
    img.def("resample", 3, image_resample);

    bro.set(ev::setProperty(bro.get(), "image", img.build()));
}

} // namespace brokit::api
