#include "api/api.h"
#include "runtime/runtime.h"

#include <FastNoise/FastNoise.h>
#include <FastNoise/Metadata.h>
#include <vector>
#include <cstdint>
#include <cstring>

namespace brokit::api {

static thread_local JSClassID noise_class_id = 0;

struct NoiseWrapper {
    FastNoise::SmartNode<> node;
};

static void noise_finalizer(JSRuntime*, JSValue val)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque(val, noise_class_id));
    delete w;
}

static JSClassDef noise_class_def = { "FastNoise", noise_finalizer };

static JSValue make_float32_array(JSContext* ctx, const float* data, size_t count)
{
    size_t byte_len = count * sizeof(float);
    JSValue ab = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(data), byte_len);
    if (JS_IsException(ab)) return ab;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Float32Array");
    JSValue result = JS_CallConstructor(ctx, ctor, 1, &ab);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, ab);
    return result;
}

static NoiseWrapper* get_noise(JSContext* ctx, JSValueConst this_val)
{
    return static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
}

// Resolve a Float32Array argument to a raw pointer and element count. Returns
// false with a pending exception on failure. `name` names the argument in the
// error, since the position-array entry points take four of these and a bare
// "must be a Float32Array" would not say which.
static bool resolve_f32(JSContext* ctx, JSValueConst v, const char* name,
                        float** out, size_t* count)
{
    size_t byte_offset = 0, byte_len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &byte_offset, &byte_len, &bpe);
    if (JS_IsException(buf)) {
        // Clear the pending TypedArray exception so we can throw our own.
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "%s must be a Float32Array", name);
        return false;
    }
    if (bpe != sizeof(float)) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "%s must be a Float32Array", name);
        return false;
    }
    size_t ab_len = 0;
    uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_len, buf);
    JS_FreeValue(ctx, buf);
    if (!ab_ptr) {
        JS_ThrowTypeError(ctx, "%s has a detached or invalid buffer", name);
        return false;
    }
    *out   = reinterpret_cast<float*>(ab_ptr + byte_offset);
    *count = byte_len / sizeof(float);
    return true;
}

static JSValue wrap_node(JSContext* ctx, FastNoise::SmartNode<> node)
{
    if (!node)
        return JS_ThrowTypeError(ctx, "Failed to create FastNoise node");

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn_ctor = JS_GetPropertyStr(ctx, global, "FastNoise");
    JSValue proto = JS_GetPropertyStr(ctx, fn_ctor, "prototype");
    JS_FreeValue(ctx, fn_ctor);
    JS_FreeValue(ctx, global);

    JSValue obj = JS_NewObjectProtoClass(ctx, proto, noise_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;

    auto* w = new NoiseWrapper{std::move(node)};
    JS_SetOpaque(obj, w);
    return obj;
}

// ---------------------------------------------------------------------------
// Generation methods
// ---------------------------------------------------------------------------

static JSValue noise_gen_single_2d(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "genSingle2D(x, y, seed)");

    double x, y;
    int32_t seed;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &seed, argv[2])) return JS_EXCEPTION;

    return JS_NewFloat64(ctx, w->node->GenSingle2D(
        static_cast<float>(x), static_cast<float>(y), seed));
}

static JSValue noise_gen_single_3d(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "genSingle3D(x, y, z, seed)");

    double x, y, z;
    int32_t seed;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &z, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &seed, argv[3])) return JS_EXCEPTION;

    return JS_NewFloat64(ctx, w->node->GenSingle3D(
        static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(z), seed));
}

static JSValue noise_gen_uniform_grid_2d(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 6)
        return JS_ThrowTypeError(ctx, "genUniformGrid2D(xOffset, yOffset, xSize, ySize, frequency, seed)");

    double xOffset, yOffset, frequency;
    int32_t xSize, ySize, seed;
    if (JS_ToFloat64(ctx, &xOffset, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &yOffset, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &xSize, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &ySize, argv[3])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &frequency, argv[4])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &seed, argv[5])) return JS_EXCEPTION;

    if (xSize <= 0 || ySize <= 0)
        return JS_ThrowRangeError(ctx, "Grid dimensions must be positive");

    float step = static_cast<float>(frequency);
    size_t count = static_cast<size_t>(xSize) * static_cast<size_t>(ySize);
    std::vector<float> output(count);
    w->node->GenUniformGrid2D(output.data(),
                               static_cast<float>(xOffset), static_cast<float>(yOffset),
                               xSize, ySize, step, step, seed);
    return make_float32_array(ctx, output.data(), count);
}

