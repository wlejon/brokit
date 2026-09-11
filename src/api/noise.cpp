#include "api/api.h"
#include "api/arg_reader.h"
#include "api/host_class.h"
#include "api/object_builder.h"

#include <FastNoise/FastNoise.h>
#include <FastNoise/Metadata.h>

#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <span>

namespace brokit::api {

struct NoiseWrapper {
    FastNoise::SmartNode<> node;
};

static HostClass g_fastNoiseClass;

static void fastNoiseDtor(void* p)
{
    delete static_cast<NoiseWrapper*>(p);
}

static NoiseWrapper* getNoise(bronze::Value v)
{
    return static_cast<NoiseWrapper*>(ev::handleData(v));
}

static bronze::Value wrapNode(FastNoise::SmartNode<> node)
{
    if (!node)
        return ev::throwTypeError("Failed to create FastNoise node");

    auto* w = new NoiseWrapper{std::move(node)};
    return g_fastNoiseClass.make(w, fastNoiseDtor);
}

static bronze::Value make_float32_array(const float* data, size_t count)
{
    size_t byte_len = count * sizeof(float);
    bronze::Value ab = ev::createArrayBuffer(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data), byte_len));
    return ev::createTypedArrayView(elements::Float32, ab, 0, static_cast<uint32_t>(count));
}

static bool resolve_f32(bronze::Value v, const char* name,
                        float** out, size_t* count)
{
    auto info = ev::typedArrayInfo(v);
    if (!info.data || info.elementKind != elements::Float32) {
        ev::throwTypeError(std::string(name) + " must be a Float32Array");
        return false;
    }
    *out   = reinterpret_cast<float*>(info.data);
    *count = info.elementCount;
    return true;
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
static bronze::Value make_factory_node(bronze::Value, std::span<const bronze::Value>)
{
    auto node = FastNoise::New<T>();
    return wrapNode(std::move(node));
}

static bronze::Value fast_noise_create(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty() || !ev::isString(args[0])) return ev::throwTypeError("FastNoise.create(typeName)");
    std::string typeName = ev::toUtf8(args[0]);
    for (const auto* meta : FastNoise::Metadata::GetAll()) {
        if (meta && typeName == meta->name) {
            return wrapNode(meta->CreateNode());
        }
    }
    return ev::throwError("Unknown FastNoise type '" + typeName + "'");
}

static bronze::Value fast_noise_types(bronze::Value, std::span<const bronze::Value>)
{
    std::vector<bronze::Value> list;
    for (const auto* meta : FastNoise::Metadata::GetAll()) {
        if (!meta) continue;
        ObjectBuilder entry;
        entry.set("name", meta->name);
        std::vector<bronze::Value> groups;
        for (size_t gi = 0; gi < meta->groups.size(); gi++) {
            groups.push_back(ev::fromUtf8(meta->groups[gi]));
        }
        entry.set("groups", hostArrayOf(groups));
        list.push_back(entry.build());
    }
    return hostArrayOf(list);
}

