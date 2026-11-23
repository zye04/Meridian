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

#ifndef SHADOW
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;
// The guides and the sun's exposed radiance, as in mesh.frag; the
// transparency mask (location 2) is not written (opaque).
layout(location = 3) out vec4 outDiffuseAlbedo;
layout(location = 4) out vec4 outSpecularAlbedo;
layout(location = 5) out vec4 outNormalRoughness;
layout(location = 6) out vec4 outSunLight;
#endif
layout(depth_less) out float gl_FragDepth;

// Optical depth at opacity 0.5.
const float kHitOpticalDepth = 0.69314718;
// Shadow rays from a voxel surface start one L0 voxel out, so they don't
// shadow the surface with its own crown.
const float kVoxelRayStart = 4.0;
const float kVoxelPerceptualRoughness = 0.8;

void main() {
    SceneBuffer scene = pc.scene;
    FarFieldBuffer farField = scene.farField;
    FarField field = farField.field;
    uint index = inEntry & 0x3FFFFFFFu;
    uint level = inEntry >> 30u;
    uvec2 cell = uvec2(index % field.cellsX, index / field.cellsX);
    FarCell far = field.cells.cells[index];

#ifdef SHADOW
    // A cascade's depth (voxel_shadow.frag): the march goes along the
    // sunlight from the box face; the copy has no curvature.
    vec3 direction = -scene.sunDirection;
    vec3 fieldOrigin = vec3(field.originX, field.heightBase, field.originZ);
    vec3 origin = scene.cameraPosition + inDisplayed - fieldOrigin;
#else
    // The displayed ray from the camera, shifted up by the cell's drop: the
    // same ray in the flat world (field coordinates).
    vec3 direction = normalize(inDisplayed);
    vec3 fieldOrigin = vec3(field.originX, field.heightBase, field.originZ);
    vec3 origin = scene.cameraPosition - fieldOrigin + vec3(0.0, inDrop, 0.0);
#endif
    float pad = farCanopyPad(level);
    vec3 boxLo = vec3(vec2(cell) * kCellSize, far.bottom - pad - field.heightBase).xzy;
    vec3 boxHi = boxLo + vec3(kCellSize, far.top - far.bottom + 2.0 * pad, kCellSize);
    vec3 inverseDirection = 1.0 / mix(vec3(1e-12), direction, greaterThan(abs(direction), vec3(1e-12)));
    vec3 t0 = (boxLo - origin) * inverseDirection;
    vec3 t1 = (boxHi - origin) * inverseDirection;
    vec3 tFar = max(t0, t1);
    float tEnd = min(min(tFar.x, tFar.y), tFar.z);

    uint voxel;
    float opticalDepth = 0.0;
#ifdef SHADOW
    float tStart = 0.0;
#else
    float tStart = length(inDisplayed);
#endif
    float t = farMarchCell(farField, cell, level, origin, direction, tStart, tEnd,
                           kHitOpticalDepth, opticalDepth, voxel);
    if (t < 0.0) {
        discard;
    }
#ifdef SHADOW
    vec4 shadowClip = scene.viewProjection * vec4(inDisplayed + direction * t, 1.0);
    gl_FragDepth = min(shadowClip.z / shadowClip.w, gl_FragCoord.z);
#else

    vec3 displayedHit = direction * t;
    vec4 clip = scene.viewProjection * vec4(displayedHit, 1.0);
    gl_FragDepth = min(clip.z / clip.w, gl_FragCoord.z);

    // Flat, relative to the camera.
    vec3 relative = displayedHit + vec3(0.0, inDrop, 0.0);
    vec3 world = scene.cameraPosition + relative;

    Surface s;
    s.baseColor = field.palette.albedo[voxelAlbedo(voxel)].rgb;
    vec3 transmitted = field.palette.albedo[field.paletteSize + voxelAlbedo(voxel)].rgb;
    s.alpha = 1.0;
    s.metallic = 0.0;
    s.perceptualRoughness = kVoxelPerceptualRoughness;
    s.occlusion = 1.0;
    s.microOcclusion = 1.0;
    s.sunShadow = 1.0;
    s.emissive = vec3(0.0);
    // Foliage is two-sided, like the leaf cards the voxels average.
    s.normal = voxelNormal(voxel);
    if (dot(s.normal, relative) > 0.0) {
        s.normal = -s.normal;
    }

    Shading sh = beginShading(s, relative);
    vec3 sunRadiance = vec3(0.0);
    float backNoL = clamp(-dot(s.normal, sh.L), 0.0, 1.0);
    bool backLit = backNoL > 0.0 && any(greaterThan(transmitted, vec3(0.0)));
    if (sh.NoL > 0.0 || backLit) {
        vec3 rayDirection = sampleSunDirection(pc.scene, gl_FragCoord.xy);
        vec3 marchOrigin = world + rayDirection * kVoxelRayStart;
        marchOrigin.y = max(marchOrigin.y,
                            terrainHeight(scene.terrain, marchOrigin.xz) + kShadowRayBias);
        float visibility = traceSunVisibility(world, marchOrigin, rayDirection, kVoxelRayStart,
                                              true, relative, backLit ? -s.normal : s.normal);
        if (sh.NoL > 0.0) {
            addSunLight(sh, visibility, 1.0);
            sunRadiance = sh.diffuseLight + sh.specularLight;
        }
        addTransmittedSun(sh, transmitted, backNoL, visibility);
    }
    addAmbientLight(sh, s.occlusion);
    addTransmittedAmbient(sh, transmitted, s.occlusion);
    float hazeTransmittance;
    vec3 luminance = applyAerialPerspective(sh.diffuseLight + sh.specularLight, relative, 1.0,
                                            hazeTransmittance);
    vec3 exposure = scene.sunIlluminance * scene.exposure.exposure;
    outColor = vec4(luminance * exposure, 1.0);
    outDiffuseAlbedo = vec4(sh.diffuseColor, 1.0);
    outSpecularAlbedo = vec4(mix(sh.dfg.xxx, sh.dfg.yyy, sh.f0), 1.0);
    outNormalRoughness = vec4(sh.N, sh.perceptualRoughness);
    outSunLight = vec4(sunRadiance * hazeTransmittance * exposure, 1.0);

    // Motion of the flat hit, each frame displayed with its own camera and
    // drop (static, so a still camera gives zero).
    vec4 current = scene.currentViewProjection * displayedPosition(scene, relative);
    vec4 previous = scene.previousViewProjection *
                    displayedPosition(scene, relative + (scene.cameraPosition -
                                                         scene.previousCameraPosition));
    outMotion = previous.xy / previous.w * 0.5 - current.xy / current.w * 0.5;
#endif
}
