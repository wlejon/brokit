// FastNoise2 node trees as GLSL (noise_glsl.h). Every node is ported from
// FastNoise2's own generators (include/FastNoise/Generators/*.inl at the
// pinned commit): the same primes, hashes, gradient tables, interpolants and
// bounds, in the same order of operations, FastNoise2's fused multiply-adds
// as fma(). The tree is FastNoise2's own decoding of it (NodeData), its
// members read by their metadata names, so a value set the way the CPU's
// generator was set is the value the GPU's function bakes in.

#include "api/noise_glsl.h"

#include <FastNoise/FastNoise.h>
#include <FastNoise/Metadata.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace brokit::noise {

namespace {

using FastNoise::Metadata;
using FastNoise::NodeData;

// The noise maths every field shares (FastNoise2's Utils.inl and the
// coherent noises), once a shader. Raw values: the nodes scale them to
// their output range.
constexpr const char* kLibrary = R"GLSL(
#ifndef BROKIT_FASTNOISE_GLSL
#define BROKIT_FASTNOISE_GLSL
// FastNoise2's noise maths (brokit/src/api/noise_glsl.cpp).
const int FN_PX = int(0xF797C5C7u);
const int FN_PY = int(0x6C060C89u);
const int FN_PZ = int(0x465FD04Fu);
const int FN_HASH = int(0xB7E0A5F5u);
const float FN_PERLIN_G = 2.41421356237;   // 1 + sqrt(2)

float fn_lerp(float a, float b, float t) { return fma(t, b - a, a); }
float fn_hermite(float t) { return t * t * fma(-t, 2.0, 3.0); }
float fn_quintic(float t) { return t * t * t * fma(t, fma(t, 6.0, -15.0), 10.0); }

int fn_hash(int seed, int x, int y) {
    int h = seed ^ (x ^ y);
    h *= FN_HASH;
    return (h >> 15) ^ h;
}
int fn_hash(int seed, int x, int y, int z) {
    int h = seed ^ (x ^ y ^ z);
    h *= FN_HASH;
    return (h >> 15) ^ h;
}
float fn_valueCoord(int seed, int x, int y) {
    int h = seed ^ (x ^ y);
    h *= h * FN_HASH;
    return float(h);
}
float fn_valueCoord(int seed, int x, int y, int z) {
    int h = seed ^ (x ^ y ^ z);
    h *= h * FN_HASH;
    return float(h);
}

// ( 1+R2, 1 ) ( -1-R2, 1 ) ( 1+R2, -1 ) ( -1-R2, -1 ) and the same swapped.
float fn_gradPerlin(int h, float x, float y) {
    if ((h & 1) != 0) x = -x;
    if ((h & 2) != 0) y = -y;
    return (h & 4) != 0 ? fma(FN_PERLIN_G, y, x) : fma(FN_PERLIN_G, x, y);
}
// The 12 edges of a cube (and 4 repeated).
float fn_gradCommon(int h, float x, float y, float z) {
    int a = h & 13;
    float u = a > 7 ? y : x;
    float v = a == 12 ? x : z;
    v = a < 2 ? y : v;
    if ((h & 1) != 0) u = -u;
    if ((h & 2) != 0) v = -v;
    return u + v;
}

float fn_value(int seed, vec2 p) {
    vec2 s = floor(p);
    int x0 = int(s.x) * FN_PX, y0 = int(s.y) * FN_PY;
    int x1 = x0 + FN_PX, y1 = y0 + FN_PY;
    float xs = fn_hermite(p.x - s.x), ys = fn_hermite(p.y - s.y);
    return fn_lerp(fn_lerp(fn_valueCoord(seed, x0, y0), fn_valueCoord(seed, x1, y0), xs),
                   fn_lerp(fn_valueCoord(seed, x0, y1), fn_valueCoord(seed, x1, y1), xs), ys);
}
float fn_value(int seed, vec3 p) {
    vec3 s = floor(p);
    int x0 = int(s.x) * FN_PX, y0 = int(s.y) * FN_PY, z0 = int(s.z) * FN_PZ;
    int x1 = x0 + FN_PX, y1 = y0 + FN_PY, z1 = z0 + FN_PZ;
    float xs = fn_hermite(p.x - s.x), ys = fn_hermite(p.y - s.y), zs = fn_hermite(p.z - s.z);
    return fn_lerp(fn_lerp(fn_lerp(fn_valueCoord(seed, x0, y0, z0), fn_valueCoord(seed, x1, y0, z0), xs),
                           fn_lerp(fn_valueCoord(seed, x0, y1, z0), fn_valueCoord(seed, x1, y1, z0), xs), ys),
                   fn_lerp(fn_lerp(fn_valueCoord(seed, x0, y0, z1), fn_valueCoord(seed, x1, y0, z1), xs),
                           fn_lerp(fn_valueCoord(seed, x0, y1, z1), fn_valueCoord(seed, x1, y1, z1), xs), ys), zs);
}

float fn_perlin(int seed, vec2 p) {
    vec2 s = floor(p);
    int x0 = int(s.x) * FN_PX, y0 = int(s.y) * FN_PY;
    int x1 = x0 + FN_PX, y1 = y0 + FN_PY;
    float xf0 = p.x - s.x, yf0 = p.y - s.y;
    float xf1 = xf0 - 1.0, yf1 = yf0 - 1.0;
    float xs = fn_quintic(xf0), ys = fn_quintic(yf0);
    return fn_lerp(fn_lerp(fn_gradPerlin(fn_hash(seed, x0, y0), xf0, yf0), fn_gradPerlin(fn_hash(seed, x1, y0), xf1, yf0), xs),
                   fn_lerp(fn_gradPerlin(fn_hash(seed, x0, y1), xf0, yf1), fn_gradPerlin(fn_hash(seed, x1, y1), xf1, yf1), xs), ys);
}
float fn_perlin(int seed, vec3 p) {
    vec3 s = floor(p);
    int x0 = int(s.x) * FN_PX, y0 = int(s.y) * FN_PY, z0 = int(s.z) * FN_PZ;
    int x1 = x0 + FN_PX, y1 = y0 + FN_PY, z1 = z0 + FN_PZ;
    float xf0 = p.x - s.x, yf0 = p.y - s.y, zf0 = p.z - s.z;
    float xf1 = xf0 - 1.0, yf1 = yf0 - 1.0, zf1 = zf0 - 1.0;
    float xs = fn_quintic(xf0), ys = fn_quintic(yf0), zs = fn_quintic(zf0);
    return fn_lerp(
        fn_lerp(fn_lerp(fn_gradCommon(fn_hash(seed, x0, y0, z0), xf0, yf0, zf0), fn_gradCommon(fn_hash(seed, x1, y0, z0), xf1, yf0, zf0), xs),
                fn_lerp(fn_gradCommon(fn_hash(seed, x0, y1, z0), xf0, yf1, zf0), fn_gradCommon(fn_hash(seed, x1, y1, z0), xf1, yf1, zf0), xs), ys),
        fn_lerp(fn_lerp(fn_gradCommon(fn_hash(seed, x0, y0, z1), xf0, yf0, zf1), fn_gradCommon(fn_hash(seed, x1, y0, z1), xf1, yf0, zf1), xs),
                fn_lerp(fn_gradCommon(fn_hash(seed, x0, y1, z1), xf0, yf1, zf1), fn_gradCommon(fn_hash(seed, x1, y1, z1), xf1, yf1, zf1), xs), ys),
        zs);
}

float fn_simplex(int seed, vec2 p) {
    const float kSkew = 0.366025403784438647;          // 1 / (sqrt(3) + 1)
    const float kUnskew = -0.211324865405187118;       // -1 / (sqrt(3) + 3)
    const float kUnskew1 = 0.788675134594812882;       // kUnskew + 1
    const float kUnskew2 = 0.577350269189625765;       // kUnskew * 2 + 1
    const float kFalloff2 = -3.15470053837925153;      // -4 (sqrt(3) + 2) / (sqrt(3) + 3)
    float skewDelta = kSkew * (p.x + p.y);
    float xSkewed = p.x + skewDelta, ySkewed = p.y + skewDelta;
    float xb = floor(xSkewed), yb = floor(ySkewed);
    float dxs = xSkewed - xb, dys = ySkewed - yb;
    int xp = int(xb) * FN_PX, yp = int(yb) * FN_PY;
    bool xge = dxs >= dys;
    float unskewDelta = kUnskew * (dxs + dys);
    float dx0 = dxs + unskewDelta, dy0 = dys + unskewDelta;
    float dx1 = (xge ? dx0 : dx0 + 1.0) - kUnskew1;
    float dy1 = (xge ? dy0 + 1.0 : dy0) - kUnskew1;
    float dx2 = dx0 - kUnskew2, dy2 = dy0 - kUnskew2;
    float f0 = fma(-dx0, dx0, fma(-dy0, dy0, 0.5));
    float f1 = fma(-dx1, dx1, fma(-dy1, dy1, 0.5));
    float f2 = f0 + fma(unskewDelta, kFalloff2, -0.666666666666666667);
    f0 = max(f0, 0.0); f1 = max(f1, 0.0); f2 = max(f2, 0.0);
    f0 *= f0; f0 *= f0;
    f1 *= f1; f1 *= f1;
    f2 *= f2; f2 *= f2;
    float g0 = fn_gradPerlin(fn_hash(seed, xp, yp), dx0, dy0);
    float g1 = fn_gradPerlin(fn_hash(seed, xge ? xp + FN_PX : xp, xge ? yp : yp + FN_PY), dx1, dy1);
    float g2 = fn_gradPerlin(fn_hash(seed, xp + FN_PX, yp + FN_PY), dx2, dy2);
    return fma(g0, f0, fma(g1, f1, g2 * f2));
}
float fn_simplex(int seed, vec3 p) {
    const float kSkew = 0.333333333333333333;
    const float kUnskew = -0.5;
    float skewDelta = kSkew * (p.x + p.y + p.z);
    vec3 skewed = vec3(p.x + skewDelta, p.y + skewDelta, p.z + skewDelta);
    vec3 b = floor(skewed);
    vec3 d = skewed - b;
    int xp = int(b.x) * FN_PX, yp = int(b.y) * FN_PY, zp = int(b.z) * FN_PZ;
    bool xgy = d.x >= d.y, ygz = d.y >= d.z, xgz = d.x >= d.z;
    float unskewDelta = kUnskew * (d.x + d.y + d.z);
    float dx0 = d.x + unskewDelta, dy0 = d.y + unskewDelta, dz0 = d.z + unskewDelta;
    bool maskX1 = xgy && xgz;
    bool maskY1 = ygz && !xgy;
    bool maskZ1 = xgz || ygz;     // inverted
    bool nMaskX2 = xgy || xgz;    // inverted
    bool nMaskY2 = xgy && !ygz;
    bool nMaskZ2 = xgz && ygz;
    float dx3 = dx0 + 0.5, dy3 = dy0 + 0.5, dz3 = dz0 + 0.5;
    float dx1 = maskX1 ? dx3 - 1.0 : dx3;
    float dy1 = maskY1 ? dy3 - 1.0 : dy3;
    float dz1 = maskZ1 ? dz3 : dz3 - 1.0;
    float dx2 = nMaskX2 ? dx0 : dx0 + 1.0;
    float dy2 = nMaskY2 ? dy0 + 1.0 : dy0;
    float dz2 = nMaskZ2 ? dz0 + 1.0 : dz0;
    float f0 = fma(-dz0, dz0, fma(-dy0, dy0, fma(-dx0, dx0, 0.6)));
    float f1 = fma(-dz1, dz1, fma(-dy1, dy1, fma(-dx1, dx1, 0.6)));
    float f2 = fma(-dz2, dz2, fma(-dy2, dy2, fma(-dx2, dx2, 0.6)));
    float f3 = f0 - (unskewDelta + 0.75);
    f0 = max(f0, 0.0); f1 = max(f1, 0.0); f2 = max(f2, 0.0); f3 = max(f3, 0.0);
    f0 *= f0; f0 *= f0;
    f1 *= f1; f1 *= f1;
    f2 *= f2; f2 *= f2;
    f3 *= f3; f3 *= f3;
    float g0 = fn_gradCommon(fn_hash(seed, xp, yp, zp), dx0, dy0, dz0);
    float g1 = fn_gradCommon(fn_hash(seed, maskX1 ? xp + FN_PX : xp, maskY1 ? yp + FN_PY : yp,
                                     maskZ1 ? zp : zp + FN_PZ), dx1, dy1, dz1);
    float g2 = fn_gradCommon(fn_hash(seed, nMaskX2 ? xp + FN_PX : xp, nMaskY2 ? yp : yp + FN_PY,
                                     nMaskZ2 ? zp : zp + FN_PZ), dx2, dy2, dz2);
    float g3 = fn_gradCommon(fn_hash(seed, xp + FN_PX, yp + FN_PY, zp + FN_PZ), dx3, dy3, dz3);
    return fma(g3, f3, fma(g2, f2, fma(g1, f1, g0 * f0)));
}

float fn_pingPong(float t) {
    t -= floor(t * 0.5) * 2.0;
    return t < 1.0 ? t : 2.0 - t;
}
#endif
)GLSL";

