#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#ifdef DEBUG_ID
#extension GL_ARB_fragment_shader_interlock : require
layout(pixel_interlock_unordered) in;
#endif
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
#include "debug_id.glsl"
#endif
layout(depth_less) out float gl_FragDepth;

// Optical depth at opacity 0.5.
const float kHitOpticalDepth = 0.69314718;
// The sun through the cell's own volume: the optical depth from the hit
// toward the sun over at most this far (and inside the cell's box), marched
// through the bricks at the drawn level; the shadow maps and the terrain
// take over where that march ends, so the crown shadows the hit once (its
// copy in cascade 4 or the far shadow map lies behind the march's end).
const float kTileSunMarch = 32.0;
const float kVoxelPerceptualRoughness = 0.8;
// Sky occlusion from the cell's own bricks, in the canopy-sky form of the
// crown-light lane (lighting.glsl canopySkyVisibility, branch crown-light,
// kCanopySkyDistance): the transmittance straight up over 64 m from the hit.
// Unlike that lane's foliage start (4 m), the hit's own voxel counts: the
// hit lies inside the medium, and the cluster voxels' crown occlusion counts
// the model from the voxel too.
const float kTileSkyDistance = 64.0;

void main() {
    SceneBuffer scene = pc.scene;
#ifdef DEBUG_ID
    if (debugFilterRejects(scene.debugFilter, ~0u, ~0u)) {
        discard;
    }
#endif
    FarFieldBuffer farField = scene.farField;
    FarField field = farField.field;
    uint index = inEntry & kFarEntryCellMask;
    uint level = inEntry >> kFarEntryLevelShift;
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
    // The leaves' lobe, as a cluster voxel's (voxel_instance.frag), from the
    // level's mean alignment: the aligned share shades as one leaf, the
    // highlight spreads with the rest (Toksvig).
    float alignment = field.alignment[level];
    float aligned = clamp((3.0 * alignment - 1.0) * 0.5, 0.0, 1.0);
    float alpha = kVoxelPerceptualRoughness * kVoxelPerceptualRoughness;
    s.alpha = 1.0;
    s.metallic = 0.0;
    s.perceptualRoughness = sqrt(sqrt(min(alpha * alpha + 2.0 * (1.0 - alignment), 1.0)));
    s.occlusion = 1.0;
    s.microOcclusion = 1.0;
    s.sunShadow = 1.0;
    s.emissive = vec3(0.0);
    // Foliage is two-sided, like the leaf cards the voxels average.
    s.normal = voxelNormal(voxel);
    if (dot(s.normal, relative) > 0.0) {
        s.normal = -s.normal;
    }

    // Two short marches through the cell's own bricks from the hit, at one
    // call site (each inlined copy of the march costs registers): up for the
    // sky (it stays in the cell's column) and toward the sun, out of the
    // hit's voxel, to the cell's box or kTileSunMarch.
    vec3 hitField = origin + direction * t;
    vec3 rayDirection = sampleSunDirection(pc.scene, gl_FragCoord.xy);
    bool sunUp = rayDirection.y > -sin(scene.sunAngularRadius);
    vec3 inverseSun = 1.0 / mix(vec3(1e-12), rayDirection,
                                greaterThan(abs(rayDirection), vec3(1e-12)));
    vec3 sunExit = max((boxLo - hitField) * inverseSun, (boxHi - hitField) * inverseSun);
    float sunEnd = clamp(min(min(sunExit.x, sunExit.y), sunExit.z), 0.0, kTileSunMarch);
    // depths.x: up, depths.y: toward the sun (also what an in-crown fill
    // toward the sun, crown-light lane, would read).
    vec2 depths = vec2(0.0);
    // The fine levels shade from the 4 m volume: their own marches cost the
    // far pass 11.7 ms at a1 and darkened the lit side below the cluster
    // voxels (ledger, stage 2).
    uint shadeLevel = max(level, kFarFineLevels);
    for (uint ray = 0u; ray < (sunUp ? 2u : 1u); ++ray) {
        bool sky = ray == 0u;
        float depth = 0.0;
        uint unused;
        farMarchCell(farField, cell, shadeLevel, hitField,
                     sky ? vec3(0.0, 1.0, 0.0) : rayDirection,
                     sky ? 0.0 : farVoxelSize(shadeLevel),
                     sky ? min(kTileSkyDistance, boxHi.y - hitField.y) : sunEnd, kFarOpaqueDepth,
                     depth, unused);
        depths[ray] = depth;
    }
    float occlusion = mix(1.0, exp(-depths.x), scene.microShadowStrength);

    Shading sh = beginShading(s, relative);
    vec3 sunRadiance = vec3(0.0);
    if (sunUp) {
        vec3 sunStart = world + rayDirection * sunEnd;
        vec3 marchOrigin = sunStart;
        marchOrigin.y = max(marchOrigin.y,
                            terrainHeight(scene.terrain, marchOrigin.xz) + kShadowRayBias);
        float visibility = exp(-depths.y) *
                           traceSunVisibility(sunStart, marchOrigin, rayDirection, 0.0, true,
                                              sunStart - scene.cameraPosition, rayDirection);
        addVoxelFoliageSun(sh, s.baseColor, transmitted, aligned, visibility);
        sunRadiance = sh.diffuseLight + sh.specularLight;
    }
#ifdef DEBUG_ID
    vec3 debugSunDiffuse = sh.diffuseLight;
    vec3 debugSunSpecular = sh.specularLight;
#endif
    addVoxelFoliageAmbient(sh, transmitted, occlusion, occlusion);
    float hazeTransmittance;
    vec3 luminance = applyAerialPerspective(sh.diffuseLight + sh.specularLight, relative, 1.0,
                                            hazeTransmittance);
    vec3 exposure = scene.sunIlluminance * scene.exposure.exposure;
    outColor = vec4(luminance * exposure, 1.0);
    outDiffuseAlbedo = vec4(sh.diffuseColor, 1.0);
    outSpecularAlbedo = vec4(mix(sh.dfg.xxx, sh.dfg.yyy, sh.f0), 1.0);
    outNormalRoughness = vec4(sh.N, sh.perceptualRoughness);
    outSunLight = vec4(sunRadiance * hazeTransmittance * exposure, 1.0);
    writeDebugId(kDebugFarCell, 0u, inEntry, relative, ~0u);
#ifdef DEBUG_ID
    vec3 debugScale = exposure * hazeTransmittance;
    beginInvocationInterlockARB();
    writeDebugLight(gl_FragDepth, debugSunDiffuse * debugScale, debugSunSpecular * debugScale,
                    (sh.diffuseLight - debugSunDiffuse) * debugScale,
                    (sh.specularLight - debugSunSpecular) * debugScale, vec2(0.0));
    endInvocationInterlockARB();
#endif

    // Motion of the flat hit, each frame displayed with its own camera and
    // drop (static, so a still camera gives zero).
    vec4 current = scene.currentViewProjection * displayedPosition(scene, relative);
    vec4 previous = scene.previousViewProjection *
                    displayedPosition(scene, relative + (scene.cameraPosition -
                                                         scene.previousCameraPosition));
    outMotion = previous.xy / previous.w * 0.5 - current.xy / current.w * 0.5;
#endif
}
