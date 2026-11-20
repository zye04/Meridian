#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#include "common.glsl"
#include "atmosphere.glsl"
#include "material.glsl"
#include "terrain.glsl"
#include "lighting.glsl"
#include "farfield.glsl"

// A far cell's voxels (voxel.mesh draws its canopy box): marches the view
// ray through the cell's brick columns at the cell's level, voxels constant
// inside, and stops where the accumulated opacity reaches 0.5. A ray that
// leaves the cell below that is discarded. The threshold is deterministic,
// so the image is stable frame to frame; edges antialias through TAA
// jitter. The hit lies behind the box face, so the written depth is never
// nearer than the rasterized one (depth_less keeps early depth testing).

layout(location = 0) in vec3 inDisplayed;
layout(location = 1) flat in uint inEntry;
layout(location = 2) flat in float inDrop;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;
layout(depth_less) out float gl_FragDepth;

// Optical depth at opacity 0.5.
const float kHitOpticalDepth = 0.69314718;
// Shadow rays from a voxel surface start one L0 voxel out, so they don't
// shadow the surface with its own crown.
const float kVoxelRayStart = 4.0;
const float kVoxelPerceptualRoughness = 0.8;
const uint kMaxMarchSteps = 512u;

// Where along the ray (field coordinates: world minus (originX, heightBase,
// originZ)) the optical depth reaches kHitOpticalDepth between tStart and
// tEnd inside the cell, and the voxel there; -1 if it doesn't.
float marchCell(FarField field, uvec2 cell, uint level, vec3 origin, vec3 direction,
                float tStart, float tEnd, out uint hitVoxel) {
    float v = farVoxelSize(level);
    int n = int(kBrickEdge * farColumnsPerCell(level));
    ivec2 lo = ivec2(cell) * n;
    vec3 p = origin + direction * tStart;
    ivec2 voxel = clamp(ivec2(floor(p.xz / v)), lo, lo + n - 1);
    ivec2 stepXZ = ivec2(direction.x >= 0.0 ? 1 : -1, direction.z >= 0.0 ? 1 : -1);
    vec2 inverseXZ = 1.0 / vec2(abs(direction.x) > 1e-12 ? direction.x : 1e-12,
                              abs(direction.z) > 1e-12 ? direction.z : 1e-12);
    vec2 tMax = (vec2(voxel + max(stepXZ, ivec2(0))) * v - origin.xz) * inverseXZ;
    vec2 tDelta = abs(v * inverseXZ);
    float inverseY = 1.0 / (abs(direction.y) > 1e-12 ? direction.y : 1e-12);
    int stepY = direction.y >= 0.0 ? 1 : -1;

    float t = tStart;
    float opticalDepth = 0.0;
    uint steps = 0u;
    while (t < tEnd && steps < kMaxMarchSteps) {
        float tNext = min(min(tMax.x, tMax.y), tEnd);
        uvec2 column = field.columns[level].columns[farColumnIndex(
            field.cellsX, level, uint(voxel.x) / kBrickEdge, uint(voxel.y) / kBrickEdge)];
        uint layers = columnBrickCount(column) * kBrickEdge;
        if (layers > 0u) {
            // The part of this voxel column's segment inside the column's
            // layers, then layer by layer.
            float base = float(columnBaseHeight(column)) * kFarHeightUnit;
            vec2 slab = (vec2(base, base + float(layers) * v) - origin.y) * inverseY;
            float a = max(t, min(slab.x, slab.y));
            float b = min(tNext, max(slab.x, slab.y));
            if (a < b) {
                int k = clamp(int(floor((origin.y + direction.y * a - base) / v)), 0,
                              int(layers) - 1);
                uint firstBrick = column.x;
                while (a < b && steps < kMaxMarchSteps) {
                    ++steps;
                    float boundary = base + float(k + max(stepY, 0)) * v;
                    float end = min((boundary - origin.y) * inverseY, b);
                    uint packed = field.voxels[level].voxels[
                        (firstBrick + uint(k) / kBrickEdge) * kBrickVoxels +
                        brickVoxelIndex(uint(voxel.x), uint(k), uint(voxel.y))];
                    float extinction = voxelExtinction(packed);
                    float segment = extinction * max(end - a, 0.0);
                    if (opticalDepth + segment >= kHitOpticalDepth) {
                        hitVoxel = packed;
                        return a + (kHitOpticalDepth - opticalDepth) / extinction;
                    }
                    opticalDepth += segment;
                    a = end;
                    k += stepY;
                    if (k < 0 || k >= int(layers)) {
                        break;
                    }
                }
            }
        }
        ++steps;
        if (tNext >= tEnd) {
            break;
        }
        if (tMax.x < tMax.y) {
            voxel.x += stepXZ.x;
            tMax.x += tDelta.x;
        } else {
            voxel.y += stepXZ.y;
            tMax.y += tDelta.y;
        }
        if (any(lessThan(voxel, lo)) || any(greaterThanEqual(voxel, lo + n))) {
            break;
        }
        t = tNext;
    }
    return -1.0;
}

