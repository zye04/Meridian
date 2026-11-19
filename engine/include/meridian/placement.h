#pragma once

#include <meridian/math/vec3.h>
#include <meridian/renderer.h>
#include <meridian/terrain.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace meridian {

// Placement runs per 64 m cell; one dither pattern tile covers one cell.
constexpr float kCellSize = 64.0f;

struct Prototype {
  std::vector<MeshId> meshes;
  bool impostor = false;
};

// Each rule is a smoothstep-shaped factor in 0..1; the density is their
// product. fade is the width of each slope/altitude ramp as a fraction of
// its range, inside the range (0 at the bounds). An open bound (slopeMinDeg
// <= 0, slopeMaxDeg >= 90, an altitude bound beyond the field's heights) has
// no ramp. The mask is fractal value noise in 0..1, shared by all layers of
// a seed: the factor rises from 0 at maskThreshold to 1 at maskThreshold +
// |maskEdge|; a negative maskEdge gives 1 minus that (the exact complement:
// keeps outside the mask). The edge band also shrinks the scale.
// Frequencies are in 1/m; 0 disables the noise rule. clusterAmount in 0..1
// blends density toward clustering noise, which differs per layer.
struct DensityRule {
  float slopeMinDeg, slopeMaxDeg, altitudeMin, altitudeMax, fade;
  float maskFrequency, maskThreshold, maskEdge;
  float clusterFrequency, clusterAmount;
};

enum class Rotation { Yaw, Full };

struct PlacementLayer {
  uint32_t id;
  std::vector<std::pair<uint32_t, float>> prototypes; // (prototype, weight)
  float footprint; // nominal spacing at full density, m
  DensityRule rule;
  float scaleMin, scaleMax;
  Rotation rotation;
  float sinkDepth;
  float maxDrawDistance = 0.0f;
};

// Points over one tile, ranked progressively: any prefix of the threshold
// order is evenly spread, so raising the density adds points evenly.
struct DitherPattern {
  std::vector<uint32_t> points; // x | z << 16 in 1/65536 of the tile
  std::vector<uint16_t> thresholds; // keep a point iff threshold < density
};
DitherPattern makeDitherPattern(uint32_t seed, uint32_t count);
// Points per cell-sized tile for a footprint.
uint32_t ditherPointCount(float footprint);

// PCG3D (Jarzynski and Olano 2020), first component.
uint32_t placementHash(uint32_t a, uint32_t b, uint32_t c);

// The integer inputs of the keep/reject path, converted once from floats.
// Positions are 16.16 fixed-point meters; crops are under 32 km wide
// (|x| < 16 km, cells -256..255). Both mirror the structs in
// shaders/placement.glsl.
struct PlacementField {
  int32_t originX, originZ; // sample (0, 0); the crop is [origin, -origin]
  uint32_t spacing;
  uint32_t width, height;
};
struct PlacementRule {
  int32_t slope[4];    // squared central difference, sample steps
  int32_t altitude[4]; // sample steps
  uint32_t maskPeriod; // 16.16 m; 0 = off
  int32_t maskLo, maskHi; // 0..65535 noise; lo > hi: 1 - ramp(hi..lo)
  uint32_t clusterPeriod; // 16.16 m; 0 = off
  uint32_t clusterAmount; // 0..65535
};
static_assert(sizeof(PlacementField) == 20 && sizeof(PlacementRule) == 52);
PlacementField placementField(const Heightfield &field);
PlacementRule placementRule(const Heightfield &field, const DensityRule &rule);
// Cumulative weights in 1/65536, last = 65536: pick the first prototype
// whose entry exceeds the low 16 bits of the point hash. Throws unless the
// total weight is positive.
std::vector<uint32_t> prototypeThresholds(const PlacementLayer &layer);

// 0..65535 at fixed-point (x, z), rules evaluated at the nearest sample.
uint16_t densityAt(const Heightfield &field, const PlacementLayer &layer, uint32_t seed,
                   int64_t xFixed, int64_t zFixed);

// Rotation: axis-angle (Yaw: axis +Y, angle = yaw). position.y includes the
// sink depth. prototype is the layer's prototype entry's id.
struct PlacedPoint {
  Vec3 position;
  float yaw;
  Vec3 axis;
  float angle;
  float scale;
  uint32_t prototype;
};
// The C++ reference of one pattern point of one cell; nullopt if rejected
// or outside the heightfield crop.
std::optional<PlacedPoint> evaluatePoint(const Heightfield &field,
                                         const PlacementLayer &layer,
                                         const DitherPattern &pattern, uint32_t seed,
                                         int32_t cellX, int32_t cellZ, uint32_t point);

} // namespace meridian