// In-place overload: writes directly into a caller-supplied Float32Array.
// Avoids the std::vector + JS_NewArrayBufferCopy allocations of the
// allocating variant — the caller owns one reusable buffer for repeated calls.
// Signature: genUniformGrid2DInto(dest, xOffset, yOffset, xSize, ySize, frequency, seed)
static JSValue noise_gen_uniform_grid_2d_into(JSContext* ctx, JSValueConst this_val,
                                                int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 7)
        return JS_ThrowTypeError(ctx, "genUniformGrid2DInto(dest, xOffset, yOffset, xSize, ySize, frequency, seed)");

    // Resolve destination Float32Array -> raw pointer
    size_t byte_offset = 0, byte_len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_offset, &byte_len, &bpe);
    if (JS_IsException(buf)) {
        // Clear the pending TypedArray exception so we can throw our own
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_ThrowTypeError(ctx, "dest must be a Float32Array");
    }
    if (bpe != sizeof(float)) {
        JS_FreeValue(ctx, buf);
        return JS_ThrowTypeError(ctx, "dest must be a Float32Array");
    }
    size_t abLen = 0;
    uint8_t* abPtr = JS_GetArrayBuffer(ctx, &abLen, buf);
    JS_FreeValue(ctx, buf);
    if (!abPtr)
        return JS_ThrowTypeError(ctx, "dest has detached or invalid buffer");

    double xOffset, yOffset, frequency;
    int32_t xSize, ySize, seed;
    if (JS_ToFloat64(ctx, &xOffset, argv[1])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &yOffset, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &xSize, argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &ySize, argv[4])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &frequency, argv[5])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &seed, argv[6])) return JS_EXCEPTION;

    if (xSize <= 0 || ySize <= 0)
        return JS_ThrowRangeError(ctx, "Grid dimensions must be positive");

    size_t count = static_cast<size_t>(xSize) * static_cast<size_t>(ySize);
    if (byte_len < count * sizeof(float))
        return JS_ThrowRangeError(ctx, "dest too small: %zu floats required", count);

    float* dest = reinterpret_cast<float*>(abPtr + byte_offset);
    float step = static_cast<float>(frequency);
    w->node->GenUniformGrid2D(dest,
                               static_cast<float>(xOffset), static_cast<float>(yOffset),
                               xSize, ySize, step, step, seed);
    return JS_UNDEFINED;
}

static JSValue noise_gen_uniform_grid_3d(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 8)
        return JS_ThrowTypeError(ctx, "genUniformGrid3D(xOff, yOff, zOff, xSize, ySize, zSize, freq, seed)");

    double xOff, yOff, zOff, frequency;
    int32_t xSize, ySize, zSize, seed;
    if (JS_ToFloat64(ctx, &xOff, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &yOff, argv[1])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &zOff, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &xSize, argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &ySize, argv[4])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &zSize, argv[5])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &frequency, argv[6])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &seed, argv[7])) return JS_EXCEPTION;

    if (xSize <= 0 || ySize <= 0 || zSize <= 0)
        return JS_ThrowRangeError(ctx, "Grid dimensions must be positive");

    float step = static_cast<float>(frequency);
    size_t count = static_cast<size_t>(xSize) * static_cast<size_t>(ySize) * static_cast<size_t>(zSize);
    std::vector<float> output(count);
    w->node->GenUniformGrid3D(output.data(),
                               static_cast<float>(xOff), static_cast<float>(yOff),
                               static_cast<float>(zOff),
                               xSize, ySize, zSize,
                               step, step, step, seed);
    return make_float32_array(ctx, output.data(), count);
}

