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
#include "runtime/runtime.h"

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

// Result of unpacking a TypedArray into a raw pointer + element count.
// `bytes_per_elem` reports the JS view's element size so callers can validate
// dtype expectations (e.g. require Float32Array → bpe == 4).
struct TypedArrayView {
    uint8_t* data;        // pointer to the first element (bytes)
    size_t   byte_len;    // length of the view in bytes
    size_t   bpe;         // bytes per element of the view
};

// Unpack a JS TypedArray into a raw byte view. On failure throws via JS_Throw*
// and returns an empty view (data == nullptr).
//
// `name` is used in error messages so callers don't need to wrap.
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

// Read a property as a double, defaulting if missing/undefined. Throws on
// non-numeric values that fail to coerce.
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

// Read a property as an int32, defaulting if missing/undefined.
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

// Read a string property. Returns empty string if missing. Caller owns the
// returned string (RAII via std::string).
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

// Read scalar TypedArray as float (handles Float32, Float64, all int types).
// Returns by-index reader closure parameters: caller dispatches on bpe + signed
// flag. To keep the API small, we just convert to a float vector once when
// needed. For the hot paths (lookup, reduce on minmax, stencil, resample) we
// specialize on Float32 because that's the FastNoise2 output and the dominant
// case.
//
// For now: return raw view + a cheap accessor that converts at read time. The
// caller can specialize for Float32 when bpe == 4 to skip conversion.

// Read a scalar element as float, dispatching on bpe + signedness.
// Used by ops that accept any scalar TypedArray. For Float32 hot paths, callers
// should reinterpret directly to skip the dispatch.
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

// Identify TypedArray dtype via JS_GetTypedArrayType (QuickJS extension) or by
// inspecting the constructor name. QuickJS exposes JS_GetTypedArrayType in
// recent versions; if not available, we fall back to constructor name.
//
// For our purposes we only need: is_float (Float32/Float64), is_signed (Int*).
// We probe via the constructor name on the JS value — works on any QuickJS.
struct ScalarKind { bool is_float; bool is_signed; };