static bronze::Value fast_noise_set(bronze::Value thisVal, std::span<const bronze::Value> args)
{
    auto* w = getNoise(thisVal);
    if (!w) return ev::throwTypeError("receiver is not a FastNoise instance");
    if (args.size() < 2 || !ev::isString(args[0])) return ev::throwTypeError("set(name, value)");
    std::string name = ev::toUtf8(args[0]);
    const auto& meta = w->node->GetMetadata();
    bronze::Value val = args[1];

    for (const auto& mv : meta.memberVariables) {
        if (!memberNameMatches(name.c_str(), mv)) continue;
        if (mv.type == FastNoise::Metadata::MemberVariable::EFloat) {
            if (!ev::isDouble(val)) return ev::throwTypeError("expected number");
            double d = ev::toDouble(val);
            return mv.setFunc(w->node.get(), FastNoise::Metadata::MemberVariable::ValueUnion(static_cast<float>(d)))
                ? ev::undefined() : ev::throwTypeError("Failed to set variable");
        }
        if (mv.type == FastNoise::Metadata::MemberVariable::EInt) {
            if (!ev::isDouble(val)) return ev::throwTypeError("expected number");
            int32_t i = static_cast<int32_t>(ev::toDouble(val));
            return mv.setFunc(w->node.get(), FastNoise::Metadata::MemberVariable::ValueUnion(static_cast<int>(i)))
                ? ev::undefined() : ev::throwTypeError("Failed to set variable");
        }
        if (mv.type == FastNoise::Metadata::MemberVariable::EEnum) {
            int32_t i = -1;
            if (ev::isDouble(val)) {
                i = static_cast<int32_t>(ev::toDouble(val));
            } else if (ev::isString(val)) {
                std::string es = ev::toUtf8(val);
                for (size_t ei = 0; ei < mv.enumNames.size(); ei++) {
                    if (es == mv.enumNames[ei]) { i = static_cast<int32_t>(ei); break; }
                }
                if (i < 0) return ev::throwRangeError("Unknown enum value");
            } else return ev::throwTypeError("Enum expects int or string");
            return mv.setFunc(w->node.get(), FastNoise::Metadata::MemberVariable::ValueUnion(static_cast<int>(i)))
                ? ev::undefined() : ev::throwTypeError("Failed to set enum");
        }
        return ev::throwTypeError("Unknown variable type");
    }
    for (const auto& mn : meta.memberNodeLookups) {
        if (!memberNameMatches(name.c_str(), mn)) continue;
        auto* sw = getNoise(val);
        if (!sw) return ev::throwTypeError("Node input requires a FastNoise node");
        return mn.setFunc(w->node.get(), sw->node)
            ? ev::undefined() : ev::throwTypeError("Failed to set node source (type mismatch)");
    }
    for (const auto& mh : meta.memberHybrids) {
        if (!memberNameMatches(name.c_str(), mh)) continue;
        auto* sw = getNoise(val);
        if (sw) return mh.setNodeFunc(w->node.get(), sw->node)
            ? ev::undefined() : ev::throwTypeError("Failed to set hybrid node source");
        if (!ev::isDouble(val)) return ev::throwTypeError("expected number");
        double d = ev::toDouble(val);
        return mh.setValueFunc(w->node.get(), static_cast<float>(d))
            ? ev::undefined() : ev::throwTypeError("Failed to set hybrid value");
    }
    return ev::throwError("No member '" + name + "' on this node type");
}

static bronze::Value fast_noise_get_members(bronze::Value thisVal, std::span<const bronze::Value>)
{
    auto* w = getNoise(thisVal);
    if (!w) return ev::throwTypeError("receiver is not a FastNoise instance");
    const auto& meta = w->node->GetMetadata();
    ObjectBuilder result;
    result.set("type", meta.name);

    std::vector<bronze::Value> vars;
    for (size_t i = 0; i < meta.memberVariables.size(); i++) {
        const auto& mv = meta.memberVariables[i];
        ObjectBuilder entry;
        entry.set("name", mv.name);
        entry.set("type", mv.type == FastNoise::Metadata::MemberVariable::EFloat ? "float" :
                          mv.type == FastNoise::Metadata::MemberVariable::EInt ? "int" : "enum");
        if (mv.type == FastNoise::Metadata::MemberVariable::EEnum) {
            std::vector<bronze::Value> names;
            for (size_t ei = 0; ei < mv.enumNames.size(); ei++) {
                names.push_back(ev::fromUtf8(mv.enumNames[ei]));
            }
            entry.set("enumValues", hostArrayOf(names));
        }
        vars.push_back(entry.build());
    }
    result.set("variables", hostArrayOf(vars));

    std::vector<bronze::Value> nodes;
    for (size_t i = 0; i < meta.memberNodeLookups.size(); i++) {
        ObjectBuilder entry;
        entry.set("name", meta.memberNodeLookups[i].name);
        nodes.push_back(entry.build());
    }
    result.set("nodes", hostArrayOf(nodes));

    std::vector<bronze::Value> hybrids;
    for (size_t i = 0; i < meta.memberHybrids.size(); i++) {
        ObjectBuilder entry;
        entry.set("name", meta.memberHybrids[i].name);
        entry.set("default", static_cast<double>(meta.memberHybrids[i].valueDefault));
        hybrids.push_back(entry.build());
    }
    result.set("hybrids", hostArrayOf(hybrids));

    return result.build();
}