// In-place overload: writes directly into a caller-supplied Float32Array.
// Avoids the std::vector + JS_NewArrayBufferCopy allocations of the
// allocating variant — the caller owns one reusable buffer for repeated calls.
// Signature: genUniformGrid3DInto(dest, xOff, yOff, zOff, xSize, ySize, zSize, freq, seed)
static JSValue noise_gen_uniform_grid_3d_into(JSContext* ctx, JSValueConst this_val,
                                                int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 9)
        return JS_ThrowTypeError(ctx, "genUniformGrid3DInto(dest, xOff, yOff, zOff, xSize, ySize, zSize, freq, seed)");

    // Resolve destination Float32Array -> raw pointer
    size_t byte_offset = 0, byte_len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_offset, &byte_len, &bpe);
    if (JS_IsException(buf)) {
        // Clear the pending TypedArray exception so we can throw our own
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_ThrowTypeError(ctx, "dest must be a Float32Array");
    }
    if (bpe != sizeof(float)) {
        JS_FreeValue(ctx, buf);
        return JS_ThrowTypeError(ctx, "dest must be a Float32Array");
    }
    size_t abLen = 0;
    uint8_t* abPtr = JS_GetArrayBuffer(ctx, &abLen, buf);
    JS_FreeValue(ctx, buf);
    if (!abPtr)
        return JS_ThrowTypeError(ctx, "dest has detached or invalid buffer");

    double xOff, yOff, zOff, frequency;
    int32_t xSize, ySize, zSize, seed;
    if (JS_ToFloat64(ctx, &xOff, argv[1])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &yOff, argv[2])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &zOff, argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &xSize, argv[4])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &ySize, argv[5])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &zSize, argv[6])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &frequency, argv[7])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &seed, argv[8])) return JS_EXCEPTION;

    if (xSize <= 0 || ySize <= 0 || zSize <= 0)
        return JS_ThrowRangeError(ctx, "Grid dimensions must be positive");

    size_t count = static_cast<size_t>(xSize) * static_cast<size_t>(ySize) * static_cast<size_t>(zSize);
    if (byte_len < count * sizeof(float))
        return JS_ThrowRangeError(ctx, "dest too small: %zu floats required", count);

    float* dest = reinterpret_cast<float*>(abPtr + byte_offset);
    float step = static_cast<float>(frequency);
    w->node->GenUniformGrid3D(dest,
                               static_cast<float>(xOff), static_cast<float>(yOff),
                               static_cast<float>(zOff),
                               xSize, ySize, zSize,
                               step, step, step, seed);
    return JS_UNDEFINED;
}

// Signature: genPositionArray2D(dest, xs, ys, xOffset, yOffset, seed)
//
// Samples at arbitrary 2D positions rather than on a lattice. See the 3D
// variant below for why the lattice entry points are not always enough.
//
// Positions are consumed as given. There is no `frequency` argument, unlike
// genUniformGrid2D, because there is no step to scale — pre-multiply the
// positions by whatever frequency you want.
static JSValue noise_gen_position_array_2d(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 6)
        return JS_ThrowTypeError(ctx, "genPositionArray2D(dest, xs, ys, xOffset, yOffset, seed)");

    float *dest = nullptr, *xs = nullptr, *ys = nullptr;
    size_t n_dest = 0, nx = 0, ny = 0;
    if (!resolve_f32(ctx, argv[0], "dest", &dest, &n_dest)) return JS_EXCEPTION;
    if (!resolve_f32(ctx, argv[1], "xs", &xs, &nx)) return JS_EXCEPTION;
    if (!resolve_f32(ctx, argv[2], "ys", &ys, &ny)) return JS_EXCEPTION;

    double x_off, y_off;
    int32_t seed;
    if (JS_ToFloat64(ctx, &x_off, argv[3])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y_off, argv[4])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &seed, argv[5])) return JS_EXCEPTION;

    size_t count = nx < ny ? nx : ny;
    if (count == 0) return JS_UNDEFINED;
    if (n_dest < count)
        return JS_ThrowRangeError(ctx, "dest too small: %zu floats required", count);
    if (count > static_cast<size_t>(INT32_MAX))
        return JS_ThrowRangeError(ctx, "position count exceeds INT_MAX");

    w->node->GenPositionArray2D(dest, static_cast<int>(count), xs, ys,
                                 static_cast<float>(x_off), static_cast<float>(y_off),
                                 seed);
    return JS_UNDEFINED;
}

