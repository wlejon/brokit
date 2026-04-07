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

static NoiseWrapper* get_noise(JSContext* ctx, JSValueConst this_val)
{
    return static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, this_val, noise_class_id));
}

// Helper: create a JS FastNoise object wrapping a SmartNode
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
// Node configuration methods — direct casts to concrete FN2 types
// ---------------------------------------------------------------------------

// setSource(otherNode) — works on Fractal and DomainWarp nodes
static JSValue noise_set_source(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "setSource(node)");

    auto* sw = static_cast<NoiseWrapper*>(JS_GetOpaque2(ctx, argv[0], noise_class_id));
    if (!sw)
        return JS_ThrowTypeError(ctx, "setSource: argument must be a FastNoise node");

    // Try Fractal types
    if (auto* fbm = dynamic_cast<FastNoise::FractalFBm*>(w->node.get())) {
        fbm->SetSource(sw->node); return JS_UNDEFINED;
    }
    if (auto* ridged = dynamic_cast<FastNoise::FractalRidged*>(w->node.get())) {
        ridged->SetSource(sw->node); return JS_UNDEFINED;
    }
    // Try DomainWarp types
    if (auto* warp = dynamic_cast<FastNoise::DomainWarp*>(w->node.get())) {
        warp->SetSource(sw->node); return JS_UNDEFINED;
    }
    // Try modifier types (DomainScale etc.)
    if (auto* ds = dynamic_cast<FastNoise::DomainScale*>(w->node.get())) {
        ds->SetSource(sw->node); return JS_UNDEFINED;
    }

    return JS_ThrowTypeError(ctx, "This node type does not accept a source");
}

// setScale(value) — feature scale (1/frequency). Works on ScalableGenerator.
static JSValue noise_set_scale(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setScale(value)");

    double val;
    if (JS_ToFloat64(ctx, &val, argv[0])) return JS_EXCEPTION;

    if (auto* sg = dynamic_cast<FastNoise::ScalableGenerator*>(w->node.get())) {
        sg->SetScale(static_cast<float>(val));
        return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx, "This node type does not have a scale parameter");
}

// setFrequency(value) — convenience: setScale(1 / freq)
static JSValue noise_set_frequency(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setFrequency(value)");

    double val;
    if (JS_ToFloat64(ctx, &val, argv[0])) return JS_EXCEPTION;
    if (val == 0.0) return JS_ThrowRangeError(ctx, "Frequency cannot be zero");

    if (auto* sg = dynamic_cast<FastNoise::ScalableGenerator*>(w->node.get())) {
        sg->SetScale(static_cast<float>(1.0 / val));
        return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx, "This node type does not have a frequency parameter");
}

// setOctaveCount(n) — Fractal types
static JSValue noise_set_octave_count(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setOctaveCount(n)");

    int32_t val;
    if (JS_ToInt32(ctx, &val, argv[0])) return JS_EXCEPTION;

    // Fractal<> is a template — check concrete types
    if (auto* f = dynamic_cast<FastNoise::FractalFBm*>(w->node.get())) {
        f->SetOctaveCount(val); return JS_UNDEFINED;
    }
    if (auto* f = dynamic_cast<FastNoise::FractalRidged*>(w->node.get())) {
        f->SetOctaveCount(val); return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx, "This node type does not have octaves");
}

// setGain(value) — Fractal types
static JSValue noise_set_gain(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setGain(value)");

    double val;
    if (JS_ToFloat64(ctx, &val, argv[0])) return JS_EXCEPTION;

    if (auto* f = dynamic_cast<FastNoise::FractalFBm*>(w->node.get())) {
        f->SetGain(static_cast<float>(val)); return JS_UNDEFINED;
    }
    if (auto* f = dynamic_cast<FastNoise::FractalRidged*>(w->node.get())) {
        f->SetGain(static_cast<float>(val)); return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx, "This node type does not have a gain parameter");
}

// setLacunarity(value) — Fractal types
static JSValue noise_set_lacunarity(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setLacunarity(value)");

    double val;
    if (JS_ToFloat64(ctx, &val, argv[0])) return JS_EXCEPTION;

    if (auto* f = dynamic_cast<FastNoise::FractalFBm*>(w->node.get())) {
        f->SetLacunarity(static_cast<float>(val)); return JS_UNDEFINED;
    }
    if (auto* f = dynamic_cast<FastNoise::FractalRidged*>(w->node.get())) {
        f->SetLacunarity(static_cast<float>(val)); return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx, "This node type does not have a lacunarity parameter");
}

