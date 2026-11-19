// The terrain heightfield and its CDLOD quadtree (Strugar 2009), shared by
// terrain_select.comp, terrain.mesh and the terrain variant of mesh.frag.
// Include after common.glsl.
#ifndef TERRAIN_GLSL
#define TERRAIN_GLSL

#include "material.glsl"

// Level 0 nodes are 16 m of 32x32 quads (0.5 m); each level doubles both.
const float kTerrainLeafSize = 16.0;
const uint kTerrainNodeQuads = 32u;
// Must match kMaxTerrainNodes and kTerrainTasksPerNode in C++: selection
// and draw list capacity; mesh shader workgroups per node.
const uint kMaxTerrainNodes = 8192u;
const uint kTerrainTasksPerNode = 16u;
// Vertices morph toward the next coarser grid over the last 30% of their
// level's distance span.
const float kTerrainMorphZone = 0.3;
// Procedural detail displacement on the two levels finer than the data:
// at most this many meters either way.
const float kTerrainDetailAmplitude = 0.3;

float terrainNodeSize(uint level) {
    return kTerrainLeafSize * float(1u << level);
}

float terrainLodRange(Terrain t, uint level) {
    return t.lodRange0 * float(1u << level);
}

// World (x, z) of sample (0, 0).
vec2 terrainSampleOrigin(Terrain t) {
    return -0.5 * vec2(float(t.width - 1u), float(t.height - 1u)) * t.spacing;
}

// The root node is centered on the origin, shifted by less than one sample
// spacing so that grid vertices at the data's spacing land on samples.
vec2 terrainRootOrigin(Terrain t) {
    vec2 o = terrainSampleOrigin(t);
    return vec2(-0.5 * terrainNodeSize(t.rootLevel)) + (o - t.spacing * floor(o / t.spacing));
}

// Nodes are (level, x, z) with x, z < 2^(rootLevel - level) <= 2^14.
uint terrainEncodeNode(uint level, uint x, uint z) {
    return level | (x << 4) | (z << 18);
}

uvec3 terrainDecodeNode(uint node) {
    return uvec3(node & 0xFu, (node >> 4) & 0x3FFFu, node >> 18);
}

// floor(i / n) for any sign; the float estimate is corrected because GPU
// division isn't exact.
int terrainFloorDiv(int i, int n) {
    int q = int(floor(float(i) / float(n)));
    int r = i - q * n;
    return q + (r < 0 ? -1 : 0) + (r >= n ? 1 : 0);
}

// Same rule as Heightfield::sample on the CPU: mirrored repeat, the edge
// sample duplicated, period 2n.
int terrainMirror(int i, int n) {
    if (uint(i) < uint(n)) {
        return i;
    }
    int m = i - 2 * n * terrainFloorDiv(i, 2 * n);
    return m < n ? m : 2 * n - 1 - m;
}

// The 16-bit sample at (i, j), mirrored, as an integer value: exact,
// unlike filtered reads.
float terrainRawSample(uint heightTexture, ivec2 size, int i, int j) {
    ivec2 texel = ivec2(terrainMirror(i, size.x), terrainMirror(j, size.y));
    return round(texelFetch(textures[heightTexture], texel, 0).r * 65535.0);
}

float terrainSample(Terrain t, int i, int j) {
    return t.minHeight +
           t.heightScale * terrainRawSample(t.heightTexture, ivec2(t.width, t.height), i, j);
}

// Bilinear height at world (x, z), as Heightfield::heightAt.
float terrainHeight(Terrain t, vec2 p) {
    vec2 g = p / t.spacing + 0.5 * vec2(float(t.width - 1u), float(t.height - 1u));
    vec2 g0 = floor(g);
    ivec2 i = ivec2(g0);
    vec2 f = g - g0;
    float s00 = terrainSample(t, i.x, i.y);
    float s10 = terrainSample(t, i.x + 1, i.y);
    float s01 = terrainSample(t, i.x, i.y + 1);
    float s11 = terrainSample(t, i.x + 1, i.y + 1);
    float top = s00 + (s10 - s00) * f.x;
    float bottom = s01 + (s11 - s01) * f.x;
    return top + (bottom - top) * f.y;
}

// The samples the index range [lo, hi] maps to through the mirror, as one
// conservative range in [0, n). A range shorter than n crosses at most one
// mirror edge; the end of an even period half maps to n - 1, of an odd one
// to 0.
ivec2 terrainMirrorRange(int lo, int hi, int n) {
    if (hi - lo + 1 >= n) {
        return ivec2(0, n - 1);
    }
    int a = terrainMirror(lo, n);
    int b = terrainMirror(hi, n);
    ivec2 range = ivec2(min(a, b), max(a, b));
    int half0 = terrainFloorDiv(lo, n);
    if (half0 != terrainFloorDiv(hi, n)) {
        int edge = (half0 & 1) == 0 ? n - 1 : 0;
        range = ivec2(min(range.x, edge), max(range.y, edge));
    }
    return range;
}