// Signature: genPositionArray3D(dest, xs, ys, zs, xOffset, yOffset, zOffset, seed)
//
// Samples at arbitrary 3D positions rather than on a lattice. The uniform-grid
// entry points can only express an axis-aligned box, which rules out any
// sample set that is regular in some other space — the motivating case is a
// cube-sphere, whose vertices form a regular grid in face space but an
// irregular point set in 3D.
//
// It is also what makes spherical noise seamless: sampling a 3D field on the
// sphere surface involves no projection at all, so there is no wrap seam, no
// pole singularity, and no discontinuity where cube faces meet.
//
// Positions are consumed as given. There is no `frequency` argument, unlike
// genUniformGrid3D, because there is no step to scale — pre-multiply the
// positions by whatever frequency you want.
static JSValue noise_gen_position_array_3d(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 8)
        return JS_ThrowTypeError(ctx,
            "genPositionArray3D(dest, xs, ys, zs, xOffset, yOffset, zOffset, seed)");

    float *dest = nullptr, *xs = nullptr, *ys = nullptr, *zs = nullptr;
    size_t n_dest = 0, nx = 0, ny = 0, nz = 0;
    if (!resolve_f32(ctx, argv[0], "dest", &dest, &n_dest)) return JS_EXCEPTION;
    if (!resolve_f32(ctx, argv[1], "xs", &xs, &nx)) return JS_EXCEPTION;
    if (!resolve_f32(ctx, argv[2], "ys", &ys, &ny)) return JS_EXCEPTION;
    if (!resolve_f32(ctx, argv[3], "zs", &zs, &nz)) return JS_EXCEPTION;

    double x_off, y_off, z_off;
    int32_t seed;
    if (JS_ToFloat64(ctx, &x_off, argv[4])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y_off, argv[5])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &z_off, argv[6])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &seed, argv[7])) return JS_EXCEPTION;

    // The shortest of the three position arrays bounds the run, so a caller
    // that over-allocates one axis gets the intersection rather than a read
    // past the end of another.
    size_t count = nx;
    if (ny < count) count = ny;
    if (nz < count) count = nz;
    if (count == 0) return JS_UNDEFINED;
    if (n_dest < count)
        return JS_ThrowRangeError(ctx, "dest too small: %zu floats required", count);
    if (count > static_cast<size_t>(INT32_MAX))
        return JS_ThrowRangeError(ctx, "position count exceeds INT_MAX");

    w->node->GenPositionArray3D(dest, static_cast<int>(count), xs, ys, zs,
                                 static_cast<float>(x_off), static_cast<float>(y_off),
                                 static_cast<float>(z_off), seed);
    return JS_UNDEFINED;
}

static JSValue noise_gen_tileable_2d(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "genTileable2D(xSize, ySize, frequency, seed)");

    int32_t xSize, ySize, seed;
    double frequency;
    if (JS_ToInt32(ctx, &xSize, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &ySize, argv[1])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &frequency, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &seed, argv[3])) return JS_EXCEPTION;

    if (xSize <= 0 || ySize <= 0)
        return JS_ThrowRangeError(ctx, "Grid dimensions must be positive");

    float step = static_cast<float>(frequency);
    size_t count = static_cast<size_t>(xSize) * static_cast<size_t>(ySize);
    std::vector<float> output(count);
    w->node->GenTileable2D(output.data(), xSize, ySize, step, step, seed);
    return make_float32_array(ctx, output.data(), count);
}