// setWeightedStrength(value) — Fractal types
static JSValue noise_set_weighted_strength(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setWeightedStrength(value)");

    double val;
    if (JS_ToFloat64(ctx, &val, argv[0])) return JS_EXCEPTION;

    if (auto* f = dynamic_cast<FastNoise::FractalFBm*>(w->node.get())) {
        f->SetWeightedStrength(static_cast<float>(val)); return JS_UNDEFINED;
    }
    if (auto* f = dynamic_cast<FastNoise::FractalRidged*>(w->node.get())) {
        f->SetWeightedStrength(static_cast<float>(val)); return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx, "This node type does not have a weighted strength parameter");
}

// setWarpAmplitude(value) — DomainWarp types
static JSValue noise_set_warp_amplitude(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv)
{
    auto* w = get_noise(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setWarpAmplitude(value)");

    double val;
    if (JS_ToFloat64(ctx, &val, argv[0])) return JS_EXCEPTION;

    if (auto* dw = dynamic_cast<FastNoise::DomainWarp*>(w->node.get())) {
        dw->SetWarpAmplitude(static_cast<float>(val)); return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx, "This node type does not have a warp amplitude parameter");
}

// ---------------------------------------------------------------------------
// Constructor: new FastNoise(encodedNodeTree) — backwards compat
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
// Factory functions: FastNoise.Simplex(), FastNoise.FractalFBm(), etc.
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

    if (noise_class_id == 0) {
        JS_NewClassID(rt, &noise_class_id);
        JS_NewClass(rt, noise_class_id, &noise_class_def);
    }

    // Prototype: generation + configuration methods
    JSValue proto = JS_NewObject(ctx);

    // Generation
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

    // Configuration
    JS_SetPropertyStr(ctx, proto, "setSource",
        JS_NewCFunction(ctx, noise_set_source, "setSource", 1));
    JS_SetPropertyStr(ctx, proto, "setScale",
        JS_NewCFunction(ctx, noise_set_scale, "setScale", 1));
    JS_SetPropertyStr(ctx, proto, "setFrequency",
        JS_NewCFunction(ctx, noise_set_frequency, "setFrequency", 1));
    JS_SetPropertyStr(ctx, proto, "setOctaveCount",
        JS_NewCFunction(ctx, noise_set_octave_count, "setOctaveCount", 1));
    JS_SetPropertyStr(ctx, proto, "setGain",
        JS_NewCFunction(ctx, noise_set_gain, "setGain", 1));
    JS_SetPropertyStr(ctx, proto, "setLacunarity",
        JS_NewCFunction(ctx, noise_set_lacunarity, "setLacunarity", 1));
    JS_SetPropertyStr(ctx, proto, "setWeightedStrength",
        JS_NewCFunction(ctx, noise_set_weighted_strength, "setWeightedStrength", 1));
    JS_SetPropertyStr(ctx, proto, "setWarpAmplitude",
        JS_NewCFunction(ctx, noise_set_warp_amplitude, "setWarpAmplitude", 1));

    // Constructor (encoded string — backwards compat)
    JSValue ctor = JS_NewCFunction2(ctx, noise_ctor, "FastNoise", 1,
                                     JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, noise_class_id, proto);

    // Static factories — coherent noise
    JS_SetPropertyStr(ctx, ctor, "Simplex",
        JS_NewCFunction(ctx, noise_factory<FastNoise::Simplex>, "Simplex", 0));
    JS_SetPropertyStr(ctx, ctor, "SuperSimplex",
        JS_NewCFunction(ctx, noise_factory<FastNoise::SuperSimplex>, "SuperSimplex", 0));
    JS_SetPropertyStr(ctx, ctor, "Perlin",
        JS_NewCFunction(ctx, noise_factory<FastNoise::Perlin>, "Perlin", 0));
    JS_SetPropertyStr(ctx, ctor, "Value",
        JS_NewCFunction(ctx, noise_factory<FastNoise::Value>, "Value", 0));

    // Cellular
    JS_SetPropertyStr(ctx, ctor, "CellularValue",
        JS_NewCFunction(ctx, noise_factory<FastNoise::CellularValue>, "CellularValue", 0));
    JS_SetPropertyStr(ctx, ctor, "CellularDistance",
        JS_NewCFunction(ctx, noise_factory<FastNoise::CellularDistance>, "CellularDistance", 0));

    // Fractal
    JS_SetPropertyStr(ctx, ctor, "FractalFBm",
        JS_NewCFunction(ctx, noise_factory<FastNoise::FractalFBm>, "FractalFBm", 0));
    JS_SetPropertyStr(ctx, ctor, "FractalRidged",
        JS_NewCFunction(ctx, noise_factory<FastNoise::FractalRidged>, "FractalRidged", 0));

    // Domain warp
    JS_SetPropertyStr(ctx, ctor, "DomainWarpGradient",
        JS_NewCFunction(ctx, noise_factory<FastNoise::DomainWarpGradient>, "DomainWarpGradient", 0));

    // Register global
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "FastNoise", ctor);
    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