static bronze::Value fast_noise_gen_single2_d(bronze::Value thisVal, std::span<const bronze::Value> args)
{
    auto* w = getNoise(thisVal);
    if (!w) return ev::throwTypeError("receiver is not a FastNoise instance");
    if (args.size() < 3)
        return ev::throwTypeError("genSingle2D(x, y, seed)");

    ArgReader reader(args);
    double x = reader.getDouble(0, 0.0);
    double y = reader.getDouble(1, 0.0);
    int32_t seed = reader.getInt(2, 0);

    return ev::fromDouble(w->node->GenSingle2D(
        static_cast<float>(x), static_cast<float>(y), seed));
}

static bronze::Value fast_noise_gen_single3_d(bronze::Value thisVal, std::span<const bronze::Value> args)
{
    auto* w = getNoise(thisVal);
    if (!w) return ev::throwTypeError("receiver is not a FastNoise instance");
    if (args.size() < 4)
        return ev::throwTypeError("genSingle3D(x, y, z, seed)");

    ArgReader reader(args);
    double x = reader.getDouble(0, 0.0);
    double y = reader.getDouble(1, 0.0);
    double z = reader.getDouble(2, 0.0);
    int32_t seed = reader.getInt(3, 0);

    return ev::fromDouble(w->node->GenSingle3D(
        static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(z), seed));
}

static bronze::Value fast_noise_gen_uniform_grid2_d(bronze::Value thisVal, std::span<const bronze::Value> args)
{
    auto* w = getNoise(thisVal);
    if (!w) return ev::throwTypeError("receiver is not a FastNoise instance");
    if (args.size() < 6)
        return ev::throwTypeError("genUniformGrid2D(xOffset, yOffset, xSize, ySize, frequency, seed)");

    ArgReader reader(args);
    double xOffset = reader.getDouble(0, 0.0);
    double yOffset = reader.getDouble(1, 0.0);
    int32_t xSize = reader.getInt(2, 0);
    int32_t ySize = reader.getInt(3, 0);
    double frequency = reader.getDouble(4, 0.0);
    int32_t seed = reader.getInt(5, 0);

    if (xSize <= 0 || ySize <= 0)
        return ev::throwRangeError("Grid dimensions must be positive");
    
    float step = static_cast<float>(frequency);
    size_t count = static_cast<size_t>(xSize) * static_cast<size_t>(ySize);
    std::vector<float> output(count);
    w->node->GenUniformGrid2D(output.data(),
                               static_cast<float>(xOffset), static_cast<float>(yOffset),
                               xSize, ySize, step, step, seed);
    return make_float32_array(output.data(), count);
}

static bronze::Value fast_noise_gen_uniform_grid2_d_into(bronze::Value thisVal, std::span<const bronze::Value> args)
{
    auto* w = getNoise(thisVal);
    if (!w) return ev::throwTypeError("receiver is not a FastNoise instance");
    if (args.size() < 7)
        return ev::throwTypeError("genUniformGrid2DInto(dest, xOffset, yOffset, xSize, ySize, frequency, seed)");

    float* dest = nullptr;
    size_t n_dest = 0;
    if (!resolve_f32(args[0], "dest", &dest, &n_dest)) return ev::undefined();

    ArgReader reader(args);
    double xOffset = reader.getDouble(1, 0.0);
    double yOffset = reader.getDouble(2, 0.0);
    int32_t xSize = reader.getInt(3, 0);
    int32_t ySize = reader.getInt(4, 0);
    double frequency = reader.getDouble(5, 0.0);
    int32_t seed = reader.getInt(6, 0);

    if (xSize <= 0 || ySize <= 0)
        return ev::throwRangeError("Grid dimensions must be positive");
    
    size_t count = static_cast<size_t>(xSize) * static_cast<size_t>(ySize);
    if (n_dest < count)
        return ev::throwRangeError("dest too small: " + std::to_string(count) + " floats required");
    
    float step = static_cast<float>(frequency);
    w->node->GenUniformGrid2D(dest,
                               static_cast<float>(xOffset), static_cast<float>(yOffset),
                               xSize, ySize, step, step, seed);
    return ev::undefined();
}