void main() {
    SceneBuffer scene = pc.scene;
    FarField field = scene.farField.field;
    uint index = inEntry & 0x3FFFFFFFu;
    uint level = inEntry >> 30u;
    uvec2 cell = uvec2(index % field.cellsX, index / field.cellsX);
    FarCell far = field.cells.cells[index];

    // The displayed ray from the camera, shifted up by the cell's drop: the
    // same ray in the flat world (field coordinates).
    vec3 direction = normalize(inDisplayed);
    vec3 fieldOrigin = vec3(field.originX, field.heightBase, field.originZ);
    vec3 origin = scene.cameraPosition - fieldOrigin + vec3(0.0, inDrop, 0.0);
    vec3 boxLo = vec3(vec2(cell) * kCellSize, far.bottom - field.heightBase).xzy;
    vec3 boxHi = boxLo + vec3(kCellSize, far.top - far.bottom, kCellSize);
    vec3 inverseDirection = 1.0 / mix(vec3(1e-12), direction, greaterThan(abs(direction), vec3(1e-12)));
    vec3 t0 = (boxLo - origin) * inverseDirection;
    vec3 t1 = (boxHi - origin) * inverseDirection;
    vec3 tFar = max(t0, t1);
    float tEnd = min(min(tFar.x, tFar.y), tFar.z);

    uint voxel;
    float t = marchCell(field, cell, level, origin, direction, length(inDisplayed), tEnd, voxel);
    if (t < 0.0) {
        discard;
    }

    vec3 displayedHit = direction * t;
    vec4 clip = scene.viewProjection * vec4(displayedHit, 1.0);
    gl_FragDepth = min(clip.z / clip.w, gl_FragCoord.z);

    // Flat, relative to the camera.
    vec3 relative = displayedHit + vec3(0.0, inDrop, 0.0);
    vec3 world = scene.cameraPosition + relative;

    Surface s;
    s.baseColor = field.palette.albedo[voxelAlbedo(voxel)].rgb;
    s.alpha = 1.0;
    s.metallic = 0.0;
    s.perceptualRoughness = kVoxelPerceptualRoughness;
    s.occlusion = 1.0;
    s.emissive = vec3(0.0);
    // Foliage is two-sided, like the leaf cards the voxels average.
    s.normal = voxelNormal(voxel);
    if (dot(s.normal, relative) > 0.0) {
        s.normal = -s.normal;
    }

    Shading sh = beginShading(s, relative);
    if (sh.NoL > 0.0) {
        vec3 rayDirection = sampleSunDirection(gl_FragCoord.xy);
        vec3 rayOrigin = world + rayDirection * kVoxelRayStart;
        vec3 marchOrigin = rayOrigin;
        marchOrigin.y = max(rayOrigin.y,
                            terrainHeight(scene.terrain, rayOrigin.xz) + kShadowRayBias);
        addSunLight(sh, traceSunVisibility(rayOrigin, marchOrigin, rayDirection));
    }
    addAmbientLight(sh, s.occlusion);
    vec3 luminance = applyAerialPerspective(sh.diffuseLight + sh.specularLight, relative, 1.0);
    outColor = vec4(luminance * scene.sunIlluminance * scene.exposure.exposure, 1.0);

    // Motion of the flat hit, each frame displayed with its own camera and
    // drop (static, so a still camera gives zero).
    vec4 current = scene.currentViewProjection * displayedPosition(scene, relative);
    vec4 previous = scene.previousViewProjection *
                    displayedPosition(scene, relative + (scene.cameraPosition -
                                                         scene.previousCameraPosition));
    outMotion = previous.xy / previous.w * 0.5 - current.xy / current.w * 0.5;
}
