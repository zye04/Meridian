// The terrain material's role weights: a pure function of the position and
// the ground normal, shared by mesh.frag's terrain surface and the grass
// (grass.mesh), which reads them at each blade's root. Include after
// terrain.glsl and placement.glsl.
#ifndef TERRAIN_ROLES_GLSL
#define TERRAIN_ROLES_GLSL

#include "placement.glsl"


// Rounds toward minus infinity (b > 0); integer division truncates.
int floorDiv(int a, int b) {
    return (a >= 0 ? a : a - b + 1) / b;
}

// Repeats every `period` cells: lattice points are wrapped to the period,
// centered on the origin, then hashed as integers (see hexHash).
float periodicHash(ivec2 cell, int period, uint seed) {
    ivec2 wrapped = ivec2(cell.x - period * floorDiv(cell.x + period / 2, period),
                          cell.y - period * floorDiv(cell.y + period / 2, period));
    return terrainHash(wrapped, seed);
}

float valueNoise(vec2 p, int period, uint seed) {
    ivec2 cell = ivec2(floor(p));
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(periodicHash(cell, period, seed),
                   periodicHash(cell + ivec2(1, 0), period, seed), f.x),
               mix(periodicHash(cell + ivec2(0, 1), period, seed),
                   periodicHash(cell + ivec2(1, 1), period, seed), f.x),
               f.y);
}

// The ground's noise cell (m) and its wrap in cells; must match
// gpu_types.h. Every octave's scale divides or multiplies the wrap evenly.
const float kNoiseCell = 40.0;
const int kNoiseWrapCells = 100;

// Value noise in kNoiseCell units at `scale` cells per cell (scale * 100
// must be a whole number).
float groundNoise(vec2 p, float scale, uint seed) {
    return valueNoise(p * scale, int(round(float(kNoiseWrapCells) * scale)), seed);
}

// The forest mask noise (placement.glsl, bit-exact with the trees') at this
// position, 0..1; -1 without a forest or outside the heightfield's crop, where
// nothing is placed.
float forestMask(Terrain terrain, vec2 worldXZ) {
    vec2 halfSize = 0.5 * (vec2(terrain.width, terrain.height) - 1.0) * terrain.spacing;
    if (terrain.forestMaskPeriod == 0u || any(greaterThan(abs(worldXZ), halfSize))) {
        return -1.0;
    }
    ivec2 p = ivec2(round(worldXZ * 65536.0));
    return float(placementFbm16(uint(p.x) ^ 0x80000000u, uint(p.y) ^ 0x80000000u,
                                terrain.forestMaskPeriod, kPlacementMaskOctaves,
                                terrain.forestMaskSeed ^ kPlacementMaskSalt)) / 65535.0;
}

// Laplacian of the ground height (1/m) over ~24 m, from the normal map's
// divergence: > 0 in hollows and valley bottoms, < 0 on ridges. On the
// Napf's ground under 12 degrees the 10th/90th percentiles are about
// -0.02/+0.017 (at 12 m steps from the 2 m data).
float groundCurvature(Terrain terrain, vec2 worldXZ) {
    const float kStep = 12.0;
    vec2 size = vec2(float(terrain.width), float(terrain.height));
    vec2 uv = (worldXZ / terrain.spacing + 0.5 * (size - 1.0) + 0.5) / size;
    vec2 d = kStep / terrain.spacing / size;
    uint t = terrain.normalTexture;
    float lod = log2(kStep / terrain.spacing);
    float dx = textureLod(textures[t], uv + vec2(d.x, 0.0), lod).r -
               textureLod(textures[t], uv - vec2(d.x, 0.0), lod).r;
    float dz = textureLod(textures[t], uv + vec2(0.0, d.y), lod).g -
               textureLod(textures[t], uv - vec2(0.0, d.y), lod).g;
    return -(dx + dz) / (2.0 * kStep);
}


// The ground noise's coordinates (kNoiseCell units) of a camera-relative
// position.
vec2 terrainNoiseCoord(Terrain terrain, vec2 relativeXZ) {
    return (relativeXZ + terrain.noiseOffset) / kNoiseCell;
}

// Each role's weight from the ground's shape, the forest mask and noise, in
// TerrainRole order (grass at the bottom, rock on top); ~5 m edge noise
// makes the borders irregular. mesh.frag stacks them with height blends.
void terrainRoleWeights(Terrain terrain, vec2 relativeXZ, vec3 normal, out float w[9]) {
    vec2 worldXZ = relativeXZ + pc.scene.cameraPosition.xz;
    vec2 p = terrainNoiseCoord(terrain, relativeXZ);
    float edge = (groundNoise(p, 8.0, 23u) - 0.5) * 0.5;
    float patches = groundNoise(p, 2.0, 21u);
    float curvature = groundCurvature(terrain, worldXZ);
    float flatGround = smoothstep(0.93, 0.98, normal.y);
    float belowRock = 1.0 - smoothstep(terrain.rockNormalY.y, terrain.rockNormalY.y + 0.08,
                                       normal.y);
    float rock = 1.0 - smoothstep(terrain.rockNormalY.x, terrain.rockNormalY.y, normal.y);

    w[0] = 1.0;
    w[1] = flatGround * smoothstep(0.006, 0.015, curvature + edge * 0.01) * smoothstep(0.45, 0.6, patches);
    w[2] = flatGround * smoothstep(0.02, 0.04, curvature + edge * 0.02);
    w[3] = max(belowRock, smoothstep(0.03, 0.06, -curvature + edge * 0.02)) *
           smoothstep(0.45, 0.6, patches + edge * 0.5);
    float m = forestMask(terrain, worldXZ);
    float lo = float(terrain.forestMaskLo) / 65535.0;
    float d = float(terrain.forestMaskHi - terrain.forestMaskLo) / 65535.0;
    // Path over the outer half of the trees' ramp (sparse outer trees),
    // floor from its middle in, under the canopy.
    w[4] = m < 0.0 ? 0.0 : smoothstep(lo - 0.25 * d, lo + 0.25 * d, m + edge * d);
    w[5] = m < 0.0 ? 0.0 : smoothstep(lo + 0.4 * d, lo + d, m + edge * d);
    w[6] = (1.0 - flatGround) * smoothstep(0.045, 0.08, curvature + edge * 0.03);
    w[7] = (1.0 - smoothstep(terrain.rockNormalY.y, terrain.rockNormalY.y + 0.04, normal.y)) *
           smoothstep(0.3, 0.5, patches + edge);
    w[8] = rock;

}

// Large-scale brightness and hue variation (~400 m and ~160 m), so no
// stretch of one layer looks uniform.
vec3 terrainMacroTint(vec2 p) {
    float macro = 0.6 * groundNoise(p, 0.1, 24u) + 0.4 * groundNoise(p, 0.25, 25u);
    float hue = groundNoise(p, 0.25, 26u) - 0.5;
    return (0.85 + 0.3 * macro) * vec3(1.0 + 0.1 * hue, 1.0, 1.0 - 0.1 * hue);
}

#endif