static bronze::Value fast_noise_gen_uniform_grid3_d(bronze::Value thisVal, std::span<const bronze::Value> args)
{
    auto* w = getNoise(thisVal);
    if (!w) return ev::throwTypeError("receiver is not a FastNoise instance");
    if (args.size() < 8)
        return ev::throwTypeError("genUniformGrid3D(xOff, yOff, zOff, xSize, ySize, zSize, frequency, seed)");

    ArgReader reader(args);
    double xOff = reader.getDouble(0, 0.0);
    double yOff = reader.getDouble(1, 0.0);
    double zOff = reader.getDouble(2, 0.0);
    int32_t xSize = reader.getInt(3, 0);
    int32_t ySize = reader.getInt(4, 0);
    int32_t zSize = reader.getInt(5, 0);
    double frequency = reader.getDouble(6, 0.0);
    int32_t seed = reader.getInt(7, 0);

    if (xSize <= 0 || ySize <= 0 || zSize <= 0)
        return ev::throwRangeError("Grid dimensions must be positive");
    
    float step = static_cast<float>(frequency);
    size_t count = static_cast<size_t>(xSize) * static_cast<size_t>(ySize) * static_cast<size_t>(zSize);
    std::vector<float> output(count);
    w->node->GenUniformGrid3D(output.data(),
                               static_cast<float>(xOff), static_cast<float>(yOff),
                               static_cast<float>(zOff),
                               xSize, ySize, zSize,
                               step, step, step, seed);
    return make_float32_array(output.data(), count);
}

static bronze::Value fast_noise_gen_uniform_grid3_d_into(bronze::Value thisVal, std::span<const bronze::Value> args)
{
    auto* w = getNoise(thisVal);
    if (!w) return ev::throwTypeError("receiver is not a FastNoise instance");
    if (args.size() < 9)
        return ev::throwTypeError("genUniformGrid3DInto(dest, xOff, yOff, zOff, xSize, ySize, zSize, frequency, seed)");

    float* dest = nullptr;
    size_t n_dest = 0;
    if (!resolve_f32(args[0], "dest", &dest, &n_dest)) return ev::undefined();

    ArgReader reader(args);
    double xOff = reader.getDouble(1, 0.0);
    double yOff = reader.getDouble(2, 0.0);
    double zOff = reader.getDouble(3, 0.0);
    int32_t xSize = reader.getInt(4, 0);
    int32_t ySize = reader.getInt(5, 0);
    int32_t zSize = reader.getInt(6, 0);
    double frequency = reader.getDouble(7, 0.0);
    int32_t seed = reader.getInt(8, 0);

    if (xSize <= 0 || ySize <= 0 || zSize <= 0)
        return ev::throwRangeError("Grid dimensions must be positive");
    
    size_t count = static_cast<size_t>(xSize) * static_cast<size_t>(ySize) * static_cast<size_t>(zSize);
    if (n_dest < count)
        return ev::throwRangeError("dest too small: " + std::to_string(count) + " floats required");
    
    float step = static_cast<float>(frequency);
    w->node->GenUniformGrid3D(dest,
                               static_cast<float>(xOff), static_cast<float>(yOff),
                               static_cast<float>(zOff),
                               xSize, ySize, zSize,
                               step, step, step, seed);
    return ev::undefined();
}

