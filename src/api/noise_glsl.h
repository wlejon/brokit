#pragma once
// FastNoise2 node trees as GLSL: the same field on the GPU as FastNoise2
// computes on the CPU, so a world's fields are one source in both places.
//
// The tree (its encoded string, node.encode()) becomes a GLSL function
//   float <name>(vec2 p, int seed)    and/or    float <name>(vec3 p, int seed)
// whose value at p is FastNoise2's genSingle2D / genSingle3D at p with that
// seed, to float rounding (FastNoise2 runs relaxed SIMD: fused multiply-adds
// and approximate reciprocals, so the CPU's last bits vary by machine too).
// Each node is ported from FastNoise2's own maths (the same hashes, primes,
// gradients and constants), one function a node and dimension; a node
// shared in the tree is one function. The source is self-contained GLSL 4.5
// (the shared noise maths behind an include guard, so several fields can go
// in one shader) and compiles to SPIR-V like any other shader.
//
// Not yet ported: SuperSimplex, the Cellular nodes, the domain warps, and
// 4D (a 3D field through AddDimension). Not a field: ConvertRGBA8 (a colour
// packed in the float's bits, NaNs among them, which a GPU need not keep).
// A tree that uses one is refused with the node's name.

#include <string>

namespace brokit::noise {

struct GlslOptions {
    std::string name = "field";   // the function's name (a GLSL identifier)
    bool dims2 = true;            // emit float name(vec2 p, int seed)
    bool dims3 = false;           // emit float name(vec3 p, int seed)
};

/// The tree `encoded` as GLSL into `glsl`; false (and why in `error`) when
/// it does not decode or uses a node not ported.
bool treeToGlsl(const std::string& encoded, const GlslOptions& options, std::string& glsl, std::string& error);

}  // namespace brokit::noise