std::string lit(float f) {
    char b[64];
    if (!std::isfinite(f)) {
        uint32_t u;
        std::memcpy(&u, &f, sizeof u);
        std::snprintf(b, sizeof b, "uintBitsToFloat(0x%08Xu)", u);
        return b;
    }
    std::snprintf(b, sizeof b, "%.9g", f);
    std::string s = b;
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return std::signbit(f) ? "(" + s + ")" : s;
}

std::string lit(int i) { return i < 0 ? "(" + std::to_string(i) + ")" : std::to_string(i); }

const char* vecType(int dim) { return dim == 2 ? "vec2" : "vec3"; }
const char* axis(int i) { return i == 0 ? "x" : i == 1 ? "y" : "z"; }

class Emitter {
public:
    explicit Emitter(std::string prefix) : prefix_(std::move(prefix)) {}

    /// The function for node `n` in `dim` dimensions, emitted (after its
    /// sources) the first time it is asked for; "" on error.
    std::string node(const NodeData* n, int dim) {
        if (!error.empty()) return "";
        if (!n || !n->metadata) return fail("a source is not set");
        if (dim < 2 || dim > 3) return fail(std::string(n->metadata->name) + " in 4D (not ported)");
        const auto key = std::make_pair(n, dim);
        if (auto it = names_.find(key); it != names_.end()) return it->second;
        auto id = ids_.try_emplace(n, static_cast<int>(ids_.size())).first->second;
        const std::string name = prefix_ + "_n" + std::to_string(id) + "_" + std::to_string(dim);
        names_[key] = name;
        std::string body;
        if (!emitBody(*n, dim, body)) return "";
        code += "float " + name + "(" + vecType(dim) + " p, int seed) {\n" + body + "}\n";
        return name;
    }