// Lowest and highest terrain over the world rectangle [lo, hi] (x, z),
// including detail displacement, from the min/max pyramid.
vec2 terrainHeightBounds(Terrain t, vec2 lo, vec2 hi) {
    vec2 o = terrainSampleOrigin(t);
    ivec2 first = ivec2(floor((lo - o) / t.spacing));
    ivec2 last = ivec2(ceil((hi - o) / t.spacing));
    ivec2 rx = terrainMirrorRange(first.x, last.x, int(t.width));
    ivec2 rz = terrainMirrorRange(first.y, last.y, int(t.height));
    // The level whose texels (2^level samples) cover the range in at most
    // 2x2 of them.
    int extent = max(rx.y - rx.x, rz.y - rz.x) + 1;
    uint level = clamp(uint(findMSB(extent - 1) + 1), 1u, t.pyramidLevels);
    uint levelWidth = (t.width + (1u << level) - 1u) >> level;
    uvec2 a = uvec2(rx.x, rz.x) >> level;
    uvec2 b = uvec2(rx.y, rz.y) >> level;
    uint low = 0xFFFFu;
    uint high = 0u;
    for (uint z = a.y; z <= b.y; ++z) {
        for (uint x = a.x; x <= b.x; ++x) {
            uint v = t.pyramid.minMax[t.pyramidOffsets[level] + z * levelWidth + x];
            low = min(low, v & 0xFFFFu);
            high = max(high, v >> 16);
        }
    }
    return t.minHeight + t.heightScale * vec2(float(low), float(high)) +
           vec2(-kTerrainDetailAmplitude, kTerrainDetailAmplitude);
}

// 0 until the last kTerrainMorphZone of the level's span (from the previous
// level's range to its own), then up to 1 at its range.
float terrainMorph(Terrain t, uint level, float distanceToCamera) {
    float end = terrainLodRange(t, level);
    float start = level == 0u ? 0.0 : 0.5 * end;
    float morphStart = mix(start, end, 1.0 - kTerrainMorphZone);
    return clamp((distanceToCamera - morphStart) / (end - morphStart), 0.0, 1.0);
}

// Detail displacement weight: 1 within level 0's range, 0 from the end of
// level 1's, so level >= 2 vertices are exactly the heightfield.
float terrainDetailWeight(Terrain t, float distanceToCamera) {
    return 1.0 - smoothstep(terrainLodRange(t, 0u), terrainLodRange(t, 1u),
                            distanceToCamera);
}

float terrainHash(ivec2 cell, uint seed) {
    uint h = uint(cell.x) * 374761393u + uint(cell.y) * 668265263u + seed * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(h & 0xFFFFFFu) / float(0xFFFFFFu);
}

// Value noise in [-1, 1] and its gradient (Quilez, "Value noise
// derivatives").
vec3 terrainValueNoise(vec2 p, uint seed) {
    vec2 cell = floor(p);
    vec2 f = p - cell;
    vec2 u = f * f * (3.0 - 2.0 * f);
    vec2 du = 6.0 * f * (1.0 - f);
    ivec2 c = ivec2(cell);
    float a = terrainHash(c, seed);
    float b = terrainHash(c + ivec2(1, 0), seed);
    float d = terrainHash(c + ivec2(0, 1), seed);
    float e = terrainHash(c + ivec2(1, 1), seed);
    float k1 = b - a;
    float k2 = d - a;
    float k3 = a - b - d + e;
    float value = a + k1 * u.x + k2 * u.y + k3 * u.x * u.y;
    vec2 gradient = du * vec2(k1 + k3 * u.y, k2 + k3 * u.x);
    return vec3(value, gradient) * 2.0 - vec3(1.0, 0.0, 0.0);
}

// Detail displacement at world (x, z): height (m) and its x/z gradient.
// Wavelengths 4, 2, 1 and 0.5 m; the amplitudes sum to
// kTerrainDetailAmplitude.
vec3 terrainDetail(vec2 p) {
    const float kAmplitudes[4] = float[4](0.15, 0.08, 0.045, 0.025);
    vec3 sum = vec3(0.0);
    float frequency = 0.25;
    for (uint octave = 0u; octave < 4u; ++octave) {
        vec3 n = terrainValueNoise(p * frequency, octave);
        sum += kAmplitudes[octave] * vec3(n.x, n.yz * frequency);
        frequency *= 2.0;
    }
    return sum;
}

#endif
