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
//
// Kernel implementations live in the broimage sibling library so brolm,
// brodiffusion, and bro's own decode paths can share them. This file is the
// JS-glue layer: it unmarshals TypedArrays / param objects and dispatches to
// `broimage::*`. The non-Float32 reduce / lookup paths stay inline here —
// broimage's surface is Float32 (the dominant case, FastNoise output); the
// generic-scalar fallback is brokit-specific and rarely hit.

#include "api/api.h"
#include "runtime/runtime.h"

#include "broimage/kernels.h"
#include "broimage/geometric.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace brokit::api {

// ---------------------------------------------------------------------------
// Buffer extraction helpers
// ---------------------------------------------------------------------------

struct TypedArrayView {
    uint8_t* data;
    size_t   byte_len;
    size_t   bpe;
};

static bool unpack_typed_array(JSContext* ctx, JSValueConst val,
                               const char* name, TypedArrayView* out)
{
    size_t byte_offset = 0, byte_len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, val, &byte_offset, &byte_len, &bpe);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "%s must be a TypedArray", name);
        return false;
    }
    size_t ab_len = 0;
    uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_len, buf);
    JS_FreeValue(ctx, buf);
    if (!ab_ptr) {
        JS_ThrowTypeError(ctx, "%s has detached or invalid buffer", name);
        return false;
    }
    out->data = ab_ptr + byte_offset;
    out->byte_len = byte_len;
    out->bpe = bpe;
    return true;
}

static bool get_prop_f64(JSContext* ctx, JSValueConst obj, const char* key,
                         double* out, double def_val)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        *out = def_val;
        return true;
    }
    int rc = JS_ToFloat64(ctx, out, v);
    JS_FreeValue(ctx, v);
    return rc == 0;
}

static bool get_prop_i32(JSContext* ctx, JSValueConst obj, const char* key,
                         int32_t* out, int32_t def_val)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        *out = def_val;
        return true;
    }
    int rc = JS_ToInt32(ctx, out, v);
    JS_FreeValue(ctx, v);
    return rc == 0;
}

static bool get_prop_str(JSContext* ctx, JSValueConst obj, const char* key,
                         std::string* out)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        out->clear();
        return true;
    }
    const char* cstr = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!cstr) return false;
    *out = cstr;
    JS_FreeCString(ctx, cstr);
    return true;
}

// Read a scalar element as float, dispatching on bpe + signedness. Used by
// the generic-dtype lookup / reduce fallbacks. Float32 hot paths bypass this
// and call broimage directly.
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

static bool probe_scalar_kind(JSContext* ctx, JSValueConst val, size_t bpe,
                              ScalarKind* out)
{
    JSValue ctor = JS_GetPropertyStr(ctx, val, "constructor");
    if (JS_IsException(ctor)) return false;
    JSValue name_v = JS_GetPropertyStr(ctx, ctor, "name");
    JS_FreeValue(ctx, ctor);
    const char* name = JS_ToCString(ctx, name_v);
    JS_FreeValue(ctx, name_v);
    if (!name) return false;
    std::string n(name);
    JS_FreeCString(ctx, name);

    out->is_float = (n == "Float32Array" || n == "Float64Array");
    out->is_signed = (n == "Int8Array" || n == "Int16Array" || n == "Int32Array");
    (void)bpe;
    return true;
}

// ---------------------------------------------------------------------------
// gradient(stops, n=256) -> Uint8Array (RGBA8, 4*n bytes)
// ---------------------------------------------------------------------------