static bool probe_scalar_kind(JSContext* ctx, JSValueConst val, size_t bpe,
                              ScalarKind* out)
{
    // Get constructor.name
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
    // Sanity check bpe matches the type
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

    // Decode stops
    struct Stop { float t; float r, g, b, a; };
    std::vector<Stop> stops(stop_count);
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
        double t = 0, r = 0, g = 0, b = 0, a = 255;
        JSValue tv = JS_GetPropertyUint32(ctx, s, 0); JS_ToFloat64(ctx, &t, tv); JS_FreeValue(ctx, tv);
        JSValue rv = JS_GetPropertyUint32(ctx, s, 1); JS_ToFloat64(ctx, &r, rv); JS_FreeValue(ctx, rv);
        JSValue gv = JS_GetPropertyUint32(ctx, s, 2); JS_ToFloat64(ctx, &g, gv); JS_FreeValue(ctx, gv);
        JSValue bv = JS_GetPropertyUint32(ctx, s, 3); JS_ToFloat64(ctx, &b, bv); JS_FreeValue(ctx, bv);
        if (slen >= 5) {
            JSValue av = JS_GetPropertyUint32(ctx, s, 4); JS_ToFloat64(ctx, &a, av); JS_FreeValue(ctx, av);
        }
        JS_FreeValue(ctx, s);
        stops[i] = {(float)t, (float)r, (float)g, (float)b, (float)a};
    }

    // Build LUT
    std::vector<uint8_t> lut(static_cast<size_t>(n) * 4);
    uint32_t cur = 0;
    for (int32_t i = 0; i < n; i++) {
        float t = (n == 1) ? 0.0f : static_cast<float>(i) / static_cast<float>(n - 1);
        // advance cur until stops[cur+1].t >= t
        while (cur + 1 < stop_count - 1 && stops[cur + 1].t < t) cur++;
        const Stop& s0 = stops[cur];
        const Stop& s1 = stops[cur + 1];
        float span = s1.t - s0.t;
        float u = (span > 1e-9f) ? (t - s0.t) / span : 0.0f;
        u = std::clamp(u, 0.0f, 1.0f);
        // Outside the stops range: clamp to endpoints
        if (t <= stops.front().t) { u = 0.0f; }
        if (t >= stops.back().t) {
            // pick last segment, u=1
            const Stop& a = stops[stop_count - 2];
            const Stop& b = stops[stop_count - 1];
            float r = b.r, g = b.g, bv = b.b, av = b.a; (void)a;
            lut[i * 4 + 0] = (uint8_t)std::clamp(r, 0.0f, 255.0f);
            lut[i * 4 + 1] = (uint8_t)std::clamp(g, 0.0f, 255.0f);
            lut[i * 4 + 2] = (uint8_t)std::clamp(bv, 0.0f, 255.0f);
            lut[i * 4 + 3] = (uint8_t)std::clamp(av, 0.0f, 255.0f);
            continue;
        }
        float r = s0.r + (s1.r - s0.r) * u;
        float g = s0.g + (s1.g - s0.g) * u;
        float b = s0.b + (s1.b - s0.b) * u;
        float a = s0.a + (s1.a - s0.a) * u;
        lut[i * 4 + 0] = (uint8_t)std::clamp(r, 0.0f, 255.0f);
        lut[i * 4 + 1] = (uint8_t)std::clamp(g, 0.0f, 255.0f);
        lut[i * 4 + 2] = (uint8_t)std::clamp(b, 0.0f, 255.0f);
        lut[i * 4 + 3] = (uint8_t)std::clamp(a, 0.0f, 255.0f);
    }

    // Wrap as Uint8Array
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
    size_t bpe = 0;
    if (dtype == "float32")      { ctor_name = "Float32Array";      bpe = 4; }
    else if (dtype == "float64") { ctor_name = "Float64Array";      bpe = 8; }
    else if (dtype == "uint8")   { ctor_name = "Uint8Array";        bpe = 1; }
    else if (dtype == "uint8c")  { ctor_name = "Uint8ClampedArray"; bpe = 1; }
    else if (dtype == "int16")   { ctor_name = "Int16Array";        bpe = 2; }
    else if (dtype == "int32")   { ctor_name = "Int32Array";        bpe = 4; }
    else if (dtype == "uint16")  { ctor_name = "Uint16Array";       bpe = 2; }
    else if (dtype == "uint32")  { ctor_name = "Uint32Array";       bpe = 4; }
    else return JS_ThrowTypeError(ctx, "alloc: unknown dtype '%s'", dtype.c_str());

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, ctor_name);
    JS_FreeValue(ctx, global);
    JSValue len = JS_NewInt64(ctx, (int64_t)count);
    JSValueConst args[1] = { len };
    JSValue arr = JS_CallConstructor(ctx, ctor, 1, args);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, len);
    (void)bpe;
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
    bool wrap = (edge == "wrap");

    float lo_f = (float)lo;
    float inv_span = (hi > lo) ? (1.0f / (float)(hi - lo)) : 0.0f;
    float idx_max = (float)(lut_n - 1);

    ScalarKind kind{};
    if (!probe_scalar_kind(ctx, argv[1], src.bpe, &kind)) return JS_EXCEPTION;

    // Hot path: Float32 src
    if (kind.is_float && src.bpe == 4) {
        const float* sp = reinterpret_cast<const float*>(src.data);
        uint8_t* dp = dst.data;
        for (size_t i = 0; i < n; i++) {
            float t = (sp[i] - lo_f) * inv_span;
            float fi = t * idx_max;
            int idx;
            if (wrap) {
                // wrap into [0, lut_n)
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

    // General path: any scalar dtype
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

    // Optional stride for subsampling. stride=8 visits every 8th element.
    // Cheap range/sum estimate when full accuracy isn't needed (e.g. driving
    // a smoothed EMA from a million-pixel float field).
    int32_t stride = 1;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        if (!get_prop_i32(ctx, argv[2], "stride", &stride, 1)) return JS_EXCEPTION;
        if (stride < 1) return JS_ThrowRangeError(ctx, "reduce: stride must be >= 1");
    }
    const size_t step = (size_t)stride;

    auto read_at = [&](size_t i) -> float {
        return read_scalar(src.data + i * src.bpe, src.bpe, kind.is_float, kind.is_signed);
    };

    if (op == "minmax") {
        float mn = read_at(0), mx = mn;
        if (kind.is_float && src.bpe == 4) {
            const float* sp = reinterpret_cast<const float*>(src.data);
            for (size_t i = step; i < n; i += step) {
                float v = sp[i];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
        } else {
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
        double sum = 0;
        size_t count = 0;
        if (kind.is_float && src.bpe == 4) {
            const float* sp = reinterpret_cast<const float*>(src.data);
            for (size_t i = 0; i < n; i += step) { sum += sp[i]; count++; }
        } else {
            for (size_t i = 0; i < n; i += step) { sum += read_at(i); count++; }
        }
        if (op == "mean") sum /= (double)count;
        return JS_NewFloat64(ctx, sum);
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
        float lo_f = (float)lo;
        float inv_span = 1.0f / (float)(hi - lo);
        for (size_t i = 0; i < n; i += step) {
            float v = read_at(i);
            float t = (v - lo_f) * inv_span;
            int idx = (int)(t * (float)bins);
            if (idx < 0 || idx >= bins) continue;
            counts[idx]++;
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
// map(dst, src, opSpec) — element-wise unary
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
    size_t n = src.byte_len / 4;
    if (dst.byte_len < n * 4)
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
        // optional clamp = [lo, hi]
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
        float af = (float)a, bf = (float)b;
        if (clamp) {
            for (size_t i = 0; i < n; i++) {
                float v = sp[i] * af + bf;
                if (v < clo) v = clo;
                if (v > chi) v = chi;
                dp[i] = v;
            }
        } else {
            for (size_t i = 0; i < n; i++) dp[i] = sp[i] * af + bf;
        }
        return JS_UNDEFINED;
    }
    if (op == "abs")  { for (size_t i = 0; i < n; i++) dp[i] = std::fabs(sp[i]);  return JS_UNDEFINED; }
    if (op == "log")  { for (size_t i = 0; i < n; i++) dp[i] = std::log(sp[i]);   return JS_UNDEFINED; }
    if (op == "sqrt") { for (size_t i = 0; i < n; i++) dp[i] = std::sqrt(sp[i]);  return JS_UNDEFINED; }
    if (op == "exp")  { for (size_t i = 0; i < n; i++) dp[i] = std::exp(sp[i]);   return JS_UNDEFINED; }
    if (op == "pow") {
        double e = 1;
        if (!get_prop_f64(ctx, argv[2], "exp", &e, 1)) return JS_EXCEPTION;
        float ef = (float)e;
        for (size_t i = 0; i < n; i++) dp[i] = std::pow(sp[i], ef);
        return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx, "map: unknown op '%s'", op.c_str());
}

// ---------------------------------------------------------------------------
// combine(dst, a, b, opSpec) — element-wise binary
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
    size_t n = va.byte_len / 4;
    if (vb.byte_len / 4 != n)
        return JS_ThrowRangeError(ctx, "combine: a and b must have equal length");
    if (dst.byte_len < n * 4)
        return JS_ThrowRangeError(ctx, "combine: dst too small");

    std::string op;
    if (!get_prop_str(ctx, argv[3], "op", &op)) return JS_EXCEPTION;
    if (op.empty()) return JS_ThrowTypeError(ctx, "combine: opSpec.op required");

    const float* ap = reinterpret_cast<const float*>(va.data);
    const float* bp = reinterpret_cast<const float*>(vb.data);
    float* dp = reinterpret_cast<float*>(dst.data);

    if (op == "add") { for (size_t i = 0; i < n; i++) dp[i] = ap[i] + bp[i]; return JS_UNDEFINED; }
    if (op == "sub") { for (size_t i = 0; i < n; i++) dp[i] = ap[i] - bp[i]; return JS_UNDEFINED; }
    if (op == "mul") { for (size_t i = 0; i < n; i++) dp[i] = ap[i] * bp[i]; return JS_UNDEFINED; }
    if (op == "min") { for (size_t i = 0; i < n; i++) dp[i] = std::min(ap[i], bp[i]); return JS_UNDEFINED; }
    if (op == "max") { for (size_t i = 0; i < n; i++) dp[i] = std::max(ap[i], bp[i]); return JS_UNDEFINED; }
    if (op == "lerp") {
        double t = 0;
        if (!get_prop_f64(ctx, argv[3], "t", &t, 0)) return JS_EXCEPTION;
        float tf = (float)t, omt = 1.0f - tf;
        for (size_t i = 0; i < n; i++) dp[i] = ap[i] * omt + bp[i] * tf;
        return JS_UNDEFINED;
    }
    if (op == "wsum") {
        double wa = 1, wb = 1;
        if (!get_prop_f64(ctx, argv[3], "wa", &wa, 1)) return JS_EXCEPTION;
        if (!get_prop_f64(ctx, argv[3], "wb", &wb, 1)) return JS_EXCEPTION;
        float waf = (float)wa, wbf = (float)wb;
        for (size_t i = 0; i < n; i++) dp[i] = ap[i] * waf + bp[i] * wbf;
        return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx, "combine: unknown op '%s'", op.c_str());
}

// ---------------------------------------------------------------------------
// stencil(dst, src, kernel, params)
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

    // kernel = { data: Float32Array, w, h }
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
    enum { CLAMP, WRAP, ZERO } mode = CLAMP;
    if (edge == "wrap") mode = WRAP;
    else if (edge == "zero") mode = ZERO;
    else if (!edge.empty() && edge != "clamp")
        return JS_ThrowTypeError(ctx, "stencil: edge must be 'clamp'|'wrap'|'zero'");

    double divisor = 1, bias = 0;
    if (!get_prop_f64(ctx, argv[3], "divisor", &divisor, 1)) return JS_EXCEPTION;
    if (!get_prop_f64(ctx, argv[3], "bias",    &bias,    0)) return JS_EXCEPTION;
    float inv_div = (divisor != 0) ? (float)(1.0 / divisor) : 1.0f;
    float bias_f = (float)bias;

    const float* sp = reinterpret_cast<const float*>(src.data);
    const float* kp = reinterpret_cast<const float*>(kdata.data);
    float* dp = reinterpret_cast<float*>(dst.data);
    int hkw = kw / 2, hkh = kh / 2;

    for (int32_t y = 0; y < srcH; y++) {
        for (int32_t x = 0; x < srcW; x++) {
            float acc = 0;
            for (int32_t ky = 0; ky < kh; ky++) {
                int32_t sy = y + ky - hkh;
                int32_t syy;
                bool zero_row = false;
                if (mode == CLAMP) {
                    syy = std::clamp(sy, 0, srcH - 1);
                } else if (mode == WRAP) {
                    syy = ((sy % srcH) + srcH) % srcH;
                } else {
                    if (sy < 0 || sy >= srcH) { zero_row = true; syy = 0; }
                    else syy = sy;
                }
                for (int32_t kx = 0; kx < kw; kx++) {
                    int32_t sx = x + kx - hkw;
                    int32_t sxx;
                    bool zero = zero_row;
                    if (!zero) {
                        if (mode == CLAMP) {
                            sxx = std::clamp(sx, 0, srcW - 1);
                        } else if (mode == WRAP) {
                            sxx = ((sx % srcW) + srcW) % srcW;
                        } else {
                            if (sx < 0 || sx >= srcW) { zero = true; sxx = 0; }
                            else sxx = sx;
                        }
                    } else { sxx = 0; }
                    float v = zero ? 0.0f : sp[syy * srcW + sxx];
                    acc += v * kp[ky * kw + kx];
                }
            }
            dp[y * srcW + x] = acc * inv_div + bias_f;
        }
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// resample(dst, src, params) — nearest | bilinear
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

    const float* sp = reinterpret_cast<const float*>(src.data);
    float* dp = reinterpret_cast<float*>(dst.data);

    if (filter == "nearest") {
        for (int32_t y = 0; y < dstH; y++) {
            int32_t sy = (int32_t)((y + 0.5f) * srcH / dstH);
            if (sy >= srcH) sy = srcH - 1;
            for (int32_t x = 0; x < dstW; x++) {
                int32_t sx = (int32_t)((x + 0.5f) * srcW / dstW);
                if (sx >= srcW) sx = srcW - 1;
                const float* sptr = sp + (sy * srcW + sx) * channels;
                float* dptr = dp + (y * dstW + x) * channels;
                for (int32_t c = 0; c < channels; c++) dptr[c] = sptr[c];
            }
        }
        return JS_UNDEFINED;
    }
    if (filter == "bilinear") {
        float xs = (float)srcW / (float)dstW;
        float ys = (float)srcH / (float)dstH;
        for (int32_t y = 0; y < dstH; y++) {
            float fy = (y + 0.5f) * ys - 0.5f;
            int32_t y0 = (int32_t)std::floor(fy);
            int32_t y1 = y0 + 1;
            float ty = fy - y0;
            if (y0 < 0) { y0 = 0; ty = 0; }
            if (y1 >= srcH) { y1 = srcH - 1; ty = 1; }
            for (int32_t x = 0; x < dstW; x++) {
                float fx = (x + 0.5f) * xs - 0.5f;
                int32_t x0 = (int32_t)std::floor(fx);
                int32_t x1 = x0 + 1;
                float tx = fx - x0;
                if (x0 < 0) { x0 = 0; tx = 0; }
                if (x1 >= srcW) { x1 = srcW - 1; tx = 1; }
                const float* p00 = sp + (y0 * srcW + x0) * channels;
                const float* p01 = sp + (y0 * srcW + x1) * channels;
                const float* p10 = sp + (y1 * srcW + x0) * channels;
                const float* p11 = sp + (y1 * srcW + x1) * channels;
                float* dptr = dp + (y * dstW + x) * channels;
                float omtx = 1.0f - tx, omty = 1.0f - ty;
                for (int32_t c = 0; c < channels; c++) {
                    float v = (p00[c] * omtx + p01[c] * tx) * omty
                            + (p10[c] * omtx + p11[c] * tx) * ty;
                    dptr[c] = v;
                }
            }
        }
        return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx, "resample: filter must be 'nearest'|'bilinear'");
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installImage(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    // Reuse `bro` if present (bro host already created it), else create it.
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
