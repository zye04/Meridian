#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#ifdef DEBUG_ID
#extension GL_ARB_fragment_shader_interlock : require
layout(pixel_interlock_unordered) in;
#if !defined(ALPHA_MASK) && !defined(ALPHA_BLEND) && !defined(DEPTH_ONLY)
// The debug stores would otherwise move the depth test after shading (the
// masked variant declares it below); solo/hide is in the mesh shaders.
layout(early_fragment_tests) in;
#endif
#endif
#include "common.glsl"
#include "atmosphere.glsl"
#include "material.glsl"
#include "terrain.glsl"
#include "lighting.glsl"
#define DEBUG_ID_MESH
#include "debug_id.glsl"
#ifdef TERRAIN
#include "placement.glsl"
#endif

layout(location = 0) in vec3 inRelativePosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inCurrentClip;
layout(location = 3) in vec4 inPreviousClip;
layout(location = 4) in vec2 inUv;
layout(location = 5) in vec4 inTangent;
layout(location = 6) flat in uint inMaterialIndex;
#ifndef TERRAIN
// How far (m) this LOD cluster may deviate from the full-detail surface the
// shadow rays are traced against; 0 at full detail.
layout(location = 7) flat in float inLodError;
// Height within the mesh, 0 at its base to 1 at its top (1 where unknown).
layout(location = 8) in float inHeight;
// Placed instances' prototype tint (Prototype::tint), 1 otherwise.
layout(location = 9) flat in vec3 inTint;
// A streamed plant's shrink factor (1 = full size): it shades like the
// terrain it turns into as this goes to 0.
layout(location = 10) flat in float inStreamFade;
// Sky visibility inside a tree crown (mesh.mesh), 1 elsewhere.
layout(location = 12) in float inCrownVisibility;
#endif
#ifdef ALPHA_MASK
// The cluster's terrain sun visibility (cluster_cull.comp), or -1 where the
// terrain is marched here per fragment.
layout(location = 11) flat in float inTerrainSun;
#endif
#if defined(ALPHA_MASK) && defined(DEPTH_ONLY)
layout(location = 14) in float inVertexAlpha;
#endif

#ifdef ALPHA_MASK
#ifndef DEPTH_ONLY
layout(early_fragment_tests) in;
#endif
#endif

layout(location = 0) out vec4 outColor;
// Screen-space motion in UV units, pointing from this frame's position to
// last frame's (the convention DLSS also uses).
layout(location = 1) out vec2 outMotion;
#ifdef ALPHA_BLEND
// Transparency mask for the upscaler: blended coverage, blended with MAX.
layout(location = 2) out float outTransparency;
#else
// Ray Reconstruction guides: diffuse albedo, specular albedo (the DFG
// pre-integrated reflectance), world normal and perceptual roughness.
layout(location = 3) out vec4 outDiffuseAlbedo;
layout(location = 4) out vec4 outSpecularAlbedo;
layout(location = 5) out vec4 outNormalRoughness;
#endif
// The sun's exposed radiance in outColor (after haze, pre-exposed): the
// contact shadow pass removes the occluded part of it. A blended surface
// scales what is behind it by its transmittance (alpha) and adds none of
// its own, so glass never darkens.
layout(location = 6) out vec4 outSunLight;

// inRelativePosition + cameraPosition, set first thing in main(); both are
// in the flat world, before the curvature warp (common.glsl). World
// positions place the shadow rays; derivatives, distances and terrain
// texture coordinates use the relative position, which keeps its precision
// far from the origin.
vec3 worldPosition;

// ---------------------------------------------------------------- Shadows

