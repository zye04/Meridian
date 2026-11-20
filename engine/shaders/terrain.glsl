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
float terrainNodeSize(uint level) {
    return kTerrainLeafSize * float(1u << level);
}

// World (x, z) of sample (0, 0).
vec2 terrainSampleOrigin(Terrain t) {
    return -0.5 * vec2(float(t.width - 1u), float(t.height - 1u)) * t.spacing;
}

// World (x, z) of a node's corner. The root is centered on the origin,
// shifted by less than one sample spacing so that grid vertices at the
// data's spacing land on samples. The shift is added last: the root's
// corner (~524 km out) would round it to centimeters.
vec2 terrainNodeOrigin(Terrain t, uint level, uvec2 index) {
    vec2 o = terrainSampleOrigin(t);
    return (vec2(index) * terrainNodeSize(level) - 0.5 * terrainNodeSize(t.rootLevel)) +
           (o - t.spacing * floor(o / t.spacing));
}

// Nodes are (level, x, z) with x, z < 2^(rootLevel - level) <= 2^16.
uvec2 terrainEncodeNode(uint level, uint x, uint z) {
    return uvec2(level | (x << 5), z);
}

uvec3 terrainDecodeNode(uvec2 node) {
    return uvec3(node.x & 0x1Fu, node.x >> 5, node.y);
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

// How far the bilinear cell under p rises above the plane through the
// surface point (p, height) with this normal: the highest of its corners,
// since the cell minus a plane is bilinear. 0 if it stays below.
float terrainCellAbovePlane(Terrain t, vec2 p, float height, vec3 normal) {
    vec2 g = p / t.spacing + 0.5 * vec2(float(t.width - 1u), float(t.height - 1u));
    vec2 g0 = floor(g);
    ivec2 i = ivec2(g0);
    vec2 slope = -normal.xz / normal.y;
    float above = 0.0;
    for (int corner = 0; corner < 4; ++corner) {
        ivec2 c = ivec2(corner & 1, corner >> 1);
        vec2 offset = (g0 + vec2(c) - g) * t.spacing;
        above = max(above, terrainSample(t, i.x + c.x, i.y + c.y) - (height + dot(slope, offset)));
    }
    return above;
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

// ---------------------------------------------------------------- Shadows

// Highest terrain (m) over pyramid level >= 1 texel c, which bounds the
// bilinear surface over its cells. Reads the offsets through the scene
// reference: indexing a copied Terrain's array spills it to local memory,
// one more dependent load per march step.
float terrainCellMax(SceneBuffer scene, uint level, uvec2 c) {
    Terrain t = scene.terrain;
    uint levelWidth = (t.width + (1u << level) - 1u) >> level;
    uint v = t.pyramid.minMax[scene.terrain.pyramidOffsets[level] + c.y * levelWidth + c.x];
    return t.minHeight + t.heightScale * float(v >> 16);
}

#ifdef TERRAIN_MARCH_BRUTE_FORCE
// Reference for A/B checks: bilinear height every 1 m for 2 km, then every
// 8 m.
bool terrainOccludes(SceneBuffer scene, vec3 origin, vec3 direction, float maxDistance) {
    Terrain t = scene.terrain;
    if (t.width == 0u) {
        return false;
    }
    float top = terrainCellMax(scene, t.pyramidLevels, uvec2(0u));
    float s = 0.0;
    while (s < maxDistance) {
        vec3 p = origin + s * direction;
        if (p.y > top && direction.y >= 0.0) {
            return false;
        }
        if (p.y < terrainHeight(t, p.xz)) {
            return true;
        }
        s += s < 2000.0 ? 1.0 : 8.0;
    }
    return false;
}
#else
// Whether the bilinear heightfield blocks the ray (flat world): a march of
// the max pyramid (Tevs 2008). Each step tests the cell around the ray at
// the current level: if the ray stays above the cell's max over the cell,
// it skips to the cell's exit and tries one level coarser; otherwise it
// descends. Level 0 solves the ray against the bilinear cell exactly.
//
// Outside the crop the heights mirror. Within one period tile ([-0.5,
// w - 0.5) in sample units, the mirror plane halfway between two equal
// edge samples) the mirrored field is the crop with clamped indices, in
// local coordinates that reverse on odd tiles; steps stop at tile edges so
// a cell never straddles one.
bool terrainOccludes(SceneBuffer scene, vec3 origin, vec3 direction, float maxDistance) {
    Terrain t = scene.terrain;
    if (t.width == 0u) {
        return false;
    }
    float tEnd = maxDistance;
    float top = terrainCellMax(scene, t.pyramidLevels, uvec2(0u));
    if (direction.y > 0.0) {
        tEnd = min(tEnd, (top - origin.y) / direction.y);
    }
    vec2 size = vec2(float(t.width), float(t.height));
    vec2 g0 = (origin.xz - terrainSampleOrigin(t)) / t.spacing;
    vec2 dg = direction.xz / t.spacing;
    dg = mix(dg, vec2(1e-12), equal(dg, vec2(0.0)));
    // Cell lookups are nudged across the boundary the last step exited
    // through (a position computed there can round to just before it), and
    // only on that axis: nudging the other could cross into the next row.
    const float kNudge = 0.01;
    vec2 nudge = vec2(0.0);

    float s = 0.0;
    uint level = 0u;
    // The tile's local position at s = 0 and local direction, and where
    // the ray leaves the tile; set when s reaches it.
    vec2 lBase = vec2(0.0);
    vec2 dl = vec2(0.0);
    vec2 inverseDl = vec2(0.0);
    vec2 exitSide = vec2(0.0);
    vec2 tileExit = vec2(-1.0);
    float sTileExit = -1.0;
    for (uint i = 0u; i < 256u && s < tEnd; ++i) {
        if (s >= sTileExit) {
            vec2 tile = floor((g0 + s * dg + sign(dg) * nudge + 0.5) / size);
            bvec2 odd = equal(mod(tile, 2.0), vec2(1.0));
            lBase = mix(g0 - tile * size, (tile + 1.0) * size - 1.0 - g0, odd);
            dl = mix(dg, -dg, odd);
            inverseDl = 1.0 / dl;
            exitSide = step(0.0, dl);
            tileExit = (mix(vec2(-0.5), size - 0.5, exitSide) - lBase) * inverseDl;
            sTileExit = min(tileExit.x, tileExit.y);
        }
        vec2 l = lBase + s * dl;
        vec2 ln = l + sign(dl) * nudge;

        float cellSize = float(1u << level);
        vec2 cell = floor(ln / cellSize);
        if (level > 0u) {
            vec2 levelSize = ceil(size / cellSize);
            cell = clamp(cell, vec2(0.0), levelSize - 1.0);
        }
        // Edge cells extend to the tile edge, where their exit lies behind.
        vec2 cellExit = max(((cell + exitSide) * cellSize - l) * inverseDl, 0.0);
        cellExit = mix(cellExit, vec2(1e30), equal(cellExit, vec2(0.0)));
        vec2 axisExit = min(cellExit + s, tileExit);
        float sNext = min(min(axisExit.x, axisExit.y), tEnd);

        if (level == 0u) {
            // The 2x2 samples from cell to cell + 1, clamped to the crop
            // by the sampler (CLAMP_TO_EDGE).
            vec4 q = t.minHeight + t.heightScale * 65535.0 *
                     textureGather(textures[t.heightTexture], (cell + 1.0) / size);
            float s00 = q.w;
            float s10 = q.z;
            float s01 = q.x;
            float s11 = q.y;
            // Surface minus ray height along the segment is a quadratic
            // a u^2 + b u + c0 in the distance u past s.
            vec2 f = l - cell;
            float k1 = s10 - s00;
            float k2 = s01 - s00;
            float k3 = s00 - s10 - s01 + s11;
            float c0 = s00 + k1 * f.x + k2 * f.y + k3 * f.x * f.y - (origin.y + s * direction.y);
            float b = k1 * dl.x + k2 * dl.y + k3 * (f.x * dl.y + f.y * dl.x) - direction.y;
            float a = k3 * dl.x * dl.y;
            float u = sNext - s;
            float peak = max(c0, c0 + u * (b + u * a));
            float vertex = a < 0.0 ? -b / (2.0 * a) : -1.0;
            if (vertex > 0.0 && vertex < u) {
                peak = max(peak, c0 + vertex * (b + vertex * a));
            }
            if (peak > 0.0) {
                return true;
            }
        } else {
            float rayLow = origin.y + (direction.y > 0.0 ? s : sNext) * direction.y;
            if (rayLow <= terrainCellMax(scene, level, uvec2(cell))) {
                --level;
                continue;
            }
        }
        s = sNext;
        nudge = kNudge * vec2(lessThanEqual(axisExit, vec2(sNext)));
        level = min(level + 1u, t.pyramidLevels);
    }
    return false;
}
#endif

// 0 until the last kTerrainMorphZone of the level's span (from the previous
// level's range to its own), then up to 1 at its range.
float terrainMorph(Terrain t, uint level, float distanceToCamera) {
    float end = terrainLodRange(t, level);
    float start = level == 0u ? 0.0 : 0.5 * end;
    float morphStart = mix(start, end, 1.0 - kTerrainMorphZone);
    return clamp((distanceToCamera - morphStart) / (end - morphStart), 0.0, 1.0);
}

#endif