static bronze::Value fast_noise_gen_position_array2_d(bronze::Value thisVal, std::span<const bronze::Value> args)
{
    auto* w = getNoise(thisVal);
    if (!w) return ev::throwTypeError("receiver is not a FastNoise instance");
    if (args.size() < 6)
        return ev::throwTypeError("genPositionArray2D(dest, xs, ys, x_off, y_off, seed)");

    float* dest = nullptr;
    size_t n_dest = 0;
    if (!resolve_f32(args[0], "dest", &dest, &n_dest)) return ev::undefined();
    float* xs = nullptr;
    size_t n_xs = 0;
    if (!resolve_f32(args[1], "xs", &xs, &n_xs)) return ev::undefined();
    float* ys = nullptr;
    size_t n_ys = 0;
    if (!resolve_f32(args[2], "ys", &ys, &n_ys)) return ev::undefined();

    ArgReader reader(args);
    double x_off = reader.getDouble(3, 0.0);
    double y_off = reader.getDouble(4, 0.0);
    int32_t seed = reader.getInt(5, 0);

    size_t count = n_xs < n_ys ? n_xs : n_ys;
    if (count == 0) return ev::undefined();
    if (n_dest < count)
        return ev::throwRangeError("dest too small: " + std::to_string(count) + " floats required");
    if (count > static_cast<size_t>(INT32_MAX))
        return ev::throwRangeError("position count exceeds INT_MAX");
    
    w->node->GenPositionArray2D(dest, static_cast<int>(count), xs, ys,
                                 static_cast<float>(x_off), static_cast<float>(y_off),
                                 seed);
    return ev::undefined();
}

static bronze::Value fast_noise_gen_position_array3_d(bronze::Value thisVal, std::span<const bronze::Value> args)
{
    auto* w = getNoise(thisVal);
    if (!w) return ev::throwTypeError("receiver is not a FastNoise instance");
    if (args.size() < 8)
        return ev::throwTypeError("genPositionArray3D(dest, xs, ys, zs, x_off, y_off, z_off, seed)");

    float* dest = nullptr;
    size_t n_dest = 0;
    if (!resolve_f32(args[0], "dest", &dest, &n_dest)) return ev::undefined();
    float* xs = nullptr;
    size_t n_xs = 0;
    if (!resolve_f32(args[1], "xs", &xs, &n_xs)) return ev::undefined();
    float* ys = nullptr;
    size_t n_ys = 0;
    if (!resolve_f32(args[2], "ys", &ys, &n_ys)) return ev::undefined();
    float* zs = nullptr;
    size_t n_zs = 0;
    if (!resolve_f32(args[3], "zs", &zs, &n_zs)) return ev::undefined();

    ArgReader reader(args);
    double x_off = reader.getDouble(4, 0.0);
    double y_off = reader.getDouble(5, 0.0);
    double z_off = reader.getDouble(6, 0.0);
    int32_t seed = reader.getInt(7, 0);

    size_t count = n_xs;
    if (n_ys < count) count = n_ys;
    if (n_zs < count) count = n_zs;
    if (count == 0) return ev::undefined();
    if (n_dest < count)
        return ev::throwRangeError("dest too small: " + std::to_string(count) + " floats required");
    if (count > static_cast<size_t>(INT32_MAX))
        return ev::throwRangeError("position count exceeds INT_MAX");
    
    w->node->GenPositionArray3D(dest, static_cast<int>(count), xs, ys, zs,
                                 static_cast<float>(x_off), static_cast<float>(y_off),
                                 static_cast<float>(z_off), seed);
    return ev::undefined();
}

static bronze::Value fast_noise_gen_tileable2_d(bronze::Value thisVal, std::span<const bronze::Value> args)
{
    auto* w = getNoise(thisVal);
    if (!w) return ev::throwTypeError("receiver is not a FastNoise instance");
    if (args.size() < 4)
        return ev::throwTypeError("genTileable2D(xSize, ySize, frequency, seed)");

    ArgReader reader(args);
    int32_t xSize = reader.getInt(0, 0);
    int32_t ySize = reader.getInt(1, 0);
    double frequency = reader.getDouble(2, 0.0);
    int32_t seed = reader.getInt(3, 0);

    if (xSize <= 0 || ySize <= 0)
        return ev::throwRangeError("Grid dimensions must be positive");
    
    float step = static_cast<float>(frequency);
    size_t count = static_cast<size_t>(xSize) * static_cast<size_t>(ySize);
    std::vector<float> output(count);
    w->node->GenTileable2D(output.data(), xSize, ySize, step, step, seed);
    return make_float32_array(output.data(), count);
}