// ---------------------------------------------------------------------------
// Metadata-driven generic property setter: node.set(name, value)
//
// Searches the node's metadata for a matching member by name. Handles:
//   - MemberVariable (float, int, enum)
//   - MemberNodeLookup (source node connections)
//   - MemberHybrid (accepts either a float or a FastNoise node)
// ---------------------------------------------------------------------------

// Matches a user-supplied member name against a Metadata member.
//
// Simple (non per-dimension) members match by exact string equality. Per-
// dimension members (those registered with AddPerDimensionVariable /
// AddPerDimensionHybridSource) have 4 entries sharing the same name and
// differing only by dimensionIdx (0..3 for X/Y/Z/W). We let the caller
// disambiguate by writing the dimension as a suffix: "Multiplier Y" picks
// the entry with base name "Multiplier" and dimensionIdx 1.
static bool memberNameMatches(const char* query, const FastNoise::Metadata::Member& m)
{
    if (m.dimensionIdx < 0) {
        // Non per-dimension: exact match
        return strcmp(query, m.name) == 0;
    }
    // Per-dimension: expect "<BaseName> <X|Y|Z|W>"
    size_t baseLen = strlen(m.name);
    if (strncmp(query, m.name, baseLen) != 0) return false;
    if (query[baseLen] != ' ') return false;
    char dim = query[baseLen + 1];
    if (query[baseLen + 2] != '\0') return false;
    int queryIdx = (dim == 'X') ? 0
                 : (dim == 'Y') ? 1
                 : (dim == 'Z') ? 2
                 : (dim == 'W') ? 3
                 : -1;
    return queryIdx == m.dimensionIdx;
}

static JSValue noise_set(JSContext* ctx, JSValueConst this_val,
                          int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "set(name, value)");

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    const auto& meta = w->node->GetMetadata();
    JSValueConst val = argv[1];

    // Search MemberVariable (float, int, enum by name or enum string)
    for (const auto& mv : meta.memberVariables) {
        if (!memberNameMatches(name, mv)) continue;
        JS_FreeCString(ctx, name);

        if (mv.type == FastNoise::Metadata::MemberVariable::EFloat) {
            double d;
            if (JS_ToFloat64(ctx, &d, val)) return JS_EXCEPTION;
            FastNoise::Metadata::MemberVariable::ValueUnion vu(static_cast<float>(d));
            if (!mv.setFunc(w->node.get(), vu))
                return JS_ThrowTypeError(ctx, "Failed to set variable");
            return JS_UNDEFINED;
        }
        if (mv.type == FastNoise::Metadata::MemberVariable::EInt) {
            int32_t i;
            if (JS_ToInt32(ctx, &i, val)) return JS_EXCEPTION;
            FastNoise::Metadata::MemberVariable::ValueUnion vu(static_cast<int>(i));
            if (!mv.setFunc(w->node.get(), vu))
                return JS_ThrowTypeError(ctx, "Failed to set variable");
            return JS_UNDEFINED;
        }
        if (mv.type == FastNoise::Metadata::MemberVariable::EEnum) {
            // Accept int index or string name
            if (JS_IsNumber(val)) {
                int32_t i;
                if (JS_ToInt32(ctx, &i, val)) return JS_EXCEPTION;
                FastNoise::Metadata::MemberVariable::ValueUnion vu(static_cast<int>(i));
                if (!mv.setFunc(w->node.get(), vu))
                    return JS_ThrowTypeError(ctx, "Failed to set enum");
                return JS_UNDEFINED;
            }
            if (JS_IsString(val)) {
                const char* enumStr = JS_ToCString(ctx, val);
                if (!enumStr) return JS_EXCEPTION;
                for (size_t ei = 0; ei < mv.enumNames.size(); ei++) {
                    if (strcmp(mv.enumNames[ei], enumStr) == 0) {
                        JS_FreeCString(ctx, enumStr);
                        FastNoise::Metadata::MemberVariable::ValueUnion vu(static_cast<int>(ei));
                        if (!mv.setFunc(w->node.get(), vu))
                            return JS_ThrowTypeError(ctx, "Failed to set enum");
                        return JS_UNDEFINED;
                    }
                }
                JSValue err = JS_ThrowRangeError(ctx, "Unknown enum value '%s'", enumStr);
                JS_FreeCString(ctx, enumStr);
                return err;
            }
            return JS_ThrowTypeError(ctx, "Enum expects int or string");
        }
        return JS_ThrowTypeError(ctx, "Unknown variable type");
    }

    // Search MemberNodeLookup (source connections)
    for (const auto& mn : meta.memberNodeLookups) {
        if (!memberNameMatches(name, mn)) continue;
        JS_FreeCString(ctx, name);

        auto* sw = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, val, noise_class_id));
        if (!sw)
            return JS_ThrowTypeError(ctx, "Node input requires a FastNoise node");
        if (!mn.setFunc(w->node.get(), sw->node))
            return JS_ThrowTypeError(ctx, "Failed to set node source (type mismatch)");
        return JS_UNDEFINED;
    }

    // Search MemberHybrid (float or node)
    for (const auto& mh : meta.memberHybrids) {
        if (!memberNameMatches(name, mh)) continue;
        JS_FreeCString(ctx, name);

        // If value is a FastNoise node, connect it
        auto* sw = static_cast<NoiseWrapper*>(JS_GetOpaque(val, noise_class_id));
        if (sw) {
            if (!mh.setNodeFunc(w->node.get(), sw->node))
                return JS_ThrowTypeError(ctx, "Failed to set hybrid node source");
            return JS_UNDEFINED;
        }
        // Otherwise treat as float
        double d;
        if (JS_ToFloat64(ctx, &d, val)) return JS_EXCEPTION;
        if (!mh.setValueFunc(w->node.get(), static_cast<float>(d)))
            return JS_ThrowTypeError(ctx, "Failed to set hybrid value");
        return JS_UNDEFINED;
    }

    JSValue err = JS_ThrowReferenceError(ctx, "No member '%s' on this node type", name);
    JS_FreeCString(ctx, name);
    return err;
}