    std::string code;
    std::string error;

private:
    std::string fail(const std::string& why) {
        if (error.empty()) error = why;
        return "";
    }

    static int varIndex(const NodeData& n, std::string_view name, int dim = -1) {
        const auto& vars = n.metadata->memberVariables;
        for (size_t i = 0; i < vars.size(); ++i)
            if (name == vars[i].name && vars[i].dimensionIdx == dim) return static_cast<int>(i);
        return -1;
    }
    static float fvar(const NodeData& n, std::string_view name, float def, int dim = -1) {
        const int i = varIndex(n, name, dim);
        return i < 0 ? def : n.variables[static_cast<size_t>(i)].f;
    }
    static int ivar(const NodeData& n, std::string_view name, int def, int dim = -1) {
        const int i = varIndex(n, name, dim);
        return i < 0 ? def : n.variables[static_cast<size_t>(i)].i;
    }
    const NodeData* lookup(const NodeData& n, std::string_view name) {
        const auto& members = n.metadata->memberNodeLookups;
        for (size_t i = 0; i < members.size(); ++i)
            if (name == members[i].name) return n.nodeLookups[i];
        return nullptr;
    }
    /// A source's value at (p, seed) in `dim` dimensions.
    std::string call(const NodeData* src, int dim, const std::string& p = "p", const std::string& seed = "seed") {
        const std::string f = node(src, dim);
        return f.empty() ? "0.0" : f + "(" + p + ", " + seed + ")";
    }
    std::string source(const NodeData& n, std::string_view name, int dim) {
        const NodeData* src = lookup(n, name);
        if (!src) {
            fail(std::string(n.metadata->name) + ": its " + std::string(name) + " is not set");
            return "0.0";
        }
        return call(src, dim);
    }
    /// A hybrid member: its node's value at (p, seed), or its constant.
    std::string hybrid(const NodeData& n, std::string_view name, int dim, int memberDim = -1) {
        const auto& members = n.metadata->memberHybrids;
        for (size_t i = 0; i < members.size(); ++i)
            if (name == members[i].name && members[i].dimensionIdx == memberDim) {
                const auto& h = n.hybrids[i];
                return h.first ? call(h.first, dim) : lit(h.second);
            }
        fail(std::string(n.metadata->name) + ": no member " + std::string(name));
        return "0.0";
    }

