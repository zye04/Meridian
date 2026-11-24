#pragma once

// CPU voxelization of foliage for the voxel levels of the cluster DAG
// (docs/superpowers/specs/2026-11-24-dag-voxel-foliage-design.md). A
// surface voxelization: per axis, rays on a kVoxelRaysPerSide^2 grid per
// voxel face (Loubet and Neyret 2017) record which voxels they cross a
// triangle in; the blocked fraction along an axis gives the voxel's optical
// depth along it, the hits give its mean material. Voxels sit on a lattice
// anchored at the origin: voxel (i, j, k) spans [i, i + 1) edge along x,
// and so on, so every mesh of a model and every level share aligned grids.

#include <meridian/math/vec3.h>
#include <meridian/texture.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace meridian {

constexpr uint32_t kVoxelRaysPerSide = 4;
// Optical depth of a fully blocked axis: a voxel keeps 1/32 transmittance,
// so coverage never saturates in the coarser levels' averaging.
constexpr float kVoxelMinTransmittance = 1.0f / 32.0f;

// The material inputs a voxel averages, glTF metallic-roughness with
// KHR_materials_diffuse_transmission. Factors are linear; images may be null.
struct VoxelMaterial {
  const CpuImage *baseColor = nullptr; // sRGB colour, linear alpha
  Vec3 baseColorFactor{1.0f, 1.0f, 1.0f};
  bool alphaMask = false;
  float alphaCutoff = 0.5f;
  float diffuseTransmission = 0.0f;
  Vec3 transmissionColor{1.0f, 1.0f, 1.0f};
  const CpuImage *transmissionColorImage = nullptr; // sRGB
  const CpuImage *metallicRoughness = nullptr;      // roughness in G, linear
  float roughnessFactor = 1.0f;
};

struct VoxelTriangle {
  Vec3 p[3];
  Vec3 n[3];
  float uv[3][2];
  uint32_t material = 0;
};

// What one voxel represents, from the hits inside it.
struct VoxelPayload {
  // Optical depth through the voxel along x, y and z.
  float depth[3] = {};
  // Reflected diffuse colour, base colour x (1 - diffuse transmission), and
  // transmitted colour, transmission colour x diffuse transmission: the
  // split the leaf shading makes (mesh.frag).
  Vec3 albedo;
  Vec3 transmission;
  // Second moment of the hits' unit normals (xx, yy, zz, xy, xz, yz): sign
  // free, as double-sided leaves have no meaningful mean normal.
  float moment[6] = {};
  float roughness = 1.0f;
  // Projected coverage, 1 - the mean transmittance over the three axes: the
  // weight of this voxel's material where levels merge.
  float coverage = 0.0f;
};

// A 4^3 brick; voxel i (x + 4 (y + 4 z)) is present when bit i of occupied
// is set. origin is the lattice index of voxel 0, a multiple of 4.
struct VoxelBrick {
  int32_t origin[3] = {};
  uint64_t occupied = 0;
  std::array<VoxelPayload, 64> voxels{};
};

struct VoxelGrid {
  float edge = 0.0f;
  std::vector<VoxelBrick> bricks; // sorted by origin (z, y, x)
};

// Principal axis of a voxel's normal moment and its eigenvalue: 1 when every
// hit faces along the axis, 1/3 for normals in every direction.
struct NormalLobe {
  Vec3 axis{0.0f, 1.0f, 0.0f};
  float alignment = 1.0f / 3.0f;
};
NormalLobe normalLobe(const VoxelPayload &voxel);

VoxelGrid voxelize(std::span<const VoxelTriangle> triangles,
                   std::span<const VoxelMaterial> materials, float edge);

// The next level: edge x2. Along each axis a coarse voxel keeps the mean
// transmittance of the four two-voxel columns through it (the coverage it
// projects), not the mean optical depth, which would thicken sparse crowns;
// its material is the children's, weighted by their coverage.
VoxelGrid mergeVoxelGrid(const VoxelGrid &fine);

// Reference for tests: the optical depth of the voxel at voxelLo along axis
// from raysPerSide^2 rays at the cell centres of its face, each blocked when
// it crosses a triangle (passing the alpha test) inside the voxel.
float bruteForceOpticalDepth(std::span<const VoxelTriangle> triangles,
                             std::span<const VoxelMaterial> materials, Vec3 voxelLo,
                             float edge, int axis, int raysPerSide);

// A voxel as the GPU reads it (GpuVoxel, gpu_types.h), 16 bytes:
// depthSpread: optical depth along x, y, z (8 bits each, steps of 1/16) and
// the normal lobe's alignment (8 bits over [1/3, 1]); albedoRoughness and
// transmission: RGB as sqrt(linear) in 8 bits each (dark leaves keep their
// precision), roughness in the albedo word's top byte; normal: the lobe axis,
// octahedral 2 x 16 bits.
struct PackedVoxel {
  uint32_t depthSpread = 0;
  uint32_t albedoRoughness = 0;
  uint32_t transmission = 0;
  uint32_t normal = 0;
};
static_assert(sizeof(PackedVoxel) == 16);
PackedVoxel packVoxelPayload(const VoxelPayload &voxel);
// The payload back from its packed form; the moment is rebuilt from the lobe
// (axis, alignment), which is all the packing keeps of it.
VoxelPayload unpackVoxelPayload(const PackedVoxel &voxel);

// A brick of a voxel cluster: voxel i (x + 4 (y + 4 z)) present when bit i of
// occupied is set, stored at firstVoxel + the number of present voxels below
// it. origin is the lattice index of voxel 0.
struct VoxelBrickRef {
  int32_t origin[3] = {};
  uint32_t firstVoxel = 0;
  uint64_t occupied = 0;
};
static_assert(sizeof(VoxelBrickRef) == 24);

} // namespace meridian