// ---------------------------------------------------------------------------
// Metadata-driven introspection: node.getMembers()
// Returns { variables: [...], nodes: [...], hybrids: [...] }
// ---------------------------------------------------------------------------

static JSValue noise_get_members(JSContext* ctx, JSValueConst this_val,
                                  int, JSValueConst*)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;

    const auto& meta = w->node->GetMetadata();
    JSValue result = JS_NewObject(ctx);
    JSValue typeName = JS_NewString(ctx, meta.name);
    JS_SetPropertyStr(ctx, result, "type", typeName);

    // Variables
    JSValue vars = JS_NewArray(ctx);
    for (size_t i = 0; i < meta.memberVariables.size(); i++) {
        const auto& mv = meta.memberVariables[i];
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, mv.name));
        const char* typeStr = mv.type == FastNoise::Metadata::MemberVariable::EFloat ? "float"
                            : mv.type == FastNoise::Metadata::MemberVariable::EInt   ? "int"
                            : "enum";
        JS_SetPropertyStr(ctx, entry, "type", JS_NewString(ctx, typeStr));
        if (mv.type == FastNoise::Metadata::MemberVariable::EEnum) {
            JSValue names = JS_NewArray(ctx);
            for (size_t ei = 0; ei < mv.enumNames.size(); ei++)
                JS_SetPropertyUint32(ctx, names, static_cast<uint32_t>(ei),
                                     JS_NewString(ctx, mv.enumNames[ei]));
            JS_SetPropertyStr(ctx, entry, "enumValues", names);
        }
        JS_SetPropertyUint32(ctx, vars, static_cast<uint32_t>(i), entry);
    }
    JS_SetPropertyStr(ctx, result, "variables", vars);

    // Node lookups
    JSValue nodes = JS_NewArray(ctx);
    for (size_t i = 0; i < meta.memberNodeLookups.size(); i++) {
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "name",
                          JS_NewString(ctx, meta.memberNodeLookups[i].name));
        JS_SetPropertyUint32(ctx, nodes, static_cast<uint32_t>(i), entry);
    }
    JS_SetPropertyStr(ctx, result, "nodes", nodes);

    // Hybrids
    JSValue hybrids = JS_NewArray(ctx);
    for (size_t i = 0; i < meta.memberHybrids.size(); i++) {
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "name",
                          JS_NewString(ctx, meta.memberHybrids[i].name));
        JS_SetPropertyStr(ctx, entry, "default",
                          JS_NewFloat64(ctx, meta.memberHybrids[i].valueDefault));
        JS_SetPropertyUint32(ctx, hybrids, static_cast<uint32_t>(i), entry);
    }
    JS_SetPropertyStr(ctx, result, "hybrids", hybrids);

    return result;
}

