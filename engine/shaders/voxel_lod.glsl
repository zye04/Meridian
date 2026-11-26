// The voxel clusters of the cluster DAGs (docs/superpowers/specs/
// 2026-11-24-dag-voxel-foliage-design.md; cluster.h, voxelize.h): their
// pools, the voxel lists cluster_cull.comp, voxel_bricks.comp and
// voxel_instance.mesh pass along, and the voxel payload. Include after
// common.glsl. Layouts must match gpu_types.h.

#ifndef VOXEL_LOD_GLSL
#define VOXEL_LOD_GLSL

#extension GL_EXT_buffer_reference : require

// DrawCounters words (gpu_types.h): per phase voxel_bricks.comp's dispatch
// (x = voxel clusters), per phase and list (only list phase * 2 is used)
// voxel_instance.mesh's task arguments (one group per 64 voxels), 3 uints
// each, the brick counts per phase and the voxel counts per list, then the
// drops.
const uint kCounterVoxelDispatch = 152u;
const uint kCounterVoxelTasks = 158u;
const uint kCounterVoxelBricks = 170u;
const uint kCounterVoxelQuads = 172u;
const uint kCounterVoxelDrops = 176u;
const uint kCounterVoxelArea = 177u;
const uint kCounterVoxelSubpixel = 178u;

// Must match gpu_types.h.
const uint kMaxVoxelInstances = 6650000u;
const uint kMaxVoxelQuads = 24660000u;
// Where the voxel lists start in SceneData.voxelInstances (uvec2 entries:
// cluster entry | terrain sun << 31, pool brick << 6 | voxel), per phase
// after the cluster lists.
const uint kVoxelQuadListBase = 2u * kMaxVoxelInstances;

// The first entry of a phase's voxel list (list = phase * 2).
uint voxelQuadListStart(uint list) {
    return kVoxelQuadListBase + (list >> 1) * kMaxVoxelQuads;
}

// A voxel this deep in its model seen from the camera (its axis depths,
// voxelBoxDepth) shows through less than 3% of the time: skipped.
const float kVoxelHiddenDepth = 3.5;

// GpuVoxelBrick: voxel i present when bit i of occupied is set, stored at
// firstVoxel + the present voxels below it.
struct VoxelBrick {
    int originX, originY, originZ;
    uint firstVoxel;
    uint occupiedLo, occupiedHi;
};

layout(buffer_reference, std430) readonly buffer VoxelBrickBuffer {
    VoxelBrick bricks[];
};
// meridian::PackedVoxel: depth x, y, z and lobe alignment (8 bits each);
// albedo as sqrt RGB + roughness; transmission as sqrt RGB; lobe axis,
// octahedral 2 x 16 bits.
layout(buffer_reference, std430) readonly buffer VoxelDataBuffer {
    uvec4 voxels[];
};
layout(buffer_reference, std430) readonly buffer VoxelAxisBuffer {
    uint words[];
};
// Must match GpuVoxelPools.
layout(buffer_reference, std430) readonly buffer VoxelPoolBuffer {
    VoxelBrickBuffer bricks;
    VoxelDataBuffer voxels;
    VoxelAxisBuffer axis;
};

bool voxelPresent(VoxelBrick brick, uint i) {
    return ((i < 32u ? brick.occupiedLo >> i : brick.occupiedHi >> (i - 32u)) & 1u) != 0u;
}

// Where voxel i of a brick is stored.
uint voxelSlot(VoxelBrick brick, uint i) {
    uint below = i < 32u ? bitCount(brick.occupiedLo & ((1u << i) - 1u))
                         : bitCount(brick.occupiedLo) +
                               bitCount(brick.occupiedHi & ((1u << (i - 32u)) - 1u));
    return brick.firstVoxel + below;
}

// Voxel i's lattice index.
ivec3 voxelCoords(VoxelBrick brick, uint i) {
    return ivec3(brick.originX, brick.originY, brick.originZ) +
           ivec3(i & 3u, (i >> 2) & 3u, i >> 4);
}

vec3 voxelDepths(uvec4 v) {
    return vec3(uvec3(v.x, v.x >> 8, v.x >> 16) & 255u) * (1.0 / 16.0);
}

// The normal lobe's alignment: 1 when every leaf faces along its axis, 1/3
// for leaves facing every way.
float voxelAlignment(uvec4 v) {
    return float(v.x >> 24) * (1.0 / 255.0) / 1.5 + 1.0 / 3.0;
}

vec3 voxelSqrtRgb(uint word) {
    vec3 s = vec3(uvec3(word, word >> 8, word >> 16) & 255u) * (1.0 / 255.0);
    return s * s;
}

float voxelRoughness(uvec4 v) {
    return float(v.y >> 24) * (1.0 / 255.0);
}

vec3 voxelLobeAxis(uvec4 v) {
    vec2 e = vec2(float(v.w & 0xFFFFu), float(v.w >> 16)) * (2.0 / 65535.0) - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * mix(vec2(-1.0), vec2(1.0), greaterThanEqual(n.xy, vec2(0.0)));
    }
    return normalize(n);
}

