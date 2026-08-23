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

static void fast_noise_finalizer(JSRuntime*, JSValue val)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque(val, noise_class_id));
    delete w;
}

static JSClassDef fast_noise_class_def = { "FastNoise", fast_noise_finalizer };

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

static bool resolve_f32(JSContext* ctx, JSValueConst v, const char* name,
                        float** out, size_t* count)
{
    size_t byte_offset = 0, byte_len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &byte_offset, &byte_len, &bpe);
    if (JS_IsException(buf)) {
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

static NoiseWrapper* get_noise(JSContext* ctx, JSValueConst this_val)
{
    return static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
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

static bool memberNameMatches(const char* query, const FastNoise::Metadata::Member& m)
{
    if (m.dimensionIdx < 0) {
        return strcmp(query, m.name) == 0;
    }
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

template<typename T>
static JSValue make_factory_node(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    auto node = FastNoise::New<T>();
    return wrap_node(ctx, std::move(node));
}

static JSValue fast_noise_create(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "FastNoise.create(typeName)");
    const char* typeName = JS_ToCString(ctx, argv[0]); if (!typeName) return JS_EXCEPTION;
    for (const auto* meta : FastNoise::Metadata::GetAll()) {
        if (meta && strcmp(meta->name, typeName) == 0) {
            JS_FreeCString(ctx, typeName); return wrap_node(ctx, meta->CreateNode());
        }
    }
    JSValue err = JS_ThrowReferenceError(ctx, "Unknown FastNoise type '%s'", typeName); JS_FreeCString(ctx, typeName); return err;
}

static JSValue fast_noise_types(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    JSValue arr = JS_NewArray(ctx); uint32_t idx = 0;
    for (const auto* meta : FastNoise::Metadata::GetAll()) {
        if (!meta) continue;
        JSValue entry = JS_NewObject(ctx); JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, meta->name));
        JSValue groups = JS_NewArray(ctx);
        for (size_t gi = 0; gi < meta->groups.size(); gi++) JS_SetPropertyUint32(ctx, groups, static_cast<uint32_t>(gi), JS_NewString(ctx, meta->groups[gi]));
        JS_SetPropertyStr(ctx, entry, "groups", groups); JS_SetPropertyUint32(ctx, arr, idx++, entry);
    }
    return arr;
}

static JSValue fast_noise_set(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
    if (!w) return JS_EXCEPTION;
    if (argc < 2 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "set(name, value)");
    const char* name = JS_ToCString(ctx, argv[0]); if (!name) return JS_EXCEPTION;
    const auto& meta = w->node->GetMetadata(); JSValueConst val = argv[1];
    for (const auto& mv : meta.memberVariables) {
        if (!memberNameMatches(name, mv)) continue;
        JS_FreeCString(ctx, name);
        if (mv.type == FastNoise::Metadata::MemberVariable::EFloat) {
            double d; if (JS_ToFloat64(ctx, &d, val)) return JS_EXCEPTION;
            return mv.setFunc(w->node.get(), FastNoise::Metadata::MemberVariable::ValueUnion(static_cast<float>(d))) ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "Failed to set variable");
        }
        if (mv.type == FastNoise::Metadata::MemberVariable::EInt) {
            int32_t i; if (JS_ToInt32(ctx, &i, val)) return JS_EXCEPTION;
            return mv.setFunc(w->node.get(), FastNoise::Metadata::MemberVariable::ValueUnion(static_cast<int>(i))) ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "Failed to set variable");
        }
        if (mv.type == FastNoise::Metadata::MemberVariable::EEnum) {
            int32_t i = -1;
            if (JS_IsNumber(val)) { JS_ToInt32(ctx, &i, val); }
            else if (JS_IsString(val)) {
                const char* es = JS_ToCString(ctx, val); if (!es) return JS_EXCEPTION;
                for (size_t ei = 0; ei < mv.enumNames.size(); ei++) { if (strcmp(mv.enumNames[ei], es) == 0) { i = static_cast<int32_t>(ei); break; } }
                JS_FreeCString(ctx, es); if (i < 0) return JS_ThrowRangeError(ctx, "Unknown enum value");
            } else return JS_ThrowTypeError(ctx, "Enum expects int or string");
            return mv.setFunc(w->node.get(), FastNoise::Metadata::MemberVariable::ValueUnion(static_cast<int>(i))) ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "Failed to set enum");
        }
        return JS_ThrowTypeError(ctx, "Unknown variable type");
    }
    for (const auto& mn : meta.memberNodeLookups) {
        if (!memberNameMatches(name, mn)) continue;
        JS_FreeCString(ctx, name); auto* sw = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, val, noise_class_id));
        if (!sw) return JS_ThrowTypeError(ctx, "Node input requires a FastNoise node");
        return mn.setFunc(w->node.get(), sw->node) ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "Failed to set node source (type mismatch)");
    }
    for (const auto& mh : meta.memberHybrids) {
        if (!memberNameMatches(name, mh)) continue;
        JS_FreeCString(ctx, name); auto* sw = static_cast<NoiseWrapper*>(JS_GetOpaque(val, noise_class_id));
        if (sw) return mh.setNodeFunc(w->node.get(), sw->node) ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "Failed to set hybrid node source");
        double d; if (JS_ToFloat64(ctx, &d, val)) return JS_EXCEPTION;
        return mh.setValueFunc(w->node.get(), static_cast<float>(d)) ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "Failed to set hybrid value");
    }
    JSValue err = JS_ThrowReferenceError(ctx, "No member '%s' on this node type", name); JS_FreeCString(ctx, name); return err;
}

