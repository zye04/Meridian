// Placement keep/reject in integer math, bit-exact with the C++ reference
// (engine/src/placement.cpp); change both together. Positions are 16.16
// fixed-point meters.
#ifndef PLACEMENT_GLSL
#define PLACEMENT_GLSL

// Must match PlacementField and PlacementRule in placement.h.
struct PlacementField {
  int originX, originZ;
  uint spacing;
  uint width, height;
};
struct PlacementRule {
  int slope[4];
  int altitude[4];
  uint maskPeriod;
  int maskLo, maskHi;
  uint clusterPeriod;
  uint clusterAmount;
  int channels[4][3];
};
// Per geo texel its 8 channel bytes (meridian/geo.h).
layout(buffer_reference, std430) readonly buffer GeoTexels {
    uvec2 texels[];
};

// Must match PlacementParent in placement.h.
struct PlacementParent {
  PlacementRule rule;
  uint layerSeed;
  int lo, hi;
  uint enabled;
};

const uint kPlacementLayerSalt = 0x6c617972u;
const uint kPlacementMaskSalt = 0x6d61736bu;
const uint kPlacementClusterSalt = 0x636c7374u;
const uint kPlacementMaskOctaves = 4u;
const uint kPlacementClusterOctaves = 2u;

// PCG3D (Jarzynski and Olano 2020), first component.
uint placementHash(uint a, uint b, uint c) {
  uvec3 v = uvec3(a, b, c) * 1664525u + 1013904223u;
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v ^= v >> 16u;
  v.x += v.y * v.z;
  return v.x;
}

uint placementMul16(uint a, uint b) { return (a * b + 0xFFFFu) >> 16u; }

uint placementSmooth16(uint p) {
  uint p2 = (p * p) >> 16u;
  return min(3u * p2 - 2u * ((p2 * p) >> 16u), 0xFFFFu);
}

uint placementRamp16(int v, int lo, int hi) {
  if (v <= lo) {
    return 0u;
  }
  if (v >= hi) {
    return 0xFFFFu;
  }
  uint d = uint(hi) - uint(lo);
  uint n = uint(v) - uint(lo);
  int s = max(0, findMSB(d) - 15);
  return (n >> s) * 0xFFFFu / (d >> s);
}

// 1 - ramp(hi..lo) when lo > hi, as the mask.
uint placementRampEither16(int v, int lo, int hi) {
  return lo <= hi ? placementSmooth16(placementRamp16(v, lo, hi))
                  : 0xFFFFu - placementSmooth16(placementRamp16(v, hi, lo));
}

// Byte c of a geo texel's 8 channels packed in a uvec2.
int placementGeoByte(uvec2 texel, int c) {
  return int(((c < 4 ? texel.x : texel.y) >> (8u * uint(c & 3))) & 0xFFu);
}

uint placementBand16(int v, int t[4]) {
  return placementMul16(placementSmooth16(placementRamp16(v, t[0], t[1])),
                        0xFFFFu - placementSmooth16(placementRamp16(v, t[2], t[3])));
}

uint placementLerp16(uint a, uint b, uint t) {
  return ((a * (0x10000u - t)) >> 16u) + ((b * t) >> 16u);
}

uint placementValueNoise16(uint u, uint v, uint period, uint salt) {
  int s = max(0, findMSB(period) - 14);
  uint ix = u / period;
  uint iz = v / period;
  uint fx = placementSmooth16(min((((u % period) >> s) << 16u) / (period >> s), 0xFFFFu));
  uint fz = placementSmooth16(min((((v % period) >> s) << 16u) / (period >> s), 0xFFFFu));
  uint top = placementLerp16(placementHash(ix, iz, salt) >> 16u,
                             placementHash(ix + 1u, iz, salt) >> 16u, fx);
  uint bottom = placementLerp16(placementHash(ix, iz + 1u, salt) >> 16u,
                                placementHash(ix + 1u, iz + 1u, salt) >> 16u, fx);
  return placementLerp16(top, bottom, fz);
}

uint placementFbm16(uint u, uint v, uint period, uint octaves, uint salt) {
  uint sum = 0u;
  for (uint k = 0u; k < octaves; ++k) {
    sum += placementValueNoise16(u, v, period >> k, salt + k) << (octaves - 1u - k);
  }
  return sum / ((1u << octaves) - 1u);
}

