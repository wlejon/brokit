#include "api/api.h"
#include "runtime/runtime.h"

#include <FastNoise/FastNoise.h>
#include <vector>
#include <cstring>

namespace brokit::api {

static JSClassID noise_class_id = 0;

struct NoiseWrapper {
    FastNoise::SmartNode<> node;
};

static void noise_finalizer(JSRuntime*, JSValue val)
{
    auto* w = static_cast<NoiseWrapper*>(JS_GetOpaque(val, noise_class_id));
    delete w;
}

static JSClassDef noise_class_def = { "FastNoise", noise_finalizer };

// Helper: create a Float32Array from a float buffer
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

// Helper: unwrap this_val to NoiseWrapper*
static NoiseWrapper* get_noise(JSContext* ctx, JSValueConst this_val)
{
    return static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
}

// genSingle2D(x, y, seed)
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

    float result = w->node->GenSingle2D(static_cast<float>(x),
                                         static_cast<float>(y), seed);
    return JS_NewFloat64(ctx, result);
}

// genSingle3D(x, y, z, seed)
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

    float result = w->node->GenSingle3D(static_cast<float>(x),
                                         static_cast<float>(y),
                                         static_cast<float>(z), seed);
    return JS_NewFloat64(ctx, result);
}

// genUniformGrid2D(xOffset, yOffset, xSize, ySize, frequency, seed) -> Float32Array
// frequency is used as step size for both axes
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
                               xSize, ySize,
                               step, step, seed);
    return make_float32_array(ctx, output.data(), count);
}

// genUniformGrid3D(xOffset, yOffset, zOffset, xSize, ySize, zSize, frequency, seed) -> Float32Array
static JSValue noise_gen_uniform_grid_3d(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 8)
        return JS_ThrowTypeError(ctx, "genUniformGrid3D(xOffset, yOffset, zOffset, xSize, ySize, zSize, frequency, seed)");

    double xOffset, yOffset, zOffset, frequency;
    int32_t xSize, ySize, zSize, seed;
    if (JS_ToFloat64(ctx, &xOffset, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &yOffset, argv[1])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &zOffset, argv[2])) return JS_EXCEPTION;
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
                               static_cast<float>(xOffset), static_cast<float>(yOffset),
                               static_cast<float>(zOffset),
                               xSize, ySize, zSize,
                               step, step, step, seed);
    return make_float32_array(ctx, output.data(), count);
}

// genTileable2D(xSize, ySize, frequency, seed) -> Float32Array
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

// Constructor: new FastNoise(encodedNodeTree)
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

void installNoise(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);

    if (noise_class_id == 0) {
        JS_NewClassID(rt, &noise_class_id);
        JS_NewClass(rt, noise_class_id, &noise_class_def);
    }

    // Prototype with methods
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "genSingle2D",
        JS_NewCFunction(ctx, noise_gen_single_2d, "genSingle2D", 3));
    JS_SetPropertyStr(ctx, proto, "genSingle3D",
        JS_NewCFunction(ctx, noise_gen_single_3d, "genSingle3D", 4));
    JS_SetPropertyStr(ctx, proto, "genUniformGrid2D",
        JS_NewCFunction(ctx, noise_gen_uniform_grid_2d, "genUniformGrid2D", 6));
    JS_SetPropertyStr(ctx, proto, "genUniformGrid3D",
        JS_NewCFunction(ctx, noise_gen_uniform_grid_3d, "genUniformGrid3D", 8));
    JS_SetPropertyStr(ctx, proto, "genTileable2D",
        JS_NewCFunction(ctx, noise_gen_tileable_2d, "genTileable2D", 4));

    // Constructor
    JSValue ctor = JS_NewCFunction2(ctx, noise_ctor, "FastNoise", 1,
                                     JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, noise_class_id, proto);

    // Register global
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "FastNoise", ctor);
    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
