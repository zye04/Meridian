#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace meridian::cook {

// Side of the leaf tint palette texture; each leaf piece samples one texel
// center.
constexpr uint32_t kLeafPaletteSize = 16;

struct TreeCookResult {
  size_t sourceTriangles, triangles;
  double leafAreaBefore, leafAreaAfter;
};

// Cooks an unflattened tree glTF (usd2gltf's output: one mesh per prototype,
// one node per instance) into outDir/<name>.gltf: flattened into one mesh per
// material, at most `triangleBudget` triangles. Leaf materials are the
// double-sided ones. `barkTexture` is an optional ambientCG texture set
// directory for the other materials.
TreeCookResult cookTree(const std::filesystem::path &in, const std::filesystem::path &outDir,
                        size_t triangleBudget, const std::filesystem::path &barkTexture);

} // namespace meridian::cook
