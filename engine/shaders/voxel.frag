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
// How far back into the cell a ray comes from the view march starts, in that
// cell's voxels: the canopy just before the border is what moves the hits
// next to it (a ray that crosses more of that cell without reaching opacity
// 0.5 has passed through sparse canopy); marching all of it cost the far
// pass 0.8 ms more at a1.
const float kTileCarryVoxels = 2.0;
// The sun through the forest's volume: the optical depth from the hit
// toward the sun over this far, marched through the bricks at the drawn
// level (farFieldMarch, into the neighbouring cells); the shadow maps and the terrain
// take over where that march ends, so the crown shadows the hit once (its
// copy in cascade 4 or the far shadow map lies behind the march's end).
const float kTileSunMarch = 32.0;
const float kVoxelPerceptualRoughness = 0.8;
// Sky occlusion from the bricks above the hit, in the canopy-sky form of the
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
    // A ray entering through a side face first gathers the optical depth of
    // the cell it comes from (at this level, or that cell's finest stored
    // one), over its last kTileCarryVoxels; a hit there is that cell's
    // fragment's.
    // Restarting at zero put the hits next to every cell border deeper into
    // the canopy: a dark line along each border.
    vec3 tNear = min(t0, t1);
    float tEntry = max(max(tNear.x, tNear.y), tNear.z);
    ivec2 from = ivec2(cell);
    if (tNear.x >= tEntry) {
        from.x -= direction.x >= 0.0 ? 1 : -1;
    } else if (tNear.z >= tEntry) {
        from.y -= direction.z >= 0.0 ? 1 : -1;
    }
    bool carried = from != ivec2(cell) && all(greaterThanEqual(from, ivec2(0))) &&
                   all(lessThan(from, ivec2(field.cellsX, field.cellsZ)));
    float t = -1.0;
    uint pass = carried ? 0u : 1u;
    for (; pass < 2u; ++pass) {
        uvec2 marchCell = cell;
        uint marchLevel = level;
        float a = tStart;
        float b = tEnd;
        if (pass == 0u) {
            marchCell = uvec2(from);
            marchLevel = max(level, farFinestLevel(field.bands, from));
            FarCell previous = field.cells.cells[marchCell.y * field.cellsX + marchCell.x];
            float previousPad = farCanopyPad(marchLevel);
            vec3 lo = vec3(vec2(marchCell) * kCellSize,
                           previous.bottom - previousPad - field.heightBase).xzy;
            vec3 hi = lo + vec3(kCellSize, previous.top - previous.bottom + 2.0 * previousPad,
                                kCellSize);
            vec3 near = min((lo - origin) * inverseDirection, (hi - origin) * inverseDirection);
#ifdef SHADOW
            a = max(max(near.x, near.y), near.z);
#else
            a = max(max(max(near.x, near.y), near.z), 0.0);
#endif
            a = max(a, tEntry - kTileCarryVoxels * farVoxelSize(marchLevel));
            b = tEntry;
        }
        t = farMarchCell(farField, marchCell, marchLevel, origin, direction, a, b,
                         kHitOpticalDepth, opticalDepth, voxel);
        if (t >= 0.0) {
            break;
        }
    }
    if (t < 0.0 || pass == 0u) {
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

    // Two short marches through the field's bricks from the hit, at one
    // call site (each inlined copy of the march costs registers): up for the
    // sky and toward the sun, out of the hit's voxel, over kTileSunMarch.
    // Both cross into the neighbouring cells: stopping at the cell's box left
    // the canopy past it to the shadow maps, whose receiver offsets skip
    // it, and lit a stripe along every cell's sunward edge.
    vec3 rayDirection = sampleSunDirection(pc.scene, gl_FragCoord.xy);
    bool sunUp = rayDirection.y > -sin(scene.sunAngularRadius);
    // depths.x: up, depths.y: toward the sun (also what an in-crown fill
    // toward the sun, crown-light lane, would read).
    vec2 depths = vec2(0.0);
    // The fine levels shade from the 4 m volume: their own marches cost the
    // far pass 11.7 ms at a1 and darkened the lit side below the cluster
    // voxels (ledger, stage 2).
    uint shadeLevel = max(level, kFarFineLevels);
    // The shadow maps and the terrain past the march, looked up first: where
    // they shadow the hit, the sun march is skipped.
    float sunLookup = 0.0;
    if (sunUp) {
        // The lookup starts counting casters its path's receiver offset
        // past its origin (sunLookupOffset: far map texel + bias, cascade
        // 2.5 of its texels, 0 for a ray); placed that much short of the
        // march's end, it counts from exactly where the march stopped. The
        // path is the one at the march's end: one whose boundary lies
        // between the two points (within the offset) can take the other.
        vec3 marchEnd = world + rayDirection * kTileSunMarch;
        vec3 sunStart = marchEnd - rayDirection * sunLookupOffset(marchEnd,
                                                                  marchEnd - scene.cameraPosition);
        vec3 marchOrigin = sunStart;
        marchOrigin.y = max(marchOrigin.y,
                            terrainHeight(scene.terrain, marchOrigin.xz) + kShadowRayBias);
        sunLookup = traceSunVisibility(sunStart, marchOrigin, rayDirection, 0.0, true,
                                       sunStart - scene.cameraPosition, rayDirection);
    }
    for (uint ray = 0u; ray < (sunLookup > 0.0 ? 2u : 1u); ++ray) {
        bool sky = ray == 0u;
        FarScatterSum unused = FarScatterSum(vec3(0.0), vec3(0.0), 0.0);
        float transmittance =
            farFieldMarch(farField, world, sky ? vec3(0.0, 1.0, 0.0) : rayDirection,
                          sky ? 0.0 : farVoxelSize(shadeLevel),
                          sky ? kTileSkyDistance : kTileSunMarch, shadeLevel, false, false,
                          unused);
        depths[ray] = -log(max(transmittance, exp(-kFarOpaqueDepth)));
    }
    float occlusion = mix(1.0, exp(-depths.x), scene.microShadowStrength);

    Shading sh = beginShading(s, relative);
    vec3 sunRadiance = vec3(0.0);
    if (sunUp) {
        float visibility = exp(-depths.y) * sunLookup;
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
