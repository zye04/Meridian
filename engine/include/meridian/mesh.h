#pragma once

#include <meridian/cluster.h>
#include <meridian/math/vec3.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace meridian {

// CPU-side vertex; the GPU gets it quantized (PackedVertex, vertex_pack.h).
struct Vertex {
  Vec3 position;
  float uvX = 0.0f;
  Vec3 normal;
  float uvY = 0.0f;
  // xyz: tangent along +U; w: bitangent sign (glTF/MikkTSpace convention).
  float tangent[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  // glTF COLOR_0 alpha (1 without COLOR_0); the alpha test multiplies it in.
  float alpha = 1.0f;
};
static_assert(sizeof(Vertex) == 52);

struct MeshData {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  // Filled by loadGltf; meshes without clusters are built at upload.
  ClusterLod clusters;
  // Opaque double-sided leaves with transmission: disjoint pieces whose
  // coarser cluster levels are scaled to keep their area (buildClusterLod).
  bool leaf = false;
  // Part of a model that gets voxel levels in its cluster DAG
  // (voxelizesModel).
  bool voxelize = false;
  // The height of the whole model, for a solid (non-foliage) part of a
  // voxelizing model: its finest voxel follows the model, so a short part
  // (a trunk base) doesn't voxelize at millimetre edges while the crown
  // keeps its triangles. 0 for foliage, whose own height sets its switch.
  float voxelModelHeight = 0.0f;
  // Seeds its voxel clusters' coverage hash (voxel_bricks.comp): from the
  // model's positions and indices and the primitive's index (seedVoxels), so
  // a rebuild, another load order or a cache/vertex format change keeps the
  // pattern. Not cached.
  uint32_t voxelSeed = 0;
};

// A model gets voxel levels when it holds a tree crown: a foliage mesh
// (material with diffuse transmission, or alpha MASK on a mesh without
// vertex alpha; foliage[i] != 0) over 4 m tall. Then all its meshes do, bark included; smaller plants keep their
// triangles.
bool voxelizesModel(std::span<const MeshData> meshes, std::span<const uint8_t> foliage);

// The cluster cache's key: the geometry, which meshes voxelize, the bytes of
// the textures their voxels sample, the voxel materials' factors, which
// material each mesh uses and which textures (indices into voxelTextures,
// -1: none) each material samples for base colour, transmission colour and
// metallic-roughness, and the voxelizer's version.
uint64_t clusterSourceKey(std::span<const MeshData> meshes,
                          std::span<const std::vector<uint8_t>> voxelTextures,
                          std::span<const VoxelMaterial> voxelMaterials,
                          std::span<const uint32_t> primitiveMaterial,
                          std::span<const std::array<int, 3>> materialTextures);

// Sets each mesh's voxelSeed from the model's positions and indices only
// (never cache or voxelizer versions or the Vertex layout), mixed with the
// mesh's index, so models differ and a format change keeps the pattern.
void seedVoxels(std::span<MeshData> meshes);

// The MeshData::leaf rule, from the glTF material.
inline bool keepsLeafArea(bool opaque, bool doubleSided, float diffuseTransmission) {
  return opaque && doubleSided && diffuseTransmission > 0.0f;
}

// Appends clusters.addedSources/addedPositions to the vertices, where the
// cluster vertex indices expect them. Call once, after the clusters are set.
void appendClusterVertices(MeshData &mesh);

} // namespace meridian
