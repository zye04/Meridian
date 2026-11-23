#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <fastgltf/types.hpp>

namespace meridian::cook {

// Side of the leaf tint palette texture; each leaf piece samples one texel
// center.
constexpr uint32_t kLeafPaletteSize = 16;

// Leaf materials: double-sided or alpha-masked, or named like foliage
// (leaf, foliage, needle, two-sided) when an exporter drops both flags.
bool isLeafMaterial(const fastgltf::Material &material);

// Leaf transmission given as "the back transmits t times the base colour"
// (Unreal's translucency, the cook's default), as
// KHR_materials_diffuse_transmission: base colour and transmission colour
// both base x (1 + t), factor t / (1 + t). The front still reflects the base
// colour and the back transmits t times it.
struct KhrLeafTransmission {
  float factor;
  float colorScale;
};
inline KhrLeafTransmission khrLeafTransmission(float transmittedOverReflected) {
  return {transmittedOverReflected / (1.0f + transmittedOverReflected),
          1.0f + transmittedOverReflected};
}

struct TreeCookResult {
  size_t sourceTriangles, triangles;
  double leafAreaBefore, leafAreaAfter;
};

// Cooks an unflattened tree glTF (usd2gltf's output: one mesh per prototype,
// one node per instance) into outDir/<name>.gltf: flattened into one mesh per
// material, at most `triangleBudget` triangles. Materials with a base color
// texture keep their UVs and maps; an alpha-tested leaf material's cards are
// traced into opaque leaves (leaf_trace.h); untextured leaves get a tint palette, and
// `barkTexture`, an optional ambientCG texture set directory, textures
// untextured wood.
TreeCookResult cookTree(const std::filesystem::path &in, const std::filesystem::path &outDir,
                        size_t triangleBudget, const std::filesystem::path &barkTexture);

} // namespace meridian::cook
