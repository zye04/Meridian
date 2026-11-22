#pragma once

#include <meridian/cluster.h>
#include <meridian/math/vec3.h>

#include <cstdint>
#include <vector>

namespace meridian {

// Layout matches the std430 Vertex struct in the shaders: each vec3 is padded
// to 16 bytes, so the UVs fill the padding for free.
struct Vertex {
  Vec3 position;
  float uvX = 0.0f;
  Vec3 normal;
  float uvY = 0.0f;
  // xyz: tangent along +U; w: bitangent sign (glTF/MikkTSpace convention).
  float tangent[4] = {1.0f, 0.0f, 0.0f, 1.0f};
};
static_assert(sizeof(Vertex) == 48);

struct MeshData {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  // Filled by loadGltf; meshes without clusters are built at upload.
  ClusterLod clusters;
  // Opaque double-sided leaves with transmission: disjoint pieces whose
  // coarser cluster levels are scaled to keep their area (buildClusterLod).
  bool leaf = false;
};

// The MeshData::leaf rule, from the glTF material.
inline bool keepsLeafArea(bool opaque, bool doubleSided, float diffuseTransmission) {
  return opaque && doubleSided && diffuseTransmission > 0.0f;
}

// Appends clusters.addedSources/addedPositions to the vertices, where the
// cluster vertex indices expect them. Call once, after the clusters are set.
void appendClusterVertices(MeshData &mesh);

} // namespace meridian