    // FastNoise2's VariableRange::ScaleOutput of a raw value in [lo, hi].
    static std::string scaleOutput(const NodeData& n, const std::string& raw, float lo, float hi) {
        const float rangeMin = fvar(n, "Output Min", -1.0f);
        const float rangeScale = fvar(n, "Output Max", 1.0f) - rangeMin;
        const float k = (1.0f / (hi - lo)) * rangeScale;
        return "fma(" + lit(k) + ", " + raw + " - " + lit(lo) + ", " + lit(rangeMin) + ")";
    }
    // ScalableGenerator::ScalePositions and Seeded's offset.
    static std::string scaleAndSeed(const NodeData& n, bool seeded) {
        const float frequency = 1.0f / fvar(n, "Feature Scale", 100.0f);
        std::string s = "    p *= " + lit(frequency) + ";\n";
        if (seeded) s += "    seed += " + lit(ivar(n, "Seed Offset", 0)) + ";\n";
        return s;
    }

    static std::string distance(int fn, const std::vector<std::string>& d, const std::string& minkowski) {
        const size_t k = d.size();
        std::string s;
        switch (fn) {
            default:
            case 0:   // Euclidean
            case 1: { // Euclidean Squared
                s = d[0] + " * " + d[0];
                for (size_t i = 1; i < k; ++i) s = "fma(" + d[i] + ", " + d[i] + ", " + s + ")";
                return fn == 0 ? "sqrt(" + s + ")" : s;
            }
            case 2:   // Manhattan
                s = "abs(" + d[0] + ")";
                for (size_t i = 1; i < k; ++i) s += " + abs(" + d[i] + ")";
                return "(" + s + ")";
            case 3:   // Hybrid
                s = "fma(" + d[0] + ", " + d[0] + ", abs(" + d[0] + "))";
                for (size_t i = 1; i < k; ++i) s += " + fma(" + d[i] + ", " + d[i] + ", abs(" + d[i] + "))";
                return "(" + s + ")";
            case 4:   // Max Axis
                s = "abs(" + d[0] + ")";
                for (size_t i = 1; i < k; ++i) s = "max(abs(" + d[i] + "), " + s + ")";
                return s;
            case 5:   // Minkowski
                s = "pow(abs(" + d[0] + "), " + minkowski + ")";
                for (size_t i = 1; i < k; ++i) s += " + pow(abs(" + d[i] + "), " + minkowski + ")";
                return "pow(" + s + ", 1.0 / " + minkowski + ")";
        }
    }

