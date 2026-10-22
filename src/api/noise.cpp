#include "api/api.h"
#include "api/arg_reader.h"
#include "api/host_class.h"
#include "api/object_builder.h"

#include <FastNoise/FastNoise.h>
#include <FastNoise/Metadata.h>

#include <memory>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <span>

namespace brokit::api {

// A node and its tree as data (FastNoise's NodeData, mirroring every set),
// so encode() can hand the tree on as the string NewFromEncodedNodeTree
// reads: to another thread or process, or to a file. A source's data is
// held by the node it is set on.
struct NoiseWrapper {
    FastNoise::SmartNode<> node;
    std::shared_ptr<FastNoise::NodeData> data;
    std::vector<std::shared_ptr<FastNoise::NodeData>> lookups, hybrids;   // the sources' data, a slot each
};

static HostClass g_fastNoiseClass;

static void fastNoiseDtor(void* p)
{
    delete static_cast<NoiseWrapper*>(p);
}

static NoiseWrapper* getNoise(bronze::Value v)
{
    // Brand-checked (host_class.cpp): a Blob or CryptoKey passed as a
    // FastNoise is refused, not reinterpreted.
    return static_cast<NoiseWrapper*>(g_fastNoiseClass.unwrap(v));
}

static bronze::Value wrap(NoiseWrapper* w)
{
    w->lookups.resize(w->data->nodeLookups.size());
    w->hybrids.resize(w->data->hybrids.size());
    return g_fastNoiseClass.make(w, fastNoiseDtor);
}

static bronze::Value wrapNode(FastNoise::SmartNode<> node)
{
    if (!node)
        return ev::throwTypeError("Failed to create FastNoise node");

    // The node takes its metadata's defaults, the ones its data starts
    // with: a class's own default can differ (DistanceToPoint's distance
    // function is EuclideanSquared, its metadata's Euclidean), and the node
    // here must make the noise its encoding decodes to.
    const auto& meta = node->GetMetadata();
    auto data = std::make_shared<FastNoise::NodeData>(&meta);
    for (size_t vi = 0; vi < meta.memberVariables.size(); vi++)
        meta.memberVariables[vi].setFunc(node.get(), data->variables[vi]);
    for (size_t hi = 0; hi < meta.memberHybrids.size(); hi++)
        meta.memberHybrids[hi].setValueFunc(node.get(), data->hybrids[hi].second);
    return wrap(new NoiseWrapper{std::move(node), std::move(data), {}, {}});
}

static bronze::Value make_float32_array(const float* data, size_t count)
{
    size_t byte_len = count * sizeof(float);
    ev::Persistent ab{ev::createArrayBuffer(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data), byte_len))};
    return ev::createTypedArrayView(elements::Float32, ab.get(), 0, static_cast<uint32_t>(count));
}

// Largest grid a generator allocates: the result becomes one Float32Array,
// and bronze caps a buffer at 256 MiB.
static constexpr size_t kMaxGridFloats = size_t{1} << 26;

// xSize * ySize (* zSize) floats, multiplied without overflow and bounded by
// `cap`. Three script int32s multiply past 2^64, and a product that wrapped
// small would pass a size check FastNoise then writes far beyond; bounding
// it also keeps FastNoise's own int arithmetic in range.
static bool gridCount(std::initializer_list<int32_t> dims, size_t cap, size_t* out)
{
    size_t product = 1;
    for (int32_t d : dims) {
        if (d <= 0) return false;
        if (product > cap / static_cast<size_t>(d)) return false;
        product *= static_cast<size_t>(d);
    }
    *out = product;
    return true;
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
    ArrayBuilder list;
    for (const auto* meta : FastNoise::Metadata::GetAll()) {
        if (!meta) continue;
        ObjectBuilder entry;
        entry.set("name", meta->name);
        ArrayBuilder groups;
        for (size_t gi = 0; gi < meta->groups.size(); gi++) {
            groups.push(ev::fromUtf8(meta->groups[gi]));
        }
        entry.set("groups", groups.get());
        list.push(entry.build());
    }
    return list.get();
}