static JSValue image_gradient(JSContext* ctx, JSValueConst /*this_val*/,
                              int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsArray(argv[0]))
        return JS_ThrowTypeError(ctx, "gradient(stops, n=256): stops must be an array");

    int32_t n = 256;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt32(ctx, &n, argv[1])) return JS_EXCEPTION;
    }
    if (n < 2) return JS_ThrowRangeError(ctx, "gradient: n must be >= 2");

    JSValue len_v = JS_GetPropertyStr(ctx, argv[0], "length");
    uint32_t stop_count = 0;
    if (JS_ToUint32(ctx, &stop_count, len_v)) {
        JS_FreeValue(ctx, len_v);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, len_v);
    if (stop_count < 2) return JS_ThrowTypeError(ctx, "gradient: need at least 2 stops");

    std::vector<broimage::GradientStop> stops(stop_count);
    for (uint32_t i = 0; i < stop_count; i++) {
        JSValue s = JS_GetPropertyUint32(ctx, argv[0], i);
        if (!JS_IsArray(s)) {
            JS_FreeValue(ctx, s);
            return JS_ThrowTypeError(ctx, "gradient: stop %u must be an array", i);
        }
        JSValue lv = JS_GetPropertyStr(ctx, s, "length");
        uint32_t slen = 0;
        JS_ToUint32(ctx, &slen, lv);
        JS_FreeValue(ctx, lv);
        if (slen < 4) {
            JS_FreeValue(ctx, s);
            return JS_ThrowTypeError(ctx, "gradient: stop %u must be [t, r, g, b, a?]", i);
        }
        double t = 0, r = 0, g = 0, b = 0;
        // a defaults to -1 (sentinel for "fully opaque" inside broimage::gradient).
        double a = -1;
        JSValue tv = JS_GetPropertyUint32(ctx, s, 0); JS_ToFloat64(ctx, &t, tv); JS_FreeValue(ctx, tv);
        JSValue rv = JS_GetPropertyUint32(ctx, s, 1); JS_ToFloat64(ctx, &r, rv); JS_FreeValue(ctx, rv);
        JSValue gv = JS_GetPropertyUint32(ctx, s, 2); JS_ToFloat64(ctx, &g, gv); JS_FreeValue(ctx, gv);
        JSValue bv = JS_GetPropertyUint32(ctx, s, 3); JS_ToFloat64(ctx, &b, bv); JS_FreeValue(ctx, bv);
        if (slen >= 5) {
            JSValue av = JS_GetPropertyUint32(ctx, s, 4); JS_ToFloat64(ctx, &a, av); JS_FreeValue(ctx, av);
        }
        JS_FreeValue(ctx, s);
        stops[i] = { (float)t, (float)r, (float)g, (float)b, (float)a };
    }

    std::vector<uint8_t> lut;
    broimage::gradient(stops.data(), static_cast<int>(stop_count), n, lut);

    JSValue ab = JS_NewArrayBufferCopy(ctx, lut.data(), lut.size());
    if (JS_IsException(ab)) return ab;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue u8ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
    JS_FreeValue(ctx, global);
    JSValueConst args[1] = { ab };
    JSValue arr = JS_CallConstructor(ctx, u8ctor, 1, args);
    JS_FreeValue(ctx, u8ctor);
    JS_FreeValue(ctx, ab);
    return arr;
}

// ---------------------------------------------------------------------------
// alloc(w, h, channels, dtype='float32') -> TypedArray
// ---------------------------------------------------------------------------

static JSValue image_alloc(JSContext* ctx, JSValueConst /*this_val*/,
                           int argc, JSValueConst* argv)
{
    if (argc < 3) return JS_ThrowTypeError(ctx, "alloc(w, h, channels, dtype='float32')");
    int32_t w, h, channels;
    if (JS_ToInt32(ctx, &w, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &channels, argv[2])) return JS_EXCEPTION;
    if (w <= 0 || h <= 0 || channels <= 0)
        return JS_ThrowRangeError(ctx, "alloc: dimensions must be positive");

    std::string dtype = "float32";
    if (argc >= 4 && !JS_IsUndefined(argv[3])) {
        const char* s = JS_ToCString(ctx, argv[3]);
        if (!s) return JS_EXCEPTION;
        dtype = s;
        JS_FreeCString(ctx, s);
    }

    size_t count = (size_t)w * (size_t)h * (size_t)channels;
    const char* ctor_name = nullptr;
    if (dtype == "float32")      { ctor_name = "Float32Array";      }
    else if (dtype == "float64") { ctor_name = "Float64Array";      }
    else if (dtype == "uint8")   { ctor_name = "Uint8Array";        }
    else if (dtype == "uint8c")  { ctor_name = "Uint8ClampedArray"; }
    else if (dtype == "int16")   { ctor_name = "Int16Array";        }
    else if (dtype == "int32")   { ctor_name = "Int32Array";        }
    else if (dtype == "uint16")  { ctor_name = "Uint16Array";       }
    else if (dtype == "uint32")  { ctor_name = "Uint32Array";       }
    else return JS_ThrowTypeError(ctx, "alloc: unknown dtype '%s'", dtype.c_str());

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, ctor_name);
    JS_FreeValue(ctx, global);
    JSValue len = JS_NewInt64(ctx, (int64_t)count);
    JSValueConst args[1] = { len };
    JSValue arr = JS_CallConstructor(ctx, ctor, 1, args);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, len);
    return arr;
}

