#pragma once

// Crown coverage across the cluster LOD cut: how much of a tree's side view
// the clusters drawn at a distance still cover, against full detail. Leaves
// are disjoint pieces, which coarse cluster levels can drop, thinning crowns.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace meridian::cook {

// Fraction of a resolution^2 grid over [x0,x1]x[y0,y1] covered by the
// triangles projected onto xy (orthographic), sampled at cell centres.
double projectedCoverage(const std::vector<std::array<float, 3>> &positions,
                         const std::vector<uint32_t> &indices, float x0, float y0, float x1,
                         float y1, uint32_t resolution);

struct CoverageSample {
  float distance;
  double coverage, ratio;
  size_t triangles;
};

// Side view of a cooked tree along -z: per distance, the clusters the LOD cut
// draws at 1 px (2560x1440, 70 degrees horizontal), projected at 1024^2 over
// the tree's xy bounds; ratio = coverage / full-detail coverage. A distance of
// 0 stands for the voxel switch: where the tree's finest voxel (height / 128)
// projects to 2 px; its sample reports that distance.
std::vector<CoverageSample> crownCoverage(const std::filesystem::path &gltf,
                                          const std::vector<float> &distances);

} // namespace meridian::cook