static bronze::Value fast_noise_set(bronze::Value thisVal, std::span<const bronze::Value> args)
{
    auto* w = getNoise(thisVal);
    if (!w) return ev::throwTypeError("receiver is not a FastNoise instance");
    if (args.size() < 2 || !ev::isString(args[0])) return ev::throwTypeError("set(name, value)");
    std::string name = ev::toUtf8(args[0]);
    const auto& meta = w->node->GetMetadata();
    bronze::Value val = args[1];
    using Value = FastNoise::Metadata::MemberVariable::ValueUnion;
    FastNoise::NodeData& data = *w->data;

    for (size_t vi = 0; vi < meta.memberVariables.size(); vi++) {
        const auto& mv = meta.memberVariables[vi];
        if (!memberNameMatches(name.c_str(), mv)) continue;
        const auto setVariable = [&](Value v) {
            if (!mv.setFunc(w->node.get(), v)) return false;
            data.variables[vi] = v;
            return true;
        };
        if (mv.type == FastNoise::Metadata::MemberVariable::EFloat) {
            if (!ev::isDouble(val)) return ev::throwTypeError("expected number");
            double d = ev::toDouble(val);
            return setVariable(Value(static_cast<float>(d)))
                ? ev::undefined() : ev::throwTypeError("Failed to set variable");
        }
        if (mv.type == FastNoise::Metadata::MemberVariable::EInt) {
            if (!ev::isDouble(val)) return ev::throwTypeError("expected number");
            int32_t i = saturateI32(ev::toDouble(val));
            return setVariable(Value(static_cast<int>(i)))
                ? ev::undefined() : ev::throwTypeError("Failed to set variable");
        }
        if (mv.type == FastNoise::Metadata::MemberVariable::EEnum) {
            int32_t i = -1;
            if (ev::isDouble(val)) {
                i = saturateI32(ev::toDouble(val));
                if (i < 0 || static_cast<size_t>(i) >= mv.enumNames.size())
                    return ev::throwRangeError("Enum index out of range");
            } else if (ev::isString(val)) {
                std::string es = ev::toUtf8(val);
                for (size_t ei = 0; ei < mv.enumNames.size(); ei++) {
                    if (es == mv.enumNames[ei]) { i = static_cast<int32_t>(ei); break; }
                }
                if (i < 0) return ev::throwRangeError("Unknown enum value");
            } else return ev::throwTypeError("Enum expects int or string");
            return setVariable(Value(static_cast<int>(i)))
                ? ev::undefined() : ev::throwTypeError("Failed to set enum");
        }
        return ev::throwTypeError("Unknown variable type");
    }
    for (size_t ni = 0; ni < meta.memberNodeLookups.size(); ni++) {
        const auto& mn = meta.memberNodeLookups[ni];
        if (!memberNameMatches(name.c_str(), mn)) continue;
        auto* sw = getNoise(val);
        if (!sw) return ev::throwTypeError("Node input requires a FastNoise node");
        if (!mn.setFunc(w->node.get(), sw->node))
            return ev::throwTypeError("Failed to set node source (type mismatch)");
        data.nodeLookups[ni] = sw->data.get();
        w->lookups[ni] = sw->data;
        return ev::undefined();
    }
    for (size_t hi = 0; hi < meta.memberHybrids.size(); hi++) {
        const auto& mh = meta.memberHybrids[hi];
        if (!memberNameMatches(name.c_str(), mh)) continue;
        auto* sw = getNoise(val);
        if (sw) {
            if (!mh.setNodeFunc(w->node.get(), sw->node))
                return ev::throwTypeError("Failed to set hybrid node source");
            data.hybrids[hi].first = sw->data.get();
            w->hybrids[hi] = sw->data;
            return ev::undefined();
        }
        if (!ev::isDouble(val)) return ev::throwTypeError("expected number");
        double d = ev::toDouble(val);
        if (!mh.setValueFunc(w->node.get(), static_cast<float>(d)))
            return ev::throwTypeError("Failed to set hybrid value");
        data.hybrids[hi] = {nullptr, static_cast<float>(d)};
        w->hybrids[hi].reset();
        return ev::undefined();
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

    ArrayBuilder vars;
    for (size_t i = 0; i < meta.memberVariables.size(); i++) {
        const auto& mv = meta.memberVariables[i];
        ObjectBuilder entry;
        entry.set("name", mv.name);
        entry.set("type", mv.type == FastNoise::Metadata::MemberVariable::EFloat ? "float" :
                          mv.type == FastNoise::Metadata::MemberVariable::EInt ? "int" : "enum");
        if (mv.type == FastNoise::Metadata::MemberVariable::EEnum) {
            ArrayBuilder names;
            for (size_t ei = 0; ei < mv.enumNames.size(); ei++) {
                names.push(ev::fromUtf8(mv.enumNames[ei]));
            }
            entry.set("enumValues", names.get());
        }
        vars.push(entry.build());
    }
    result.set("variables", vars.get());

    ArrayBuilder nodes;
    for (size_t i = 0; i < meta.memberNodeLookups.size(); i++) {
        ObjectBuilder entry;
        entry.set("name", meta.memberNodeLookups[i].name);
        nodes.push(entry.build());
    }
    result.set("nodes", nodes.get());

    ArrayBuilder hybrids;
    for (size_t i = 0; i < meta.memberHybrids.size(); i++) {
        ObjectBuilder entry;
        entry.set("name", meta.memberHybrids[i].name);
        entry.set("default", static_cast<double>(meta.memberHybrids[i].valueDefault));
        hybrids.push(entry.build());
    }
    result.set("hybrids", hybrids.get());

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
    size_t count = 0;
    if (!gridCount({xSize, ySize}, kMaxGridFloats, &count))
        return ev::throwRangeError("genUniformGrid2D: grid too large");
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
    
    size_t count = 0;
    if (!gridCount({xSize, ySize}, n_dest, &count))
        return ev::throwRangeError("dest too small: " + std::to_string(xSize) + " x " +
                                   std::to_string(ySize) + " floats required");

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
    size_t count = 0;
    if (!gridCount({xSize, ySize, zSize}, kMaxGridFloats, &count))
        return ev::throwRangeError("genUniformGrid3D: grid too large");
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
    
    size_t count = 0;
    if (!gridCount({xSize, ySize, zSize}, n_dest, &count))
        return ev::throwRangeError("dest too small: " + std::to_string(xSize) + " x " +
                                   std::to_string(ySize) + " x " + std::to_string(zSize) +
                                   " floats required");
    
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
    size_t count = 0;
    if (!gridCount({xSize, ySize}, kMaxGridFloats, &count))
        return ev::throwRangeError("genTileable2D: grid too large");
    std::vector<float> output(count);
    w->node->GenTileable2D(output.data(), xSize, ySize, step, step, seed);
    return make_float32_array(output.data(), count);
}

static bronze::Value fast_noise_encode(bronze::Value thisVal, std::span<const bronze::Value>)
{
    auto* w = getNoise(thisVal);
    if (!w) return ev::throwTypeError("receiver is not a FastNoise instance");
    std::string encoded = FastNoise::Metadata::SerialiseNodeData(w->data.get());
    if (encoded.empty()) return ev::throwError("encode: the node tree has an unset source");
    return ev::fromUtf8(encoded);
}

static bronze::Value js_fast_noise_constructor(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty() || !ev::isString(args[0]))
        return ev::throwTypeError("FastNoise: expected encoded node tree string");

    std::string encoded = ev::toUtf8(args[0]);
    auto node = FastNoise::NewFromEncodedNodeTree(encoded.c_str());
    // The tree as data: every node's, owned together, the root's shared.
    auto owned = std::make_shared<std::vector<std::unique_ptr<FastNoise::NodeData>>>();
    FastNoise::NodeData* root = FastNoise::Metadata::DeserialiseNodeData(encoded.c_str(), *owned);
    if (!node || !root)
        return ev::throwTypeError("FastNoise: invalid encoded node tree");

    return wrap(new NoiseWrapper{std::move(node), std::shared_ptr<FastNoise::NodeData>(owned, root), {}, {}});
}