static JSValue fast_noise_get_members(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
    if (!w) return JS_EXCEPTION;
    const auto& meta = w->node->GetMetadata(); JSValue result = JS_NewObject(ctx); JS_SetPropertyStr(ctx, result, "type", JS_NewString(ctx, meta.name));
    JSValue vars = JS_NewArray(ctx);
    for (size_t i = 0; i < meta.memberVariables.size(); i++) {
        const auto& mv = meta.memberVariables[i]; JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, mv.name));
        JS_SetPropertyStr(ctx, entry, "type", JS_NewString(ctx, mv.type == FastNoise::Metadata::MemberVariable::EFloat ? "float" : mv.type == FastNoise::Metadata::MemberVariable::EInt ? "int" : "enum"));
        if (mv.type == FastNoise::Metadata::MemberVariable::EEnum) {
            JSValue names = JS_NewArray(ctx); for (size_t ei = 0; ei < mv.enumNames.size(); ei++) JS_SetPropertyUint32(ctx, names, static_cast<uint32_t>(ei), JS_NewString(ctx, mv.enumNames[ei]));
            JS_SetPropertyStr(ctx, entry, "enumValues", names);
        }
        JS_SetPropertyUint32(ctx, vars, static_cast<uint32_t>(i), entry);
    }
    JS_SetPropertyStr(ctx, result, "variables", vars); JSValue nodes = JS_NewArray(ctx);
    for (size_t i = 0; i < meta.memberNodeLookups.size(); i++) {
        JSValue entry = JS_NewObject(ctx); JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, meta.memberNodeLookups[i].name));
        JS_SetPropertyUint32(ctx, nodes, static_cast<uint32_t>(i), entry);
    }
    JS_SetPropertyStr(ctx, result, "nodes", nodes); JSValue hybrids = JS_NewArray(ctx);
    for (size_t i = 0; i < meta.memberHybrids.size(); i++) {
        JSValue entry = JS_NewObject(ctx); JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, meta.memberHybrids[i].name));
        JS_SetPropertyStr(ctx, entry, "default", JS_NewFloat64(ctx, meta.memberHybrids[i].valueDefault)); JS_SetPropertyUint32(ctx, hybrids, static_cast<uint32_t>(i), entry);
    }
    JS_SetPropertyStr(ctx, result, "hybrids", hybrids); return result;
}

static JSValue fast_noise_gen_single2_d(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
    if (!w) return JS_EXCEPTION;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "genSingle2D(x, y, seed)");

    double x;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    double y;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;
    int32_t seed;
    if (JS_ToInt32(ctx, &seed, argv[2])) return JS_EXCEPTION;

    return JS_NewFloat64(ctx, w->node->GenSingle2D(
        static_cast<float>(x), static_cast<float>(y), seed));
}