// ---------------------------------------------------------------------------
// FastNoise.create(typeName) — metadata-driven factory for any node type
// ---------------------------------------------------------------------------

static JSValue noise_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "FastNoise.create(typeName)");

    const char* typeName = JS_ToCString(ctx, argv[0]);
    if (!typeName) return JS_EXCEPTION;

    for (const auto* meta : FastNoise::Metadata::GetAll()) {
        if (meta && strcmp(meta->name, typeName) == 0) {
            JS_FreeCString(ctx, typeName);
            auto node = meta->CreateNode();
            return wrap_node(ctx, std::move(node));
        }
    }

    JSValue err = JS_ThrowReferenceError(ctx, "Unknown FastNoise type '%s'", typeName);
    JS_FreeCString(ctx, typeName);
    return err;
}

// ---------------------------------------------------------------------------
// FastNoise.types() — list all available node types
// ---------------------------------------------------------------------------

static JSValue noise_types(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    for (const auto* meta : FastNoise::Metadata::GetAll()) {
        if (!meta) continue;
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, meta->name));

        JSValue groups = JS_NewArray(ctx);
        for (size_t gi = 0; gi < meta->groups.size(); gi++)
            JS_SetPropertyUint32(ctx, groups, static_cast<uint32_t>(gi),
                                 JS_NewString(ctx, meta->groups[gi]));
        JS_SetPropertyStr(ctx, entry, "groups", groups);

        JS_SetPropertyUint32(ctx, arr, idx++, entry);
    }
    return arr;
}

// ---------------------------------------------------------------------------
// Constructor: new FastNoise(encodedNodeTree)
// ---------------------------------------------------------------------------

static JSValue noise_ctor(JSContext* ctx, JSValueConst new_target,
                           int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "FastNoise: expected encoded node tree string");

    const char* encoded = JS_ToCString(ctx, argv[0]);
    if (!encoded) return JS_EXCEPTION;

    auto node = FastNoise::NewFromEncodedNodeTree(encoded);
    JS_FreeCString(ctx, encoded);

    if (!node)
        return JS_ThrowTypeError(ctx, "FastNoise: invalid encoded node tree");

    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) return proto;

    JSValue obj = JS_NewObjectProtoClass(ctx, proto, noise_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;

    auto* w = new NoiseWrapper{std::move(node)};
    JS_SetOpaque(obj, w);
    return obj;
}

// ---------------------------------------------------------------------------
// Convenience factory template (for named shortcuts like FastNoise.Simplex())
// ---------------------------------------------------------------------------

