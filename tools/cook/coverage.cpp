#include "coverage.h"

#include <meridian/cluster.h>
#include <meridian/mesh.h>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace meridian::cook {

namespace {

// The bench view: 2560x1440, 70 degrees horizontal, the default 1 px LOD error.
constexpr float kScreenWidth = 2560.0f, kScreenHeight = 1440.0f;
constexpr float kHorizontalFov = 70.0f * std::numbers::pi_v<float> / 180.0f;
constexpr float kNearPlane = 0.1f;
constexpr float kLodErrorPixels = 1.0f;
constexpr uint32_t kResolution = 1024;

class Raster {
public:
  Raster(float x0, float y0, float x1, float y1, uint32_t resolution)
      : x0_(x0), y0_(y0), cell_((x1 - x0) / resolution), cellY_((y1 - y0) / resolution),
        resolution_(resolution), covered_(size_t(resolution) * resolution, 0) {}

  void triangle(const std::array<float, 3> &a, const std::array<float, 3> &b,
                const std::array<float, 3> &c) {
    const auto toGrid = [&](const std::array<float, 3> &p) {
      return std::array<double, 2>{(p[0] - x0_) / cell_, (p[1] - y0_) / cellY_};
    };
    const auto pa = toGrid(a), pb = toGrid(b), pc = toGrid(c);
    const double area = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pc[0] - pa[0]) * (pb[1] - pa[1]);
    if (area == 0.0) {
      return;
    }
    const auto lo = [&](int k) {
      return std::max(0, int(std::ceil(std::min({pa[k], pb[k], pc[k]}) - 0.5)));
    };
    const auto hi = [&](int k) {
      return std::min(int(resolution_) - 1, int(std::floor(std::max({pa[k], pb[k], pc[k]}) - 0.5)));
    };
    const auto edge = [](const std::array<double, 2> &p, const std::array<double, 2> &q, double x,
                         double y) { return (q[0] - p[0]) * (y - p[1]) - (q[1] - p[1]) * (x - p[0]); };
    const double sign = area > 0.0 ? 1.0 : -1.0;
    for (int y = lo(1); y <= hi(1); ++y) {
      for (int x = lo(0); x <= hi(0); ++x) {
        const double cx = x + 0.5, cy = y + 0.5;
        if (sign * edge(pa, pb, cx, cy) >= 0.0 && sign * edge(pb, pc, cx, cy) >= 0.0 &&
            sign * edge(pc, pa, cx, cy) >= 0.0) {
          covered_[size_t(y) * resolution_ + x] = 1;
        }
      }
    }
  }

  double coverage() const {
    return static_cast<double>(std::count(covered_.begin(), covered_.end(), 1)) /
           covered_.size();
  }

private:
  float x0_, y0_, cell_, cellY_;
  uint32_t resolution_;
  std::vector<uint8_t> covered_;
};

std::array<float, 3> position(const MeshData &mesh, uint32_t v) {
  const Vec3 &p = mesh.vertices[v].position;
  return {p.x, p.y, p.z};
}

} // namespace

double projectedCoverage(const std::vector<std::array<float, 3>> &positions,
                         const std::vector<uint32_t> &indices, float x0, float y0, float x1,
                         float y1, uint32_t resolution) {
  Raster raster(x0, y0, x1, y1, resolution);
  for (size_t t = 0; t + 2 < indices.size(); t += 3) {
    raster.triangle(positions[indices[t]], positions[indices[t + 1]], positions[indices[t + 2]]);
  }
  return raster.coverage();
}