static JSValue fast_noise_gen_single3_d(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
    if (!w) return JS_EXCEPTION;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "genSingle3D(x, y, z, seed)");

    double x;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    double y;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;
    double z;
    if (JS_ToFloat64(ctx, &z, argv[2])) return JS_EXCEPTION;
    int32_t seed;
    if (JS_ToInt32(ctx, &seed, argv[3])) return JS_EXCEPTION;

    return JS_NewFloat64(ctx, w->node->GenSingle3D(
        static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(z), seed));
}

static JSValue fast_noise_gen_uniform_grid2_d(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
    if (!w) return JS_EXCEPTION;
    if (argc < 6)
        return JS_ThrowTypeError(ctx, "genUniformGrid2D(xOffset, yOffset, xSize, ySize, frequency, seed)");

    double xOffset;
    if (JS_ToFloat64(ctx, &xOffset, argv[0])) return JS_EXCEPTION;
    double yOffset;
    if (JS_ToFloat64(ctx, &yOffset, argv[1])) return JS_EXCEPTION;
    int32_t xSize;
    if (JS_ToInt32(ctx, &xSize, argv[2])) return JS_EXCEPTION;
    int32_t ySize;
    if (JS_ToInt32(ctx, &ySize, argv[3])) return JS_EXCEPTION;
    double frequency;
    if (JS_ToFloat64(ctx, &frequency, argv[4])) return JS_EXCEPTION;
    int32_t seed;
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

static JSValue fast_noise_gen_uniform_grid2_d_into(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
    if (!w) return JS_EXCEPTION;
    if (argc < 7)
        return JS_ThrowTypeError(ctx, "genUniformGrid2DInto(dest, xOffset, yOffset, xSize, ySize, frequency, seed)");

    float* dest = nullptr;
    size_t n_dest = 0;
    if (!resolve_f32(ctx, argv[0], "dest", &dest, &n_dest)) return JS_EXCEPTION;
    double xOffset;
    if (JS_ToFloat64(ctx, &xOffset, argv[1])) return JS_EXCEPTION;
    double yOffset;
    if (JS_ToFloat64(ctx, &yOffset, argv[2])) return JS_EXCEPTION;
    int32_t xSize;
    if (JS_ToInt32(ctx, &xSize, argv[3])) return JS_EXCEPTION;
    int32_t ySize;
    if (JS_ToInt32(ctx, &ySize, argv[4])) return JS_EXCEPTION;
    double frequency;
    if (JS_ToFloat64(ctx, &frequency, argv[5])) return JS_EXCEPTION;
    int32_t seed;
    if (JS_ToInt32(ctx, &seed, argv[6])) return JS_EXCEPTION;

    if (xSize <= 0 || ySize <= 0)
        return JS_ThrowRangeError(ctx, "Grid dimensions must be positive");
    
    size_t count = static_cast<size_t>(xSize) * static_cast<size_t>(ySize);
    if (n_dest < count)
        return JS_ThrowRangeError(ctx, "dest too small: %zu floats required", count);
    
    float step = static_cast<float>(frequency);
    w->node->GenUniformGrid2D(dest,
                               static_cast<float>(xOffset), static_cast<float>(yOffset),
                               xSize, ySize, step, step, seed);
    return JS_UNDEFINED;
}

static JSValue fast_noise_gen_uniform_grid3_d(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
    if (!w) return JS_EXCEPTION;
    if (argc < 8)
        return JS_ThrowTypeError(ctx, "genUniformGrid3D(xOff, yOff, zOff, xSize, ySize, zSize, frequency, seed)");

    double xOff;
    if (JS_ToFloat64(ctx, &xOff, argv[0])) return JS_EXCEPTION;
    double yOff;
    if (JS_ToFloat64(ctx, &yOff, argv[1])) return JS_EXCEPTION;
    double zOff;
    if (JS_ToFloat64(ctx, &zOff, argv[2])) return JS_EXCEPTION;
    int32_t xSize;
    if (JS_ToInt32(ctx, &xSize, argv[3])) return JS_EXCEPTION;
    int32_t ySize;
    if (JS_ToInt32(ctx, &ySize, argv[4])) return JS_EXCEPTION;
    int32_t zSize;
    if (JS_ToInt32(ctx, &zSize, argv[5])) return JS_EXCEPTION;
    double frequency;
    if (JS_ToFloat64(ctx, &frequency, argv[6])) return JS_EXCEPTION;
    int32_t seed;
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

static JSValue fast_noise_gen_uniform_grid3_d_into(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
    if (!w) return JS_EXCEPTION;
    if (argc < 9)
        return JS_ThrowTypeError(ctx, "genUniformGrid3DInto(dest, xOff, yOff, zOff, xSize, ySize, zSize, frequency, seed)");

    float* dest = nullptr;
    size_t n_dest = 0;
    if (!resolve_f32(ctx, argv[0], "dest", &dest, &n_dest)) return JS_EXCEPTION;
    double xOff;
    if (JS_ToFloat64(ctx, &xOff, argv[1])) return JS_EXCEPTION;
    double yOff;
    if (JS_ToFloat64(ctx, &yOff, argv[2])) return JS_EXCEPTION;
    double zOff;
    if (JS_ToFloat64(ctx, &zOff, argv[3])) return JS_EXCEPTION;
    int32_t xSize;
    if (JS_ToInt32(ctx, &xSize, argv[4])) return JS_EXCEPTION;
    int32_t ySize;
    if (JS_ToInt32(ctx, &ySize, argv[5])) return JS_EXCEPTION;
    int32_t zSize;
    if (JS_ToInt32(ctx, &zSize, argv[6])) return JS_EXCEPTION;
    double frequency;
    if (JS_ToFloat64(ctx, &frequency, argv[7])) return JS_EXCEPTION;
    int32_t seed;
    if (JS_ToInt32(ctx, &seed, argv[8])) return JS_EXCEPTION;

    if (xSize <= 0 || ySize <= 0 || zSize <= 0)
        return JS_ThrowRangeError(ctx, "Grid dimensions must be positive");
    
    size_t count = static_cast<size_t>(xSize) * static_cast<size_t>(ySize) * static_cast<size_t>(zSize);
    if (n_dest < count)
        return JS_ThrowRangeError(ctx, "dest too small: %zu floats required", count);
    
    float step = static_cast<float>(frequency);
    w->node->GenUniformGrid3D(dest,
                               static_cast<float>(xOff), static_cast<float>(yOff),
                               static_cast<float>(zOff),
                               xSize, ySize, zSize,
                               step, step, step, seed);
    return JS_UNDEFINED;
}

static JSValue fast_noise_gen_position_array2_d(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
    if (!w) return JS_EXCEPTION;
    if (argc < 6)
        return JS_ThrowTypeError(ctx, "genPositionArray2D(dest, xs, ys, x_off, y_off, seed)");

    float* dest = nullptr;
    size_t n_dest = 0;
    if (!resolve_f32(ctx, argv[0], "dest", &dest, &n_dest)) return JS_EXCEPTION;
    float* xs = nullptr;
    size_t n_xs = 0;
    if (!resolve_f32(ctx, argv[1], "xs", &xs, &n_xs)) return JS_EXCEPTION;
    float* ys = nullptr;
    size_t n_ys = 0;
    if (!resolve_f32(ctx, argv[2], "ys", &ys, &n_ys)) return JS_EXCEPTION;
    double x_off;
    if (JS_ToFloat64(ctx, &x_off, argv[3])) return JS_EXCEPTION;
    double y_off;
    if (JS_ToFloat64(ctx, &y_off, argv[4])) return JS_EXCEPTION;
    int32_t seed;
    if (JS_ToInt32(ctx, &seed, argv[5])) return JS_EXCEPTION;

    size_t count = n_xs < n_ys ? n_xs : n_ys;
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

static JSValue fast_noise_gen_position_array3_d(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
    if (!w) return JS_EXCEPTION;
    if (argc < 8)
        return JS_ThrowTypeError(ctx, "genPositionArray3D(dest, xs, ys, zs, x_off, y_off, z_off, seed)");

    float* dest = nullptr;
    size_t n_dest = 0;
    if (!resolve_f32(ctx, argv[0], "dest", &dest, &n_dest)) return JS_EXCEPTION;
    float* xs = nullptr;
    size_t n_xs = 0;
    if (!resolve_f32(ctx, argv[1], "xs", &xs, &n_xs)) return JS_EXCEPTION;
    float* ys = nullptr;
    size_t n_ys = 0;
    if (!resolve_f32(ctx, argv[2], "ys", &ys, &n_ys)) return JS_EXCEPTION;
    float* zs = nullptr;
    size_t n_zs = 0;
    if (!resolve_f32(ctx, argv[3], "zs", &zs, &n_zs)) return JS_EXCEPTION;
    double x_off;
    if (JS_ToFloat64(ctx, &x_off, argv[4])) return JS_EXCEPTION;
    double y_off;
    if (JS_ToFloat64(ctx, &y_off, argv[5])) return JS_EXCEPTION;
    double z_off;
    if (JS_ToFloat64(ctx, &z_off, argv[6])) return JS_EXCEPTION;
    int32_t seed;
    if (JS_ToInt32(ctx, &seed, argv[7])) return JS_EXCEPTION;

    size_t count = n_xs;
    if (n_ys < count) count = n_ys;
    if (n_zs < count) count = n_zs;
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

static JSValue fast_noise_gen_tileable2_d(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
    if (!w) return JS_EXCEPTION;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "genTileable2D(xSize, ySize, frequency, seed)");

    int32_t xSize;
    if (JS_ToInt32(ctx, &xSize, argv[0])) return JS_EXCEPTION;
    int32_t ySize;
    if (JS_ToInt32(ctx, &ySize, argv[1])) return JS_EXCEPTION;
    double frequency;
    if (JS_ToFloat64(ctx, &frequency, argv[2])) return JS_EXCEPTION;
    int32_t seed;
    if (JS_ToInt32(ctx, &seed, argv[3])) return JS_EXCEPTION;

    if (xSize <= 0 || ySize <= 0)
        return JS_ThrowRangeError(ctx, "Grid dimensions must be positive");
    
    float step = static_cast<float>(frequency);
    size_t count = static_cast<size_t>(xSize) * static_cast<size_t>(ySize);
    std::vector<float> output(count);
    w->node->GenTileable2D(output.data(), xSize, ySize, step, step, seed);
    return make_float32_array(ctx, output.data(), count);
}

static JSValue js_fast_noise_constructor(JSContext* ctx, JSValueConst new_target,
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

void installNoise(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue global = JS_GetGlobalObject(ctx);

    // Register FastNoise class
    if (noise_class_id == 0) JS_NewClassID(rt, &noise_class_id);
    JS_NewClass(rt, noise_class_id, &fast_noise_class_def);

    JSValue fast_noiseProto = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, fast_noiseProto, "set",
        JS_NewCFunction(ctx, fast_noise_set, "set", 2));
    JS_SetPropertyStr(ctx, fast_noiseProto, "getMembers",
        JS_NewCFunction(ctx, fast_noise_get_members, "getMembers", 0));
    JS_SetPropertyStr(ctx, fast_noiseProto, "genSingle2D",
        JS_NewCFunction(ctx, fast_noise_gen_single2_d, "genSingle2D", 3));
    JS_SetPropertyStr(ctx, fast_noiseProto, "genSingle3D",
        JS_NewCFunction(ctx, fast_noise_gen_single3_d, "genSingle3D", 4));
    JS_SetPropertyStr(ctx, fast_noiseProto, "genUniformGrid2D",
        JS_NewCFunction(ctx, fast_noise_gen_uniform_grid2_d, "genUniformGrid2D", 6));
    JS_SetPropertyStr(ctx, fast_noiseProto, "genUniformGrid2DInto",
        JS_NewCFunction(ctx, fast_noise_gen_uniform_grid2_d_into, "genUniformGrid2DInto", 7));
    JS_SetPropertyStr(ctx, fast_noiseProto, "genUniformGrid3D",
        JS_NewCFunction(ctx, fast_noise_gen_uniform_grid3_d, "genUniformGrid3D", 8));
    JS_SetPropertyStr(ctx, fast_noiseProto, "genUniformGrid3DInto",
        JS_NewCFunction(ctx, fast_noise_gen_uniform_grid3_d_into, "genUniformGrid3DInto", 9));
    JS_SetPropertyStr(ctx, fast_noiseProto, "genPositionArray2D",
        JS_NewCFunction(ctx, fast_noise_gen_position_array2_d, "genPositionArray2D", 6));
    JS_SetPropertyStr(ctx, fast_noiseProto, "genPositionArray3D",
        JS_NewCFunction(ctx, fast_noise_gen_position_array3_d, "genPositionArray3D", 8));
    JS_SetPropertyStr(ctx, fast_noiseProto, "genTileable2D",
        JS_NewCFunction(ctx, fast_noise_gen_tileable2_d, "genTileable2D", 4));

    JS_SetClassProto(ctx, noise_class_id, fast_noiseProto);

    JSValue fast_noiseCtor = JS_NewCFunction2(ctx, js_fast_noise_constructor, "FastNoise", 1,
                                         JS_CFUNC_constructor, 0);
    fast_noiseProto = JS_GetClassProto(ctx, noise_class_id);
    JS_SetPropertyStr(ctx, fast_noiseCtor, "prototype", JS_DupValue(ctx, fast_noiseProto));
    JS_SetPropertyStr(ctx, fast_noiseProto, "constructor", JS_DupValue(ctx, fast_noiseCtor));
    JS_FreeValue(ctx, fast_noiseProto);

    JS_SetPropertyStr(ctx, fast_noiseCtor, "create",
        JS_NewCFunction(ctx, fast_noise_create, "create", 1));
    JS_SetPropertyStr(ctx, fast_noiseCtor, "types",
        JS_NewCFunction(ctx, fast_noise_types, "types", 0));
    JS_SetPropertyStr(ctx, fast_noiseCtor, "Simplex",
        JS_NewCFunction(ctx, make_factory_node<FastNoise::Simplex>, "Simplex", 0));
    JS_SetPropertyStr(ctx, fast_noiseCtor, "SuperSimplex",
        JS_NewCFunction(ctx, make_factory_node<FastNoise::SuperSimplex>, "SuperSimplex", 0));
    JS_SetPropertyStr(ctx, fast_noiseCtor, "Perlin",
        JS_NewCFunction(ctx, make_factory_node<FastNoise::Perlin>, "Perlin", 0));
    JS_SetPropertyStr(ctx, fast_noiseCtor, "Value",
        JS_NewCFunction(ctx, make_factory_node<FastNoise::Value>, "Value", 0));
    JS_SetPropertyStr(ctx, fast_noiseCtor, "CellularValue",
        JS_NewCFunction(ctx, make_factory_node<FastNoise::CellularValue>, "CellularValue", 0));
    JS_SetPropertyStr(ctx, fast_noiseCtor, "CellularDistance",
        JS_NewCFunction(ctx, make_factory_node<FastNoise::CellularDistance>, "CellularDistance", 0));
    JS_SetPropertyStr(ctx, fast_noiseCtor, "CellularLookup",
        JS_NewCFunction(ctx, make_factory_node<FastNoise::CellularLookup>, "CellularLookup", 0));
    JS_SetPropertyStr(ctx, fast_noiseCtor, "FractalFBm",
        JS_NewCFunction(ctx, make_factory_node<FastNoise::FractalFBm>, "FractalFBm", 0));
    JS_SetPropertyStr(ctx, fast_noiseCtor, "FractalRidged",
        JS_NewCFunction(ctx, make_factory_node<FastNoise::FractalRidged>, "FractalRidged", 0));
    JS_SetPropertyStr(ctx, fast_noiseCtor, "DomainWarpGradient",
        JS_NewCFunction(ctx, make_factory_node<FastNoise::DomainWarpGradient>, "DomainWarpGradient", 0));

    JS_SetPropertyStr(ctx, global, "FastNoise", fast_noiseCtor);

    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