template<typename T>
static JSValue noise_factory(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    auto node = FastNoise::New<T>();
    return wrap_node(ctx, std::move(node));
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installNoise(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);

    JS_NewClassID(rt, &noise_class_id);
    JS_NewClass(rt, noise_class_id, &noise_class_def);

    // Prototype
    JSValue proto = JS_NewObject(ctx);

    // Generation
    JS_SetPropertyStr(ctx, proto, "genSingle2D",
        JS_NewCFunction(ctx, noise_gen_single_2d, "genSingle2D", 3));
    JS_SetPropertyStr(ctx, proto, "genSingle3D",
        JS_NewCFunction(ctx, noise_gen_single_3d, "genSingle3D", 4));
    JS_SetPropertyStr(ctx, proto, "genUniformGrid2D",
        JS_NewCFunction(ctx, noise_gen_uniform_grid_2d, "genUniformGrid2D", 6));
    JS_SetPropertyStr(ctx, proto, "genUniformGrid2DInto",
        JS_NewCFunction(ctx, noise_gen_uniform_grid_2d_into, "genUniformGrid2DInto", 7));
    JS_SetPropertyStr(ctx, proto, "genUniformGrid3D",
        JS_NewCFunction(ctx, noise_gen_uniform_grid_3d, "genUniformGrid3D", 8));
    JS_SetPropertyStr(ctx, proto, "genUniformGrid3DInto",
        JS_NewCFunction(ctx, noise_gen_uniform_grid_3d_into, "genUniformGrid3DInto", 9));
    JS_SetPropertyStr(ctx, proto, "genPositionArray2D",
        JS_NewCFunction(ctx, noise_gen_position_array_2d, "genPositionArray2D", 6));
    JS_SetPropertyStr(ctx, proto, "genPositionArray3D",
        JS_NewCFunction(ctx, noise_gen_position_array_3d, "genPositionArray3D", 8));
    JS_SetPropertyStr(ctx, proto, "genTileable2D",
        JS_NewCFunction(ctx, noise_gen_tileable_2d, "genTileable2D", 4));

    // Generic metadata-driven configuration
    JS_SetPropertyStr(ctx, proto, "set",
        JS_NewCFunction(ctx, noise_set, "set", 2));
    JS_SetPropertyStr(ctx, proto, "getMembers",
        JS_NewCFunction(ctx, noise_get_members, "getMembers", 0));

    // Constructor (encoded string)
    JSValue ctor = JS_NewCFunction2(ctx, noise_ctor, "FastNoise", 1,
                                     JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, noise_class_id, proto);

    // Static: generic factory + type listing
    JS_SetPropertyStr(ctx, ctor, "create",
        JS_NewCFunction(ctx, noise_create, "create", 1));
    JS_SetPropertyStr(ctx, ctor, "types",
        JS_NewCFunction(ctx, noise_types, "types", 0));

    // Named convenience factories for common types
    JS_SetPropertyStr(ctx, ctor, "Simplex",
        JS_NewCFunction(ctx, noise_factory<FastNoise::Simplex>, "Simplex", 0));
    JS_SetPropertyStr(ctx, ctor, "SuperSimplex",
        JS_NewCFunction(ctx, noise_factory<FastNoise::SuperSimplex>, "SuperSimplex", 0));
    JS_SetPropertyStr(ctx, ctor, "Perlin",
        JS_NewCFunction(ctx, noise_factory<FastNoise::Perlin>, "Perlin", 0));
    JS_SetPropertyStr(ctx, ctor, "Value",
        JS_NewCFunction(ctx, noise_factory<FastNoise::Value>, "Value", 0));
    JS_SetPropertyStr(ctx, ctor, "CellularValue",
        JS_NewCFunction(ctx, noise_factory<FastNoise::CellularValue>, "CellularValue", 0));
    JS_SetPropertyStr(ctx, ctor, "CellularDistance",
        JS_NewCFunction(ctx, noise_factory<FastNoise::CellularDistance>, "CellularDistance", 0));
    JS_SetPropertyStr(ctx, ctor, "CellularLookup",
        JS_NewCFunction(ctx, noise_factory<FastNoise::CellularLookup>, "CellularLookup", 0));
    JS_SetPropertyStr(ctx, ctor, "FractalFBm",
        JS_NewCFunction(ctx, noise_factory<FastNoise::FractalFBm>, "FractalFBm", 0));
    JS_SetPropertyStr(ctx, ctor, "FractalRidged",
        JS_NewCFunction(ctx, noise_factory<FastNoise::FractalRidged>, "FractalRidged", 0));
    JS_SetPropertyStr(ctx, ctor, "DomainWarpGradient",
        JS_NewCFunction(ctx, noise_factory<FastNoise::DomainWarpGradient>, "DomainWarpGradient", 0));

    // Register global
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "FastNoise", ctor);
    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