std::vector<CoverageSample> crownCoverage(const std::filesystem::path &gltf,
                                          const std::vector<float> &distances) {
  fastgltf::Parser parser(fastgltf::Extensions::KHR_texture_basisu |
                          fastgltf::Extensions::KHR_materials_diffuse_transmission |
                          fastgltf::Extensions::KHR_texture_transform);
  auto data = fastgltf::GltfDataBuffer::FromPath(gltf);
  if (data.error() != fastgltf::Error::None) {
    throw std::runtime_error("cannot read " + gltf.string());
  }
  auto loaded = parser.loadGltf(data.get(), gltf.parent_path(),
                                fastgltf::Options::LoadExternalBuffers);
  if (loaded.error() != fastgltf::Error::None) {
    throw std::runtime_error("invalid glTF " + gltf.string());
  }
  const fastgltf::Asset &asset = loaded.get();

  // The cook writes one node per mesh, untransformed: mesh space is tree space.
  std::vector<MeshData> meshes;
  std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (const fastgltf::Mesh &m : asset.meshes) {
    for (const fastgltf::Primitive &primitive : m.primitives) {
      MeshData mesh;
      const auto &positions = asset.accessors[primitive.findAttribute("POSITION")->accessorIndex];
      mesh.vertices.resize(positions.count);
      fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
          asset, positions, [&](fastgltf::math::fvec3 p, size_t i) {
            mesh.vertices[i].position = {p.x(), p.y(), p.z()};
            for (int c = 0; c < 3; ++c) {
              lo[c] = std::min(lo[c], p[c]);
              hi[c] = std::max(hi[c], p[c]);
            }
          });
      if (const auto normal = primitive.findAttribute("NORMAL");
          normal != primitive.attributes.cend()) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
            asset, asset.accessors[normal->accessorIndex],
            [&](fastgltf::math::fvec3 n, size_t i) { mesh.vertices[i].normal = {n.x(), n.y(), n.z()}; });
      }
      if (const auto uv = primitive.findAttribute("TEXCOORD_0");
          uv != primitive.attributes.cend()) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
            asset, asset.accessors[uv->accessorIndex], [&](fastgltf::math::fvec2 t, size_t i) {
              mesh.vertices[i].uvX = t.x();
              mesh.vertices[i].uvY = t.y();
            });
      }
      const auto &indices = asset.accessors[*primitive.indicesAccessor];
      mesh.indices.resize(indices.count);
      fastgltf::copyFromAccessor<uint32_t>(asset, indices, mesh.indices.data());
      mesh.clusters = buildClusterLod(mesh);
      meshes.push_back(std::move(mesh));
    }
  }

  const float projY =
      1.0f / (std::tan(kHorizontalFov / 2.0f) * kScreenHeight / kScreenWidth);
  Raster full(lo[0], lo[1], hi[0], hi[1], kResolution);
  for (const MeshData &mesh : meshes) {
    for (size_t t = 0; t < mesh.indices.size(); t += 3) {
      full.triangle(position(mesh, mesh.indices[t]), position(mesh, mesh.indices[t + 1]),
                    position(mesh, mesh.indices[t + 2]));
    }
  }
  const double fullCoverage = full.coverage();

  std::vector<CoverageSample> samples;
  for (float distance : distances) {
    if (distance == 0.0f) {
      distance = (hi[1] - lo[1]) / 128.0f * projY * kScreenHeight / 4.0f;
    }
    const Vec3 camera{(lo[0] + hi[0]) / 2.0f, (lo[1] + hi[1]) / 2.0f, hi[2] + distance};
    Raster raster(lo[0], lo[1], hi[0], hi[1], kResolution);
    size_t triangles = 0;
    for (const MeshData &mesh : meshes) {
      const ClusterLod &lod = mesh.clusters;
      for (uint32_t id : selectClustersBvh(lod, camera, 1.0f, projY, kNearPlane, kScreenHeight,
                                           kLodErrorPixels)) {
        const Cluster &cluster = lod.clusters[id];
        triangles += cluster.triangleCount;
        for (uint32_t t = 0; t < cluster.triangleCount; ++t) {
          std::array<std::array<float, 3>, 3> corners;
          for (int k = 0; k < 3; ++k) {
            const uint32_t local = lod.triangles[(cluster.firstTriangle + t) * 3 + k];
            corners[k] = position(mesh, lod.vertices[cluster.firstVertex + local]);
          }
          raster.triangle(corners[0], corners[1], corners[2]);
        }
      }
    }
    samples.push_back({distance, raster.coverage(), raster.coverage() / fullCoverage, triangles});
  }
  return samples;
}

} // namespace meridian::cook