    bool emitBody(const NodeData& n, int dim, std::string& b) {
        const std::string type = n.metadata->name;
        const std::string vt = vecType(dim);
        auto ret = [&](const std::string& e) { b += "    return " + e + ";\n"; };

        if (type == "Constant") {
            ret(lit(fvar(n, "Value", 1.0f)));
        } else if (type == "White") {
            b += "    seed += " + lit(ivar(n, "Seed Offset", 0)) + ";\n";
            const std::string it = dim == 2 ? "ivec2" : "ivec3";
            b += "    " + it + " q = floatBitsToInt(p);\n";
            b += "    q = (q ^ (q >> 16)) * " + it + (dim == 2 ? "(FN_PX, FN_PY)" : "(FN_PX, FN_PY, FN_PZ)") + ";\n";
            const std::string raw = dim == 2 ? "fn_valueCoord(seed, q.x, q.y)" : "fn_valueCoord(seed, q.x, q.y, q.z)";
            ret(scaleOutput(n, raw, -2147483648.0f, 2147483648.0f));
        } else if (type == "Checkerboard") {
            b += scaleAndSeed(n, false);
            std::string v = "int(roundEven(p.x)) ^ int(roundEven(p.y))";
            if (dim == 3) v += " ^ int(roundEven(p.z))";
            b += "    float raw = ((" + v + ") & 1) != 0 ? 2.0 : 0.0;\n";
            ret(scaleOutput(n, "raw", 0.0f, 2.0f));
        } else if (type == "SineWave") {
            b += scaleAndSeed(n, false);
            ret(scaleOutput(n, dim == 2 ? "(sin(p.x) * sin(p.y))" : "(sin(p.x) * (sin(p.y) * sin(p.z)))", -1.0f, 1.0f));
        } else if (type == "Value" || type == "Perlin" || type == "Simplex") {
            b += scaleAndSeed(n, true);
            float bound;
            if (type == "Value") bound = 2147483648.0f;
            else if (type == "Perlin") bound = dim == 2 ? 1.726796627044677734375f : static_cast<float>(1.0363423824310302734375);
            else bound = dim == 2 ? 0.0261208079755306243896484375f : 0.030586399137973785400390625f;
            const std::string fn = type == "Value" ? "fn_value" : type == "Perlin" ? "fn_perlin" : "fn_simplex";
            ret(scaleOutput(n, fn + "(seed, p)", -bound, bound));
        } else if (type == "Gradient") {
            std::vector<std::string> off;
            for (int i = 0; i < dim; ++i) off.push_back(hybrid(n, "Offset", dim, i));
            b += "    float r = 0.0;\n";
            for (int i = 0; i < dim; ++i)
                b += std::string("    r = fma(p.") + axis(i) + " + " + off[static_cast<size_t>(i)] + ", " +
                     lit(fvar(n, "Multiplier", 0.0f, i)) + ", r);\n";
            ret("r");
        } else if (type == "DistanceToPoint") {
            const int fn = ivar(n, "Distance Function", 1);
            if (fn == 5) b += "    float mp = " + hybrid(n, "Minkowski P", dim) + ";\n";
            std::vector<std::string> d;
            for (int i = 0; i < dim; ++i) {
                b += std::string("    float d") + axis(i) + " = p." + axis(i) + " - " + hybrid(n, "Point", dim, i) + ";\n";
                d.push_back(std::string("d") + axis(i));
            }
            ret(distance(fn, d, "mp"));
        } else if (type == "FractalFBm" || type == "FractalRidged") {
            const bool ridged = type == "FractalRidged";
            const NodeData* src = lookup(n, "Source");
            if (!src) return !fail(type + ": its Source is not set").empty();
            b += "    float gain = " + hybrid(n, "Gain", dim) + ";\n";
            b += "    float weighted = " + hybrid(n, "Weighted Strength", dim) + ";\n";
            b += "    float amp = 1.0;\n";
            const std::string sample = ridged ? "abs(" + call(src, dim) + ")" : call(src, dim);
            const std::string term = ridged ? "(noise * -2.0 + 1.0) * amp" : "noise * amp";
            b += "    float noise = " + sample + ";\n";
            b += "    float sum = " + term + ";\n";
            b += "    for (int i = 1; i < " + lit(ivar(n, "Octaves", 3)) + "; ++i) {\n";
            b += "        seed += 1;\n";
            b += std::string("        amp *= fn_lerp(1.0, ") + (ridged ? "1.0 - noise" : "(noise + 1.0) * 0.5") +
                 ", weighted);\n";
            b += "        amp *= gain;\n";
            b += "        p *= " + lit(fvar(n, "Lacunarity", 2.0f)) + ";\n";
            b += "        noise = " + sample + ";\n";
            b += "        sum += " + term + ";\n";
            b += "    }\n";
            ret("sum");
        } else if (type == "Add" || type == "Multiply" || type == "Min" || type == "Max") {
            b += "    float a = " + source(n, "LHS", dim) + ";\n";
            b += "    float c = " + hybrid(n, "RHS", dim) + ";\n";
            if (type == "Add") ret("a + c");
            else if (type == "Multiply") ret("a * c");
            else if (type == "Min") ret("min(a, c)");
            else ret("max(a, c)");
        } else if (type == "Subtract" || type == "Divide" || type == "Modulus") {
            b += "    float a = " + hybrid(n, "LHS", dim) + ";\n";
            b += "    float c = " + hybrid(n, "RHS", dim) + ";\n";
            if (type == "Subtract") ret("a - c");
            else if (type == "Divide") ret("a / c");
            else ret("a - trunc(a / c) * c");
        } else if (type == "PowFloat") {
            b += "    float v = abs(" + hybrid(n, "Value", dim) + ");\n";
            ret("pow(v, " + hybrid(n, "Pow", dim) + ")");
        } else if (type == "PowInt") {
            b += "    float v = " + source(n, "Value", dim) + ";\n";
            b += "    float r = v * v;\n";
            for (int i = 2; i < ivar(n, "Pow", 2); ++i) b += "    r *= v;\n";
            ret("r");
        } else if (type == "MinSmooth" || type == "MaxSmooth") {
            const bool mx = type == "MaxSmooth";
            b += std::string("    float a = ") + (mx ? "-" : "") + source(n, "LHS", dim) + ";\n";
            b += std::string("    float c = ") + (mx ? "-" : "") + hybrid(n, "RHS", dim) + ";\n";
            b += "    float smoothness = max(1.17549435e-38, abs(" + hybrid(n, "Smoothness", dim) + "));\n";
            b += "    float h = max(smoothness - abs(a - c), 0.0);\n";
            b += "    h *= 1.0 / smoothness;\n";
            b += "    float r = fma(-0.166666672, h * h * h * smoothness, min(a, c));\n";
            ret(mx ? "-r" : "r");
        } else if (type == "Fade") {
            b += "    float fade = " + hybrid(n, "Fade", dim) + ";\n";
            b += "    float fadeMin = " + hybrid(n, "Fade Min", dim) + ";\n";
            b += "    float fadeMax = " + hybrid(n, "Fade Max", dim) + ";\n";
            b += "    float fadeRange = fadeMax - fadeMin;\n";
            b += "    fade = max(0.0, min(1.0, (fade - fadeMin) / fadeRange));\n";
            const int interp = ivar(n, "Interpolation", 0);
            if (interp == 1) b += "    fade = fn_hermite(fade);\n";
            else if (interp == 2) b += "    fade = fn_quintic(fade);\n";
            b += "    fade = fadeRange == 0.0 ? 0.5 : fade;\n";
            ret("fn_lerp(" + source(n, "A", dim) + ", " + source(n, "B", dim) + ", fade)");
        } else if (type == "DomainScale") {
            b += "    p *= " + lit(fvar(n, "Scaling", 1.0f)) + ";\n";
            ret(source(n, "Source", dim));
        } else if (type == "DomainAxisScale") {
            for (int i = 0; i < dim; ++i)
                b += std::string("    p.") + axis(i) + " *= " + lit(fvar(n, "Scaling", 1.0f, i)) + ";\n";
            ret(source(n, "Source", dim));
        } else if (type == "DomainOffset") {
            std::string offsets;
            for (int i = 0; i < dim; ++i) offsets += (i ? ", " : "") + hybrid(n, "Offset", dim, i);
            b += "    p += " + vt + "(" + offsets + ");\n";
            ret(source(n, "Source", dim));
        } else if (type == "DomainRotate") {
            const float yaw = fvar(n, "Yaw", 0.0f), pitch = fvar(n, "Pitch", 0.0f), roll = fvar(n, "Roll", 0.0f);
            const float yc = std::cos(yaw), ys = std::sin(yaw), pc = std::cos(pitch), ps = std::sin(pitch);
            const float rc = std::cos(roll), rs = std::sin(roll);
            const float xa = yc * pc, xb = yc * ps * rs - ys * rc, xc = yc * ps * rc + ys * rs;
            const float ya = ys * pc, yb = ys * ps * rs + yc * rc, yc2 = ys * ps * rc - yc * rs;
            const float za = -ps, zb = pc * rs, zc = pc * rc;
            if (dim == 2 && ps == 0.0f && rs == 0.0f) {
                b += "    " + vt + " q = vec2(fma(-p.y, " + lit(ys) + ", p.x * " + lit(yc) + "), fma(p.x, " + lit(ys) +
                     ", p.y * " + lit(yc) + "));\n";
                ret(call(lookup(n, "Source"), 2, "q"));
            } else {
                b += dim == 2 ? "    vec3 r = vec3(p, 0.0);\n" : "    vec3 r = p;\n";
                auto row = [&](float a, float bb, float c) {
                    return "fma(r.x, " + lit(a) + ", fma(r.y, " + lit(bb) + ", r.z * " + lit(c) + "))";
                };
                b += "    vec3 q = vec3(" + row(xa, xb, xc) + ",\n        " + row(ya, yb, yc2) + ",\n        " +
                     row(za, zb, zc) + ");\n";
                ret(call(lookup(n, "Source"), 3, "q"));
            }
        } else if (type == "SeedOffset") {
            ret(call(lookup(n, "Source"), dim, "p", "seed + " + lit(ivar(n, "Seed Offset", 1))));
        } else if (type == "Remap") {
            b += "    float s = " + source(n, "Source", dim) + ";\n";
            b += "    float fromMin = " + hybrid(n, "From Min", dim) + ";\n";
            b += "    float fromMax = " + hybrid(n, "From Max", dim) + ";\n";
            b += "    float toMin = " + hybrid(n, "To Min", dim) + ";\n";
            b += "    float toMax = " + hybrid(n, "To Max", dim) + ";\n";
            b += "    float r = toMin + ((s - fromMin) / (fromMax - fromMin) * (toMax - toMin));\n";
            if (ivar(n, "Clamp Output", 0) == 1) b += "    r = min(max(r, toMin), toMax);\n";
            ret("r");
        } else if (type == "Terrace") {
            const float steps = fvar(n, "Step Count", 1.0f);
            b += "    float s = " + source(n, "Source", dim) + " * " + lit(steps) + ";\n";
            b += "    float r = roundEven(s);\n";
            const auto& hs = n.metadata->memberHybrids;
            const std::pair<NodeData*, float>* smooth = nullptr;
            for (size_t i = 0; i < hs.size(); ++i)
                if (std::string_view(hs[i].name) == "Smoothness") smooth = &n.hybrids[i];
            const std::string step =
                "        float diff = r - s;\n"
                "        bool negative = diff < 0.0;\n"
                "        diff = 0.5 - abs(diff);\n"
                "        diff = min(diff * recip, 0.5);\n"
                "        r += negative ? 0.5 - diff : diff - 0.5;\n";
            if (smooth && smooth->first) {
                b += "    float smoothness = " + call(smooth->first, dim) + ";\n";
                b += "    if (smoothness != 0.0) {\n        float recip = 1.0 + 1.0 / smoothness;\n" + step + "    }\n";
            } else if (smooth && smooth->second != 0.0f) {
                b += "    {\n        float recip = " + lit(1.0f + 1.0f / smooth->second) + ";\n" + step + "    }\n";
            }
            ret("r * " + lit(1.0f / steps));
        } else if (type == "PingPong") {
            ret("fn_pingPong(" + source(n, "Source", dim) + " * " + hybrid(n, "Ping Pong Strength", dim) + ")");
        } else if (type == "AddDimension") {
            if (dim == 3) return !fail("AddDimension of a 3D field (4D, not ported)").empty();
            b += "    vec3 q = vec3(p, " + hybrid(n, "New Dimension Position", dim) + ");\n";
            ret(call(lookup(n, "Source"), 3, "q"));
        } else if (type == "RemoveDimension") {
            const NodeData* src = lookup(n, "Source");
            if (dim == 2) {
                ret(call(src, 2));
            } else {
                const int removed = ivar(n, "Remove Dimension", 1);
                if (removed == 0) ret(call(src, 2, "p.yz"));
                else if (removed == 1) ret(call(src, 2, "p.xz"));
                else if (removed == 2) ret(call(src, 2, "p.xy"));
                else ret(call(src, 3));
            }
        } else if (type == "GeneratorCache") {
            ret(source(n, "Source", dim));
        } else if (type == "SignedSquareRoot") {
            b += "    float v = " + source(n, "Source", dim) + ";\n";
            ret("v < 0.0 ? -sqrt(-v) : sqrt(v)");
        } else if (type == "Abs") {
            ret("abs(" + source(n, "Source", dim) + ")");
        } else if (type == "DomainRotatePlane") {
            const int kind = dim == 2 ? 0 : ivar(n, "Rotation Type", 0);
            b += dim == 2 ? "    vec3 r = vec3(p, 0.0);\n" : "    vec3 r = p;\n";
            if (kind == 0) {
                b += "    float xy = r.x + r.y;\n"
                     "    float s2 = xy * -0.211324865;\n"
                     "    float nz = r.z * 0.577350269;\n"
                     "    vec3 q = vec3(r.x + s2 - nz, r.y + s2 - nz, nz + xy * 0.577350269);\n";
            } else {
                b += "    float xz = r.x + r.z;\n"
                     "    float s2 = xz * -0.211324865;\n"
                     "    float ny = r.y * 0.577350269;\n"
                     "    vec3 q = vec3(r.x + s2 - ny, ny + xz * 0.577350269, r.z + s2 - ny);\n";
            }
            ret(call(lookup(n, "Source"), 3, "q"));
        } else {
            return !fail(type + " (not ported to GLSL yet)").empty();
        }
        return error.empty();
    }