// h: the nearest sample (placementNearestSample); dx, dz: its central
// differences (indices clamped to the grid), in sample steps. layerSeed:
// placementLayerSeed. texel: the point's geo channels (placementGeoByte).
// Returns the density 0..65535; mask: the mask factor alone (it also
// shrinks the scale at the edge).
uint placementDensity(PlacementRule rule, uint seed, uint layerSeed, int x, int z, int h, int dx,
                      int dz, uvec2 texel, out uint mask) {
  dx = clamp(dx, -32767, 32767);
  dz = clamp(dz, -32767, 32767);
  int g2 = dx * dx + dz * dz;
  uint density = placementMul16(placementBand16(g2, rule.slope),
                                placementBand16(h, rule.altitude));
  uint u = uint(x) ^ 0x80000000u;
  uint v = uint(z) ^ 0x80000000u;
  mask = 0xFFFFu;
  if (rule.maskPeriod != 0u) {
    int m = int(placementFbm16(u, v, rule.maskPeriod, kPlacementMaskOctaves,
                               seed ^ kPlacementMaskSalt));
    mask = rule.maskLo <= rule.maskHi
               ? placementSmooth16(placementRamp16(m, rule.maskLo, rule.maskHi))
               : 0xFFFFu - placementSmooth16(placementRamp16(m, rule.maskHi, rule.maskLo));
  }
  density = placementMul16(density, mask);
  if (rule.clusterPeriod != 0u) {
    uint c = placementSmooth16(placementFbm16(u, v, rule.clusterPeriod, kPlacementClusterOctaves,
                                              layerSeed ^ kPlacementClusterSalt));
    density = placementMul16(density, 0xFFFFu - placementMul16(rule.clusterAmount, 0xFFFFu - c));
  }
  for (int r = 0; r < 4; ++r) {
    if (rule.channels[r][0] >= 0) {
      density = placementMul16(
          density, placementRampEither16(placementGeoByte(texel, rule.channels[r][0]) * 256,
                                         rule.channels[r][1], rule.channels[r][2]));
    }
  }
  return density;
}

uint placementLayerSeed(uint layerId, uint seed) {
  return placementHash(layerId, seed, kPlacementLayerSalt);
}

uint placementCellHash(ivec2 cell, uint layerSeed) {
  return placementHash(uint(cell.x), uint(cell.y), layerSeed);
}

// packed: the pattern point (x | z << 16). One of the 8 square symmetries
// plus a toroidal offset per cell.
ivec2 placementPointPosition(ivec2 cell, uint cellHash, uint packed) {
  uint symmetry = placementHash(cellHash, 0u, 1u) & 7u;
  uint px = packed & 0xFFFFu;
  uint pz = packed >> 16u;
  if ((symmetry & 4u) != 0u) {
    uint t = px;
    px = pz;
    pz = t;
  }
  px ^= (symmetry & 1u) != 0u ? 0xFFFFu : 0u;
  pz ^= (symmetry & 2u) != 0u ? 0xFFFFu : 0u;
  px = (px + cellHash) & 0xFFFFu;
  pz = (pz + (cellHash >> 16u)) & 0xFFFFu;
  return cell * (1 << 22) + ivec2(uvec2(px, pz) << 6u);
}

// Reject cells outside -256..255 before placementPointPosition (no overflow).
bool placementCellInRange(ivec2 cell) {
  return all(greaterThanEqual(cell, ivec2(-256))) && all(lessThan(cell, ivec2(256)));
}

bool placementInsideCrop(PlacementField field, ivec2 p) {
  return p.x >= field.originX && p.x <= -field.originX && p.y >= field.originZ &&
         p.y <= -field.originZ;
}

uint placementNearestIndex(int x, int origin, uint spacing, uint size) {
  x = clamp(x, origin, -origin);
  uint i = (uint(x) - uint(origin) + spacing / 2u) / spacing;
  return min(i, size - 1u);
}

uvec2 placementNearestSample(PlacementField field, ivec2 p) {
  return uvec2(placementNearestIndex(p.x, field.originX, field.spacing, field.width),
               placementNearestIndex(p.y, field.originZ, field.spacing, field.height));
}

bool placementKeep(uint threshold, uint density) { return threshold < density; }

// The point's stable hash: prototype pick from its low 16 bits.
uint placementPointHash(uint cellHash, uint point) { return placementHash(cellHash, point, 2u); }

float streamKeepFraction(float distance, float fullDensity, float maxDistance) {
  if (distance >= maxDistance) {
    return 0.0;
  }
  float thin = distance <= fullDensity ? 1.0 : (fullDensity / distance) * (fullDensity / distance);
  return thin * (1.0 - smoothstep(0.5 * maxDistance, maxDistance, distance));
}

float streamFade(float keepFraction, float threshold) {
  return smoothstep(0.0, 0.3, keepFraction - threshold);
}

#endif