// A cube of edge v seen along unit d (mesh space) covers v^2 S on screen,
// S = |dx| + |dy| + |dz| (voxelQuadArea, voxel_shape.h, is the CPU mirror),
// and its mean chord is v / S; its extinction along d blends the axes' by
// d^2. Opacity along the view over that chord: over many voxels the covered
// area is the voxels' coverage.
float voxelViewOpacity(vec3 depths, vec3 d) {
    float s = abs(d.x) + abs(d.y) + abs(d.z);
    return 1.0 - exp(-dot(d * d, depths) / s);
}

// Whether the coverage hash draws a voxel of that opacity along the view:
// a voxel of an opaque part (no transmission: wood) at or over the floor
// always draws. Drawn or dropped whole, coverage f errs by 2 f (1 - f) of
// the quad on average through the hash and by 1 - f drawn always, less from
// f = 1/2. Must match voxel_shape.h.
const float kOpaqueVoxelFloor = 0.5;
bool voxelDraws(uvec4 voxel, float opacity, float noise) {
    bool opaque = (voxel.z & 0xFFFFFFu) == 0u;
    return noise < opacity || (opaque && opacity >= kOpaqueVoxelFloor);
}

// The voxel's stable hash, keyed on where its brick is (lattice origin, edge)
// and the cluster's seed (firstVertex: the mesh's seed and the cluster's
// index in its hierarchy), not pool or mesh upload indices, so a cache
// rebuild or a new load order keeps the pattern, and two clusters sharing a
// brick roll independently. The seed is hashed first: XORed raw into all
// three axes, a brick at O in cluster i and one at O ^ (i ^ j) in cluster j
// would hash alike. x: the draw noise; y, z: AreaOffset's place and Phase's
// shift.
uvec3 voxelHash(uint instance, VoxelBrick brick, float edge, uint clusterSeed, uint i) {
    uint place = pcg3d(uvec3(uint(brick.originX), uint(brick.originY),
                             uint(brick.originZ) ^ floatBitsToUint(edge)) ^
                       pcg3d(uvec3(clusterSeed, 0x9E3779B9u, 0x85EBCA6Bu))).x;
    return pcg3d(uvec3(instance, place, i));
}

float voxelUnit(uint bits) {
    return float(bits) * (1.0 / 4294967296.0);
}

// Development coverage rules (--voxel-coverage, SceneData.voxelCoverage):
// rule in the low byte, the jitter phase in bits 8-15, the phases per cycle
// from bit 16. Must match voxel_shape.h (VoxelCoverage).
const uint kVoxelCoverageHash = 0u;
const uint kVoxelCoverageArea = 1u;
const uint kVoxelCoverageAreaOffset = 2u;
const uint kVoxelCoverageMixed = 3u;
const uint kVoxelCoveragePhase = 4u;
const float kPhaseVoxelPixels = 1.0;

// The rule of a view: cascades keep the hash.
uint voxelCoverageRule(uint coverage, bool cascade) {
    return cascade ? kVoxelCoverageHash : coverage & 255u;
}

float voxelAreaFraction(uint rule, float f) {
    return rule == kVoxelCoverageArea || rule == kVoxelCoverageAreaOffset ? f
           : rule == kVoxelCoverageMixed                                  ? sqrt(f)
                                                                          : 1.0;
}

bool voxelPhaseDraws(float f, uint phase, uint phases, uint offset) {
    uint draws = uint(round(float(phases) * f));
    uint j = (phase + offset) % phases;
    return (j + 1u) * draws / phases > j * draws / phases;
}

bool voxelCoverageDraws(uint rule, uint coverage, uvec4 voxel, float f, uvec3 hash, float pixels) {
    float noise = voxelUnit(hash.x);
    if (rule == kVoxelCoverageArea || rule == kVoxelCoverageAreaOffset) {
        return f > 0.0;
    }
    if (rule == kVoxelCoverageMixed) {
        return noise < sqrt(f);
    }
    if (rule == kVoxelCoveragePhase) {
        return pixels < kPhaseVoxelPixels
                   ? voxelPhaseDraws(f, (coverage >> 8) & 255u, coverage >> 16, hash.z)
                   : noise < f;
    }
    return voxelDraws(voxel, f, noise);
}

// AreaOffset: the scaled quad's centre in half sizes of the full quad.
vec2 voxelQuadOffset(uint rule, float f, uvec3 hash) {
    if (rule != kVoxelCoverageAreaOffset) {
        return vec2(0.0);
    }
    return (2.0 * vec2(voxelUnit(hash.y), voxelUnit(hash.z)) - 1.0) * (1.0 - sqrt(f));
}

// The optical depth from a voxel's centre out of its model along unit d
// (mesh space), from its axis word (ClusterLod::voxelAxis: 4 bits per
// direction +x, -x, +y, -y, +z, -z), weighted by d^2 like an ambient cube.
const float kVoxelAxisDepthStep = 0.5;
float voxelBoxDepth(uint axisWord, vec3 d) {
    uvec3 shift = 4u * (uvec3(0u, 2u, 4u) + uvec3(lessThan(d, vec3(0.0))));
    vec3 depth = vec3((uvec3(axisWord) >> shift) & 15u) * kVoxelAxisDepthStep;
    return dot(d * d, depth);
}

#endif