// ---------------------------------------------------------------------------
// lookup(dst, src, lut, {lo, hi, edge?}) — colormap workhorse
// ---------------------------------------------------------------------------

static JSValue image_lookup(JSContext* ctx, JSValueConst /*this_val*/,
                            int argc, JSValueConst* argv)
{
    if (argc < 4) return JS_ThrowTypeError(ctx, "lookup(dst, src, lut, {lo, hi, edge?})");

    TypedArrayView dst, src, lut;
    if (!unpack_typed_array(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpack_typed_array(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (!unpack_typed_array(ctx, argv[2], "lut", &lut)) return JS_EXCEPTION;

    if (dst.bpe != 1) return JS_ThrowTypeError(ctx, "lookup: dst must be Uint8Array/Uint8ClampedArray");
    if (lut.bpe != 1) return JS_ThrowTypeError(ctx, "lookup: lut must be Uint8Array (RGBA8)");
    if (lut.byte_len % 4 != 0) return JS_ThrowTypeError(ctx, "lookup: lut length must be a multiple of 4");
    size_t lut_n = lut.byte_len / 4;
    if (lut_n < 2) return JS_ThrowRangeError(ctx, "lookup: lut must have >= 2 entries");

    size_t n = src.byte_len / src.bpe;
    if (dst.byte_len < n * 4)
        return JS_ThrowRangeError(ctx, "lookup: dst too small (need %zu bytes)", n * 4);

    double lo = 0, hi = 1;
    if (!get_prop_f64(ctx, argv[3], "lo", &lo, 0)) return JS_EXCEPTION;
    if (!get_prop_f64(ctx, argv[3], "hi", &hi, 1)) return JS_EXCEPTION;
    std::string edge;
    if (!get_prop_str(ctx, argv[3], "edge", &edge)) return JS_EXCEPTION;
    const broimage::LookupEdge be =
        (edge == "wrap") ? broimage::LookupEdge::Wrap : broimage::LookupEdge::Clamp;

    ScalarKind kind{};
    if (!probe_scalar_kind(ctx, argv[1], src.bpe, &kind)) return JS_EXCEPTION;

    // Hot path: Float32 src — straight delegation to broimage.
    if (kind.is_float && src.bpe == 4) {
        broimage::lookup_f32(
            reinterpret_cast<const float*>(src.data), static_cast<int>(n),
            lut.data, static_cast<int>(lut_n),
            dst.data,
            static_cast<float>(lo), static_cast<float>(hi),
            be);
        return JS_UNDEFINED;
    }

    // General path: any scalar dtype. broimage's lookup_f32 is Float32-only;
    // converting through a temp buffer would be wasteful for what is already a
    // rare fallback (callers driving FastNoise output use Float32). Keep the
    // dispatch loop inline; it mirrors broimage::lookup_f32 verbatim.
    const float lo_f = (float)lo;
    const float inv_span = (hi > lo) ? (1.0f / (float)(hi - lo)) : 0.0f;
    const float idx_max = (float)(lut_n - 1);
    const bool wrap = (be == broimage::LookupEdge::Wrap);

    const uint8_t* sp = src.data;
    uint8_t* dp = dst.data;
    for (size_t i = 0; i < n; i++) {
        float v = read_scalar(sp + i * src.bpe, src.bpe, kind.is_float, kind.is_signed);
        float t = (v - lo_f) * inv_span;
        float fi = t * idx_max;
        int idx;
        if (wrap) {
            float lf = (float)lut_n;
            fi = std::fmod(fi, lf);
            if (fi < 0) fi += lf;
            idx = (int)fi;
            if (idx >= (int)lut_n) idx = (int)lut_n - 1;
        } else {
            if (fi < 0) fi = 0;
            if (fi > idx_max) fi = idx_max;
            idx = (int)fi;
        }
        const uint8_t* lp = lut.data + idx * 4;
        dp[i * 4 + 0] = lp[0];
        dp[i * 4 + 1] = lp[1];
        dp[i * 4 + 2] = lp[2];
        dp[i * 4 + 3] = lp[3];
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// reduce(src, op, params?) -> scalar | object
// ---------------------------------------------------------------------------

static JSValue image_reduce(JSContext* ctx, JSValueConst /*this_val*/,
                            int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "reduce(src, op, params?)");

    TypedArrayView src;
    if (!unpack_typed_array(ctx, argv[0], "src", &src)) return JS_EXCEPTION;
    const char* op_c = JS_ToCString(ctx, argv[1]);
    if (!op_c) return JS_EXCEPTION;
    std::string op(op_c);
    JS_FreeCString(ctx, op_c);

    ScalarKind kind{};
    if (!probe_scalar_kind(ctx, argv[0], src.bpe, &kind)) return JS_EXCEPTION;

    size_t n = src.byte_len / src.bpe;
    if (n == 0) return JS_ThrowRangeError(ctx, "reduce: src is empty");

    int32_t stride = 1;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        if (!get_prop_i32(ctx, argv[2], "stride", &stride, 1)) return JS_EXCEPTION;
        if (stride < 1) return JS_ThrowRangeError(ctx, "reduce: stride must be >= 1");
    }
    const size_t step = (size_t)stride;
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
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "min", JS_NewFloat64(ctx, mn));
        JS_SetPropertyStr(ctx, obj, "max", JS_NewFloat64(ctx, mx));
        return obj;
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
            r = (op == "mean") ? (sum / (double)count) : sum;
        }
        return JS_NewFloat64(ctx, r);
    }
    if (op == "histogram") {
        if (argc < 3 || !JS_IsObject(argv[2]))
            return JS_ThrowTypeError(ctx, "reduce histogram requires {bins, lo, hi}");
        int32_t bins = 256;
        double lo = 0, hi = 1;
        if (!get_prop_i32(ctx, argv[2], "bins", &bins, 256)) return JS_EXCEPTION;
        if (!get_prop_f64(ctx, argv[2], "lo", &lo, 0)) return JS_EXCEPTION;
        if (!get_prop_f64(ctx, argv[2], "hi", &hi, 1)) return JS_EXCEPTION;
        if (bins < 1) return JS_ThrowRangeError(ctx, "histogram: bins must be >= 1");
        if (hi <= lo) return JS_ThrowRangeError(ctx, "histogram: hi must be > lo");

        std::vector<uint32_t> counts((size_t)bins, 0);
        if (f32_path) {
            broimage::reduce_histogram_f32(
                reinterpret_cast<const float*>(src.data), static_cast<int>(n),
                bins, (float)lo, (float)hi, counts.data(), stride);
        } else {
            const float lo_f = (float)lo;
            const float inv_span = 1.0f / (float)(hi - lo);
            for (size_t i = 0; i < n; i += step) {
                float v = read_at(i);
                float t = (v - lo_f) * inv_span;
                int idx = (int)(t * (float)bins);
                if (idx < 0 || idx >= bins) continue;
                counts[idx]++;
            }
        }
        JSValue ab = JS_NewArrayBufferCopy(ctx,
            reinterpret_cast<const uint8_t*>(counts.data()),
            counts.size() * sizeof(uint32_t));
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ctor = JS_GetPropertyStr(ctx, global, "Uint32Array");
        JS_FreeValue(ctx, global);
        JSValueConst args[1] = { ab };
        JSValue arr = JS_CallConstructor(ctx, ctor, 1, args);
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, ab);
        return arr;
    }
    return JS_ThrowTypeError(ctx, "reduce: unknown op '%s'", op.c_str());
}

