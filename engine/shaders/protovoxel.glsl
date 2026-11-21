// Mid tier: each tree prototype as bricks of 4^3 voxels in its own space
// (protovoxel.h, renderer_farfield.cpp). Include after common.glsl and
// farfield.glsl (the voxel packing). Layouts must match gpu_types.h.
#ifndef PROTOVOXEL_GLSL
#define PROTOVOXEL_GLSL

#extension GL_EXT_buffer_reference : require

// DrawCounters words (gpu_types.h): per phase voxel_bricks.comp's dispatch
// (x = voxel trees), per phase and list (0 past the switch band, 1 in it)
// voxel_instance.mesh's task arguments (one group per 64 voxels), 3 uints
// each, the brick counts per phase and the voxel counts per list, then the
// drops.
const uint kCounterVoxelDispatch = 152u;
const uint kCounterVoxelTasks = 158u;
const uint kCounterVoxelBricks = 170u;
const uint kCounterVoxelQuads = 172u;
const uint kCounterVoxelDrops = 176u;

// Must match gpu_types.h and protovoxel.h.
const uint kMaxVoxelInstances = 1u << 18;
const uint kMaxVoxelBricks = 1u << 22;
const uint kMaxVoxelQuads = 1u << 23;
const uint kMaxVoxelBandQuads = 1u << 21;
// Where the lists start in SceneData.voxelInstances (uvec2 entries).
const uint kVoxelBrickListBase = 2u * kMaxVoxelInstances;
const uint kVoxelQuadListBase = kVoxelBrickListBase + 2u * kMaxVoxelBricks;
// The first uint of a voxel list (phase * 2 + band) in
// SceneData.voxelInstances viewed as uints, and its capacity.
uint voxelQuadListStart(uint list) {
    uint phase = list >> 1;
    return 2u * kVoxelQuadListBase +
           ((list & 1u) == 0u ? phase * kMaxVoxelQuads
                              : 2u * kMaxVoxelQuads + phase * kMaxVoxelBandQuads);
}
uint voxelQuadListCapacity(uint list) {
    return (list & 1u) == 0u ? kMaxVoxelQuads : kMaxVoxelBandQuads;
}

// A voxel this deep in its crown seen from the camera (its baked axis
// depths, protoVoxelDepth) shows through less than 3% of the time: skipped.
const float kVoxelHiddenDepth = 3.5;
const float kVoxelSwitchPixels = 2.0;
const float kVoxelSwitchBand = 0.1;
const float kVoxelMipPixels = 2.0;

struct ProtoVoxelLevel {
    float voxelSize;
    // Occupied bricks: the pool's brickOffset .. + brickCount.
    uint brickOffset;
    uint brickCount;
    uint pad;
};

struct PrototypeVoxels {
    vec3 lo;
    uint levelCount;
    vec3 hi;
    uint firstLevel;
};

layout(buffer_reference, std430) readonly buffer ProtoVoxelPrototypeBuffer {
    PrototypeVoxels prototypes[];
};
layout(buffer_reference, std430) readonly buffer ProtoVoxelLevelBuffer {
    ProtoVoxelLevel levels[];
};
layout(buffer_reference, std430) readonly buffer ProtoVoxelDataBuffer {
    uint voxels[];
};

// Must match GpuProtoVoxels.
layout(buffer_reference, std430) readonly buffer ProtoVoxelBuffer {
    ProtoVoxelPrototypeBuffer prototypes;
    ProtoVoxelLevelBuffer levels;
    ProtoVoxelDataBuffer voxels;
    ProtoVoxelDataBuffer occlusion;
    // Per pool brick: its coordinates in its level, x | y << 10 | z << 20.
    ProtoVoxelDataBuffer brickCoords;
};

// ------------------------------------------------------------- Switch rule
// (must match voxelSwitchDistance, voxelBlend and voxelMip in protovoxel.h)

float pixelsPerMeter(SceneBuffer scene) {
    return abs(scene.projectionScale.y) * 0.5 / scene.inverseResolution.y;
}

float voxelSwitchDistance(SceneBuffer scene, float finestVoxel, float scale) {
    return finestVoxel * scale * pixelsPerMeter(scene) / kVoxelSwitchPixels;
}

float voxelBlend(float distance, float switchDistance) {
    return clamp((distance - switchDistance) / (kVoxelSwitchBand * switchDistance), 0.0, 1.0);
}

uint voxelMip(SceneBuffer scene, float distance, float finestVoxel, float scale,
              uint levelCount) {
    float ratio = kVoxelMipPixels * distance / (pixelsPerMeter(scene) * finestVoxel * scale);
    return min(uint(floor(log2(max(ratio, 1.0)))), levelCount - 1u);
}

// The voxel representation's weight for a placed instance, its distance
// taken to the prototype's bounding sphere center: cull.comp and mesh.mesh
// both call this, so the voxels' and the masked clusters' dither masks stay
// complementary (opaque parts, trunks, draw whole across the band). 0 when
// voxel trees are off or the prototype has none.
float instanceVoxelBlend(SceneBuffer scene, LoadedInstance instance, PrototypeInfo prototype) {
    if (scene.voxelTrees == 0u || prototype.voxelSize <= 0.0) {
        return 0.0;
    }
    vec3 relative = (instance.transform * vec4(prototype.boundsCenter, 1.0)).xyz -
                    scene.cameraPosition;
    return voxelBlend(length(relative),
                      voxelSwitchDistance(scene, prototype.voxelSize, instance.scale));
}

// The optical depth from a voxel's center out of its prototype's box along
// unit direction d (prototype space): the occlusion word's axis depths
// (protovoxel.h), weighted by d^2 like an ambient cube.
const float kProtoAxisDepthStep = 0.5;
float protoVoxelDepth(uint occlusionWord, vec3 d) {
    uvec3 shift = 8u + 4u * (uvec3(0u, 2u, 4u) + uvec3(lessThan(d, vec3(0.0))));
    vec3 depth = vec3((uvec3(occlusionWord) >> shift) & 15u) * kProtoAxisDepthStep;
    return dot(d * d, depth);
}

// Where a ray from inside the prototype's box leaves it (prototype space).
float protoBoxExit(PrototypeVoxels info, vec3 origin, vec3 direction) {
    vec3 inverse = 1.0 / mix(vec3(1e-12), direction, greaterThan(abs(direction), vec3(1e-12)));
    vec3 t = max((info.lo - origin) * inverse, (info.hi - origin) * inverse);
    return max(min(min(t.x, t.y), t.z), 0.0);
}

// Per pixel and frame, shared by both representations: the voxels draw
// where it is below their weight, the clusters elsewhere.
float voxelDither(vec2 pixel, uint frame) {
    return interleavedGradientNoise(pixel, frame);
}

#endif