void installNoise()
{
    g_fastNoiseClass.install("FastNoise", 1, js_fast_noise_constructor, [](ObjectBuilder& proto) {
        proto.def("set", 2, fast_noise_set);
        proto.def("getMembers", 0, fast_noise_get_members);
        proto.def("encode", 0, fast_noise_encode);
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

    // setStatic re-reads the rooted constructor after each makeFunction; a raw
    // `ctor` local would be stale after the first allocation.
    auto stat = [](const char* name, uint32_t arity, ev::NativeFn fn) {
        g_fastNoiseClass.setStatic(name, ev::makeFunction(std::move(fn), arity, name));
    };
    stat("create", 1, fast_noise_create);
    stat("types", 0, fast_noise_types);
    stat("Simplex", 0, make_factory_node<FastNoise::Simplex>);
    stat("SuperSimplex", 0, make_factory_node<FastNoise::SuperSimplex>);
    stat("Perlin", 0, make_factory_node<FastNoise::Perlin>);
    stat("Value", 0, make_factory_node<FastNoise::Value>);
    stat("CellularValue", 0, make_factory_node<FastNoise::CellularValue>);
    stat("CellularDistance", 0, make_factory_node<FastNoise::CellularDistance>);
    stat("CellularLookup", 0, make_factory_node<FastNoise::CellularLookup>);
    stat("FractalFBm", 0, make_factory_node<FastNoise::FractalFBm>);
    stat("FractalRidged", 0, make_factory_node<FastNoise::FractalRidged>);
    stat("DomainWarpGradient", 0, make_factory_node<FastNoise::DomainWarpGradient>);
}

} // namespace brokit::api