// ---------------------------------------------------------------------------
// map(dst, src, opSpec) — element-wise unary (Float32)
// ---------------------------------------------------------------------------

static JSValue image_map(JSContext* ctx, JSValueConst /*this_val*/,
                         int argc, JSValueConst* argv)
{
    if (argc < 3) return JS_ThrowTypeError(ctx, "map(dst, src, opSpec)");
    TypedArrayView dst, src;
    if (!unpack_typed_array(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpack_typed_array(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4)
        return JS_ThrowTypeError(ctx, "map: dst and src must be Float32Array");
    int n = static_cast<int>(src.byte_len / 4);
    if (dst.byte_len < (size_t)n * 4)
        return JS_ThrowRangeError(ctx, "map: dst too small");

    std::string op;
    if (!get_prop_str(ctx, argv[2], "op", &op)) return JS_EXCEPTION;
    if (op.empty()) return JS_ThrowTypeError(ctx, "map: opSpec.op required");

    const float* sp = reinterpret_cast<const float*>(src.data);
    float* dp = reinterpret_cast<float*>(dst.data);

    if (op == "affine") {
        double a = 1, b = 0;
        if (!get_prop_f64(ctx, argv[2], "a", &a, 1)) return JS_EXCEPTION;
        if (!get_prop_f64(ctx, argv[2], "b", &b, 0)) return JS_EXCEPTION;
        JSValue cv = JS_GetPropertyStr(ctx, argv[2], "clamp");
        bool clamp = JS_IsArray(cv);
        float clo = 0, chi = 0;
        if (clamp) {
            JSValue v0 = JS_GetPropertyUint32(ctx, cv, 0);
            JSValue v1 = JS_GetPropertyUint32(ctx, cv, 1);
            double d0 = 0, d1 = 0;
            JS_ToFloat64(ctx, &d0, v0);
            JS_ToFloat64(ctx, &d1, v1);
            JS_FreeValue(ctx, v0);
            JS_FreeValue(ctx, v1);
            clo = (float)d0; chi = (float)d1;
        }
        JS_FreeValue(ctx, cv);
        if (clamp) {
            broimage::map_affine_clamp_f32(sp, dp, n, (float)a, (float)b, clo, chi);
        } else {
            broimage::map_affine_f32(sp, dp, n, (float)a, (float)b);
        }
        return JS_UNDEFINED;
    }
    if (op == "abs")  { broimage::map_abs_f32(sp, dp, n);  return JS_UNDEFINED; }
    if (op == "log")  { broimage::map_log_f32(sp, dp, n);  return JS_UNDEFINED; }
    if (op == "sqrt") { broimage::map_sqrt_f32(sp, dp, n); return JS_UNDEFINED; }
    if (op == "exp")  { broimage::map_exp_f32(sp, dp, n);  return JS_UNDEFINED; }
    if (op == "pow") {
        double e = 1;
        if (!get_prop_f64(ctx, argv[2], "exp", &e, 1)) return JS_EXCEPTION;
        broimage::map_pow_f32(sp, dp, n, (float)e);
        return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx, "map: unknown op '%s'", op.c_str());
}

// ---------------------------------------------------------------------------
// combine(dst, a, b, opSpec) — element-wise binary (Float32)
// ---------------------------------------------------------------------------

static JSValue image_combine(JSContext* ctx, JSValueConst /*this_val*/,
                             int argc, JSValueConst* argv)
{
    if (argc < 4) return JS_ThrowTypeError(ctx, "combine(dst, a, b, opSpec)");
    TypedArrayView dst, va, vb;
    if (!unpack_typed_array(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpack_typed_array(ctx, argv[1], "a",   &va))  return JS_EXCEPTION;
    if (!unpack_typed_array(ctx, argv[2], "b",   &vb))  return JS_EXCEPTION;
    if (dst.bpe != 4 || va.bpe != 4 || vb.bpe != 4)
        return JS_ThrowTypeError(ctx, "combine: all buffers must be Float32Array");
    int n = static_cast<int>(va.byte_len / 4);
    if (vb.byte_len / 4 != (size_t)n)
        return JS_ThrowRangeError(ctx, "combine: a and b must have equal length");
    if (dst.byte_len < (size_t)n * 4)
        return JS_ThrowRangeError(ctx, "combine: dst too small");

    std::string op;
    if (!get_prop_str(ctx, argv[3], "op", &op)) return JS_EXCEPTION;
    if (op.empty()) return JS_ThrowTypeError(ctx, "combine: opSpec.op required");

    const float* ap = reinterpret_cast<const float*>(va.data);
    const float* bp = reinterpret_cast<const float*>(vb.data);
    float* dp = reinterpret_cast<float*>(dst.data);

    if (op == "add") { broimage::combine_add_f32(ap, bp, dp, n); return JS_UNDEFINED; }
    if (op == "sub") { broimage::combine_sub_f32(ap, bp, dp, n); return JS_UNDEFINED; }
    if (op == "mul") { broimage::combine_mul_f32(ap, bp, dp, n); return JS_UNDEFINED; }
    if (op == "min") { broimage::combine_min_f32(ap, bp, dp, n); return JS_UNDEFINED; }
    if (op == "max") { broimage::combine_max_f32(ap, bp, dp, n); return JS_UNDEFINED; }
    if (op == "lerp") {
        double t = 0;
        if (!get_prop_f64(ctx, argv[3], "t", &t, 0)) return JS_EXCEPTION;
        broimage::combine_lerp_f32(ap, bp, dp, n, (float)t);
        return JS_UNDEFINED;
    }
    if (op == "wsum") {
        double wa = 1, wb = 1;
        if (!get_prop_f64(ctx, argv[3], "wa", &wa, 1)) return JS_EXCEPTION;
        if (!get_prop_f64(ctx, argv[3], "wb", &wb, 1)) return JS_EXCEPTION;
        broimage::combine_wsum_f32(ap, bp, dp, n, (float)wa, (float)wb);
        return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx, "combine: unknown op '%s'", op.c_str());
}

// ---------------------------------------------------------------------------
// stencil(dst, src, kernel, params) — single-channel 2D conv (Float32)
// ---------------------------------------------------------------------------

static JSValue image_stencil(JSContext* ctx, JSValueConst /*this_val*/,
                             int argc, JSValueConst* argv)
{
    if (argc < 4) return JS_ThrowTypeError(ctx, "stencil(dst, src, kernel, params)");
    TypedArrayView dst, src;
    if (!unpack_typed_array(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpack_typed_array(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4)
        return JS_ThrowTypeError(ctx, "stencil: dst and src must be Float32Array");

    JSValue kdata_v = JS_GetPropertyStr(ctx, argv[2], "data");
    int32_t kw = 0, kh = 0;
    if (!get_prop_i32(ctx, argv[2], "w", &kw, 0)) { JS_FreeValue(ctx, kdata_v); return JS_EXCEPTION; }
    if (!get_prop_i32(ctx, argv[2], "h", &kh, 0)) { JS_FreeValue(ctx, kdata_v); return JS_EXCEPTION; }
    TypedArrayView kdata;
    if (!unpack_typed_array(ctx, kdata_v, "kernel.data", &kdata)) {
        JS_FreeValue(ctx, kdata_v);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, kdata_v);
    if (kdata.bpe != 4) return JS_ThrowTypeError(ctx, "stencil: kernel.data must be Float32Array");
    if (kw <= 0 || kh <= 0) return JS_ThrowRangeError(ctx, "stencil: kernel w/h must be positive");
    if ((kw & 1) == 0 || (kh & 1) == 0)
        return JS_ThrowRangeError(ctx, "stencil: kernel w/h must be odd");
    if (kdata.byte_len < (size_t)kw * (size_t)kh * 4)
        return JS_ThrowRangeError(ctx, "stencil: kernel.data too small for w*h");

    int32_t srcW = 0, srcH = 0;
    if (!get_prop_i32(ctx, argv[3], "srcW", &srcW, 0)) return JS_EXCEPTION;
    if (!get_prop_i32(ctx, argv[3], "srcH", &srcH, 0)) return JS_EXCEPTION;
    if (srcW <= 0 || srcH <= 0)
        return JS_ThrowRangeError(ctx, "stencil: srcW/srcH required and positive");
    if (src.byte_len < (size_t)srcW * (size_t)srcH * 4)
        return JS_ThrowRangeError(ctx, "stencil: src too small for srcW*srcH");
    if (dst.byte_len < (size_t)srcW * (size_t)srcH * 4)
        return JS_ThrowRangeError(ctx, "stencil: dst too small for srcW*srcH");

    std::string edge;
    if (!get_prop_str(ctx, argv[3], "edge", &edge)) return JS_EXCEPTION;
    broimage::StencilEdge be = broimage::StencilEdge::Clamp;
    if (edge == "wrap") be = broimage::StencilEdge::Wrap;
    else if (edge == "zero") be = broimage::StencilEdge::Zero;
    else if (!edge.empty() && edge != "clamp")
        return JS_ThrowTypeError(ctx, "stencil: edge must be 'clamp'|'wrap'|'zero'");

    double divisor = 1, bias = 0;
    if (!get_prop_f64(ctx, argv[3], "divisor", &divisor, 1)) return JS_EXCEPTION;
    if (!get_prop_f64(ctx, argv[3], "bias",    &bias,    0)) return JS_EXCEPTION;

    broimage::stencil_f32(
        reinterpret_cast<const float*>(src.data),
        reinterpret_cast<float*>(dst.data),
        srcW, srcH,
        reinterpret_cast<const float*>(kdata.data), kw, kh,
        (float)divisor, (float)bias, be);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// resample(dst, src, params) — nearest | bilinear (Float32 HWC)
// ---------------------------------------------------------------------------

static JSValue image_resample(JSContext* ctx, JSValueConst /*this_val*/,
                              int argc, JSValueConst* argv)
{
    if (argc < 3) return JS_ThrowTypeError(ctx, "resample(dst, src, params)");
    TypedArrayView dst, src;
    if (!unpack_typed_array(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpack_typed_array(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4)
        return JS_ThrowTypeError(ctx, "resample: dst and src must be Float32Array");

    int32_t srcW, srcH, dstW, dstH, channels;
    if (!get_prop_i32(ctx, argv[2], "srcW", &srcW, 0)) return JS_EXCEPTION;
    if (!get_prop_i32(ctx, argv[2], "srcH", &srcH, 0)) return JS_EXCEPTION;
    if (!get_prop_i32(ctx, argv[2], "dstW", &dstW, 0)) return JS_EXCEPTION;
    if (!get_prop_i32(ctx, argv[2], "dstH", &dstH, 0)) return JS_EXCEPTION;
    if (!get_prop_i32(ctx, argv[2], "channels", &channels, 1)) return JS_EXCEPTION;
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0 || channels <= 0)
        return JS_ThrowRangeError(ctx, "resample: all dims/channels must be positive");
    size_t need_src = (size_t)srcW * srcH * channels * 4;
    size_t need_dst = (size_t)dstW * dstH * channels * 4;
    if (src.byte_len < need_src) return JS_ThrowRangeError(ctx, "resample: src too small");
    if (dst.byte_len < need_dst) return JS_ThrowRangeError(ctx, "resample: dst too small");

    std::string filter;
    if (!get_prop_str(ctx, argv[2], "filter", &filter)) return JS_EXCEPTION;
    if (filter.empty()) filter = "bilinear";

    broimage::Filter f;
    if      (filter == "nearest")  f = broimage::Filter::Nearest;
    else if (filter == "bilinear") f = broimage::Filter::Bilinear;
    else return JS_ThrowTypeError(ctx, "resample: filter must be 'nearest'|'bilinear'");

    broimage::resample_f32(
        reinterpret_cast<const float*>(src.data), srcW, srcH,
        reinterpret_cast<float*>(dst.data),       dstW, dstH,
        channels, f);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installImage(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue bro = JS_GetPropertyStr(ctx, global, "bro");
    if (!JS_IsObject(bro)) {
        JS_FreeValue(ctx, bro);
        bro = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, bro));
    }

    JSValue image = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, image, "gradient",
        JS_NewCFunction(ctx, image_gradient, "gradient", 2));
    JS_SetPropertyStr(ctx, image, "alloc",
        JS_NewCFunction(ctx, image_alloc, "alloc", 4));
    JS_SetPropertyStr(ctx, image, "lookup",
        JS_NewCFunction(ctx, image_lookup, "lookup", 4));
    JS_SetPropertyStr(ctx, image, "reduce",
        JS_NewCFunction(ctx, image_reduce, "reduce", 3));
    JS_SetPropertyStr(ctx, image, "map",
        JS_NewCFunction(ctx, image_map, "map", 3));
    JS_SetPropertyStr(ctx, image, "combine",
        JS_NewCFunction(ctx, image_combine, "combine", 4));
    JS_SetPropertyStr(ctx, image, "stencil",
        JS_NewCFunction(ctx, image_stencil, "stencil", 4));
    JS_SetPropertyStr(ctx, image, "resample",
        JS_NewCFunction(ctx, image_resample, "resample", 3));

    JS_SetPropertyStr(ctx, bro, "image", image);
    JS_FreeValue(ctx, bro);
    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