    std::string prefix_;
    std::map<std::pair<const NodeData*, int>, std::string> names_;
    std::map<const NodeData*, int> ids_;
};

bool identifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return s.rfind("gl_", 0) != 0;
}

}  // namespace

bool treeToGlsl(const std::string& encoded, const GlslOptions& options, std::string& glsl, std::string& error) {
    glsl.clear();
    error.clear();
    if (!identifier(options.name)) {
        error = "not a GLSL name: '" + options.name + "'";
        return false;
    }
    if (!options.dims2 && !options.dims3) {
        error = "no dimensions asked for";
        return false;
    }
    std::vector<std::unique_ptr<NodeData>> owned;
    const NodeData* root = Metadata::DeserialiseNodeData(encoded.c_str(), owned);
    if (!root) {
        error = "the encoded node tree does not decode";
        return false;
    }
    Emitter e(options.name);
    std::string entries;
    for (int dim : {2, 3}) {
        if ((dim == 2 && !options.dims2) || (dim == 3 && !options.dims3)) continue;
        const std::string f = e.node(root, dim);
        if (f.empty()) break;
        entries += std::string("float ") + options.name + "(" + vecType(dim) + " p, int seed) { return " + f +
                   "(p, seed); }\n";
    }
    if (!e.error.empty()) {
        error = e.error;
        return false;
    }
    glsl = std::string(kLibrary) + "// FastNoise2 tree " + options.name + "\n" + e.code + entries;
    return true;
}

}  // namespace brokit::noise