static bronze::Value js_fast_noise_constructor(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty() || !ev::isString(args[0]))
        return ev::throwTypeError("FastNoise: expected encoded node tree string");
    
    std::string encoded = ev::toUtf8(args[0]);
    auto node = FastNoise::NewFromEncodedNodeTree(encoded.c_str());
    if (!node)
        return ev::throwTypeError("FastNoise: invalid encoded node tree");
    
    auto* w = new NoiseWrapper{std::move(node)};
    return g_fastNoiseClass.make(w, fastNoiseDtor);
}

void installNoise()
{
    g_fastNoiseClass.install("FastNoise", 1, js_fast_noise_constructor, [](ObjectBuilder& proto) {
        proto.def("set", 2, fast_noise_set);
        proto.def("getMembers", 0, fast_noise_get_members);
        proto.def("genSingle2D", 3, fast_noise_gen_single2_d);
        proto.def("genSingle3D", 4, fast_noise_gen_single3_d);
        proto.def("genUniformGrid2D", 6, fast_noise_gen_uniform_grid2_d);
        proto.def("genUniformGrid2DInto", 7, fast_noise_gen_uniform_grid2_d_into);
        proto.def("genUniformGrid3D", 8, fast_noise_gen_uniform_grid3_d);
        proto.def("genUniformGrid3DInto", 9, fast_noise_gen_uniform_grid3_d_into);
        proto.def("genPositionArray2D", 6, fast_noise_gen_position_array2_d);
        proto.def("genPositionArray3D", 8, fast_noise_gen_position_array3_d);
        proto.def("genTileable2D", 4, fast_noise_gen_tileable2_d);
    });

    bronze::Value ctor = g_fastNoiseClass.constructor();
    ev::setProperty(ctor, "create", ev::makeFunction(fast_noise_create, 1, "create"));
    ev::setProperty(ctor, "types", ev::makeFunction(fast_noise_types, 0, "types"));
    ev::setProperty(ctor, "Simplex", ev::makeFunction(make_factory_node<FastNoise::Simplex>, 0, "Simplex"));
    ev::setProperty(ctor, "SuperSimplex", ev::makeFunction(make_factory_node<FastNoise::SuperSimplex>, 0, "SuperSimplex"));
    ev::setProperty(ctor, "Perlin", ev::makeFunction(make_factory_node<FastNoise::Perlin>, 0, "Perlin"));
    ev::setProperty(ctor, "Value", ev::makeFunction(make_factory_node<FastNoise::Value>, 0, "Value"));
    ev::setProperty(ctor, "CellularValue", ev::makeFunction(make_factory_node<FastNoise::CellularValue>, 0, "CellularValue"));
    ev::setProperty(ctor, "CellularDistance", ev::makeFunction(make_factory_node<FastNoise::CellularDistance>, 0, "CellularDistance"));
    ev::setProperty(ctor, "CellularLookup", ev::makeFunction(make_factory_node<FastNoise::CellularLookup>, 0, "CellularLookup"));
    ev::setProperty(ctor, "FractalFBm", ev::makeFunction(make_factory_node<FastNoise::FractalFBm>, 0, "FractalFBm"));
    ev::setProperty(ctor, "FractalRidged", ev::makeFunction(make_factory_node<FastNoise::FractalRidged>, 0, "FractalRidged"));
    ev::setProperty(ctor, "DomainWarpGradient", ev::makeFunction(make_factory_node<FastNoise::DomainWarpGradient>, 0, "DomainWarpGradient"));
}

} // namespace brokit::api