#ifdef TERRAIN
// The sun's visibility at a terrain point (traceSunVisibility), by the
// smooth shading normal.
float sunVisibility(vec3 position, vec3 normal) {
    // Start on the heightfield the march tests, not on the raster: coarse
    // triangles can lie below it by more than their estimated error, and
    // can face away from the sun.
    Terrain terrain = pc.scene.terrain;
    float height = terrainHeight(terrain, position.xz);
    vec3 origin = position;
    origin.y = max(origin.y, height) + kShadowRayBias;
    // Where the smooth normal faces the sun but the bilinear cell doesn't,
    // the march would shadow the cell by itself, in jagged bands along the
    // terminator: start above the cell's corners over the shading plane
    // (Chiang et al. 2019).
    vec3 marchOrigin = origin;
    marchOrigin.y = max(origin.y, height + kShadowRayBias +
                                      terrainCellAbovePlane(terrain, position.xz, height,
                                                            normal));
    return traceSunVisibility(origin, marchOrigin, sampleSunDirection(pc.scene, gl_FragCoord.xy),
                              0.0, true, inRelativePosition, normal);
}
#endif

// ---------------------------------------------------------------- Surface

#define SURFACE_FRAGMENT
#include "surface.glsl"
#ifndef TERRAIN
#define CLUSTER_SHADING_SCREEN_DERIVATIVES
#include "cluster_shading.glsl"
#endif
void main() {
#ifdef DEPTH_ONLY
    Material cutoutMaterial = pc.scene.materials.materials[inMaterialIndex];
    float cutoutAlpha = cutoutMaterial.baseColorFactor.a * inVertexAlpha;
    if (cutoutMaterial.baseColorTexture != kNoTexture) {
        cutoutAlpha *= texture(textures[nonuniformEXT(cutoutMaterial.baseColorTexture)], inUv,
                               pc.scene.mipBias + pc.scene.cutoutMipBias).a;
    }
    if (cutoutAlpha < cutoutMaterial.alphaCutoff) {
        discard;
    }
    return;
#else
#ifdef TERRAIN
    worldPosition = inRelativePosition + pc.scene.cameraPosition;
    surfaceIn.relativePosition = inRelativePosition;
    surfaceIn.worldPosition = worldPosition;
    surfaceIn.dpdx = dFdx(inRelativePosition);
    surfaceIn.dpdy = dFdy(inRelativePosition);
    surfaceIn.uv = inUv;
    surfaceIn.uvDx = dFdxCoarse(inUv);
    surfaceIn.uvDy = dFdyCoarse(inUv);
    vec2 terrainUv = terrainNormalUv(worldPosition);
    surfaceIn.terrainUvDx = dFdxCoarse(terrainUv);
    surfaceIn.terrainUvDy = dFdyCoarse(terrainUv);
    surfaceIn.tangent = inTangent;
    surfaceIn.tint = vec3(1.0);
    surfaceIn.height = 0.0;
    surfaceIn.streamFade = 1.0;
    surfaceIn.crownVisibility = 1.0;
    vec3 normal = terrainNormal();
    Surface s = pc.scene.terrain.enabled != 0u ? terrainSurface(normal)
                                               : proceduralTerrain(normal);
    Shading sh = beginShading(s, inRelativePosition);

    vec3 sunRadiance = vec3(0.0);
    // A coarse LOD triangle can face away from the sun where the
    // heightfield (the shading normal) doesn't; its facing would cut the
    // light per triangle.
    if (sh.NoL > 0.0) {
        float visibility = sunVisibility(worldPosition, normal);
        addSunLight(sh, visibility,
                    mix(1.0, microShadow(sh.NoL, s.microOcclusion), pc.scene.microShadowStrength) *
                        s.sunShadow);
        sunRadiance = sh.diffuseLight + sh.specularLight;
    }
#ifdef DEBUG_ID
    vec3 debugSunDiffuse = sh.diffuseLight;
    vec3 debugSunSpecular = sh.specularLight;
#endif
    addAmbientLight(sh, s.occlusion);
    vec3 luminance = sh.diffuseLight + sh.specularLight;
    float hazeTransmittance;
    luminance = applyAerialPerspective(luminance, inRelativePosition, 1.0, hazeTransmittance);
    sunRadiance *= hazeTransmittance;
    vec3 exposed = luminance * pc.scene.sunIlluminance + s.emissive;
    outColor = vec4(exposed * pc.scene.exposure.exposure, 1.0);
    vec2 currentUv = inCurrentClip.xy / inCurrentClip.w * 0.5;
    vec2 previousUv = inPreviousClip.xy / inPreviousClip.w * 0.5;
    outMotion = previousUv - currentUv;
    writeMeshDebugId(inRelativePosition, inMaterialIndex);
#ifdef DEBUG_ID
    vec3 debugScale = pc.scene.sunIlluminance * pc.scene.exposure.exposure * hazeTransmittance;
    beginInvocationInterlockARB();
    writeDebugLight(gl_FragCoord.z, debugSunDiffuse * debugScale, debugSunSpecular * debugScale,
                    (sh.diffuseLight - debugSunDiffuse) * debugScale,
                    (sh.specularLight - debugSunSpecular) * debugScale, vec2(0.0));
    endInvocationInterlockARB();
#endif
    outDiffuseAlbedo = vec4(sh.diffuseColor, 1.0);
    outSpecularAlbedo = vec4(mix(sh.dfg.xxx, sh.dfg.yyy, sh.f0), 1.0);
    outNormalRoughness = vec4(sh.N, sh.perceptualRoughness);
    outSunLight = vec4(sunRadiance * pc.scene.sunIlluminance * pc.scene.exposure.exposure, 1.0);
#else
    ClusterPixel p;
    p.relative = inRelativePosition;
    p.relativeDx = dFdx(inRelativePosition);
    p.relativeDy = dFdy(inRelativePosition);
    p.uvDx = dFdxCoarse(inUv);
    p.uvDy = dFdyCoarse(inUv);
    vec2 terrainUv = terrainNormalUv(inRelativePosition + pc.scene.cameraPosition);
    p.terrainUvDx = dFdxCoarse(terrainUv);
    p.terrainUvDy = dFdyCoarse(terrainUv);
    p.normal = inNormal;
    p.geometricNormal = cross(p.relativeDx, p.relativeDy);
    p.frontFacing = gl_FrontFacing;
    p.uv = inUv;
    p.tangent = inTangent;
    p.materialIndex = inMaterialIndex;
    p.lodError = inLodError;
    p.height = inHeight;
    p.tint = inTint;
    p.streamFade = inStreamFade;
    p.crownVisibility = inCrownVisibility;
#ifdef ALPHA_MASK
    p.terrainSun = inTerrainSun;
#else
    p.terrainSun = -1.0;
#endif
    p.currentClip = inCurrentClip;
    p.previousClip = inPreviousClip;
    p.normalDx = vec3(0.0);
    p.normalDy = vec3(0.0);
#ifdef DEBUG_ID
    vec2 debugStretch = vec2(0.0);
    Material debugMaterial = pc.scene.materials.materials[inMaterialIndex];
    if (debugMaterial.baseColorTexture != kNoTexture) {
        debugStretch = textureStretch(
            inUv, dFdx(inUv), dFdy(inUv), p.relativeDx, p.relativeDy,
            vec2(textureSize(textures[nonuniformEXT(debugMaterial.baseColorTexture)], 0)));
    }
#endif
    SceneOutputs o = shadeClusterPixel(p);
    outColor = o.color;
    outMotion = o.motion;
    writeMeshDebugId(inRelativePosition, inMaterialIndex);
#if defined(DEBUG_ID) && !defined(ALPHA_BLEND)
    beginInvocationInterlockARB();
    writeDebugLight(gl_FragCoord.z, o.debugSunDiffuse, o.debugSunSpecular, o.debugAmbientDiffuse,
                    o.debugAmbientSpecular, debugStretch);
    endInvocationInterlockARB();
#endif
#ifdef ALPHA_BLEND
    outTransparency = o.transparency;
#else
    outDiffuseAlbedo = o.diffuseAlbedo;
    outSpecularAlbedo = o.specularAlbedo;
    outNormalRoughness = o.normalRoughness;
#endif
    outSunLight = o.sunLight;
#endif
#endif
}
