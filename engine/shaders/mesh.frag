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

// The sun's visibility at a surface point (traceSunVisibility). On terrain
// the normal is the smooth shading normal, elsewhere the triangle's.
float sunVisibility(vec3 position, vec3 normal) {
#ifdef TERRAIN
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
#else
    vec3 origin = position + normal * max(kShadowRayBias, inLodError);
#ifdef ALPHA_MASK
    if (inTerrainSun == 0.0) {
        return 0.0;
    }
    if (inTerrainSun > 0.0) {
        return traceSunVisibility(origin, origin, sampleSunDirection(pc.scene, gl_FragCoord.xy),
                                  0.0, false, inRelativePosition, normal);
    }
#endif
    // An object's base can sit below the bilinear heightfield (detail
    // displacement, coarse ridges), where the march would hit at once.
    vec3 marchOrigin = origin;
    marchOrigin.y = max(origin.y, terrainHeight(pc.scene.terrain, origin.xz) + kShadowRayBias);
#endif
    return traceSunVisibility(origin, marchOrigin, sampleSunDirection(pc.scene, gl_FragCoord.xy),
                              0.0, true, inRelativePosition, normal);
}

// ---------------------------------------------------------------- Surface

#define SURFACE_FRAGMENT
#include "surface.glsl"
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
#ifdef TERRAIN
    surfaceIn.tint = vec3(1.0);
    surfaceIn.height = 0.0;
    surfaceIn.streamFade = 1.0;
    surfaceIn.crownVisibility = 1.0;
#else
    surfaceIn.tint = inTint;
    surfaceIn.height = inHeight;
    surfaceIn.streamFade = inStreamFade;
    surfaceIn.crownVisibility = inCrownVisibility;
#endif
#ifdef TERRAIN
    vec3 normal = terrainNormal();
    Surface s = pc.scene.terrain.enabled != 0u ? terrainSurface(normal)
                                               : proceduralTerrain(normal);
#else
    Material material = pc.scene.materials.materials[inMaterialIndex];
    vec3 normal = normalize(inNormal);
    if (!gl_FrontFacing) {
        normal = -normal;
    }
    Surface s = materialSurface(material, normal);
    if (inStreamFade < 1.0) {
        s.normal = normalize(mix(terrainNormal(), s.normal, inStreamFade));
    }
    vec3 normalDx = dFdx(s.normal);
    vec3 normalDy = dFdy(s.normal);
    if (pc.scene.foliageSpecularAa != 0u && material.diffuseTransmission > 0.0) {
        s.perceptualRoughness = specularAaRoughness(
            s.perceptualRoughness, dot(normalDx, normalDx) + dot(normalDy, normalDy));
    }
#endif
    Shading sh = beginShading(s, inRelativePosition);
    vec2 debugStretch = vec2(0.0);
#if defined(DEBUG_ID) && !defined(TERRAIN)
    vec2 debugUvDx = dFdx(inUv);
    vec2 debugUvDy = dFdy(inUv);
    if (material.baseColorTexture != kNoTexture) {
        debugStretch = textureStretch(
            inUv, debugUvDx, debugUvDy, surfaceIn.dpdx, surfaceIn.dpdy,
            vec2(textureSize(textures[nonuniformEXT(material.baseColorTexture)], 0)));
    }
#endif
#ifndef TERRAIN
    bool leaf = material.diffuseTransmission > 0.0;
    vec3 transmitted = vec3(0.0);
    if (leaf) {
        vec3 transmissionColor = material.diffuseTransmissionColor * inTint;
        if (material.diffuseTransmissionColorTexture != kNoTexture) {
            transmissionColor *= sampleTexture(material.diffuseTransmissionColorTexture, inUv).rgb;
        }
        transmitted = transmissionColor * (material.diffuseTransmission * (1.0 - s.metallic));
        sh.diffuseColor *= 1.0 - material.diffuseTransmission;
    }
#endif

    // Offset shadow rays along the true triangle normal: the smooth shading
    // normal can point below the surface and make the ray hit its own
    // triangle. Derivatives must be taken outside the branch below.
    vec3 geometricNormal =
        normalize(cross(dFdx(inRelativePosition), dFdy(inRelativePosition)));
    if (dot(geometricNormal, normal) < 0.0) {
        geometricNormal = -geometricNormal;
    }

    vec3 sunRadiance = vec3(0.0);
#ifdef TERRAIN
    // A coarse LOD triangle can face away from the sun where the
    // heightfield (the shading normal) doesn't; its facing would cut the
    // light per triangle.
    bool facesSun = true;
#else
    // A fading plant's normal is blended toward the terrain's (up), which
    // turns off its back faces' transmission; let them take the sun.
    bool facesSun = dot(geometricNormal, sh.L) > 0.0 || inStreamFade < 1.0;
#endif
    if (sh.NoL > 0.0 && facesSun) {
#ifdef TERRAIN
        float visibility = sunVisibility(worldPosition, normal);
#else
        float visibility = sunVisibility(worldPosition, geometricNormal);
#endif
#ifdef TERRAIN
        float plantSun = 1.0;
#else
        float plantSun = mix(1.0, plantOcclusion(material), 0.5);
#endif
        addSunLight(sh, visibility,
                    plantSun *
                        mix(1.0, microShadow(sh.NoL, s.microOcclusion), pc.scene.microShadowStrength) *
                        s.sunShadow);
        sunRadiance = sh.diffuseLight + sh.specularLight;
    }
#ifndef TERRAIN
    // A thin surface's lit side is the geometric one; where the shading
    // normal disagrees, its Lambert lobes take |N.L| instead of nothing
    // (pbrt: lobe by the geometric normal, BSDF in the shading frame).
    // A fading plant's normal is blended up, toward the terrain's; it keeps
    // transmission only where both normals face away.
    if (leaf) {
        float NgoL = dot(geometricNormal, sh.L);
        float NoL = dot(sh.N, sh.L);
        if (NgoL < 0.0 && (NoL < 0.0 || inStreamFade >= 1.0)) {
            addTransmittedSun(sh, transmitted, min(abs(NoL), 1.0),
                              sunVisibility(worldPosition, -geometricNormal));
        } else if (NoL <= 0.0) {
            addTransmittedSun(sh, sh.diffuseColor, min(-NoL, 1.0),
                              sunVisibility(worldPosition, geometricNormal));
        }
    }
#endif

#ifndef TERRAIN
    s.occlusion *= crownOcclusion();
#endif
#ifdef DEBUG_ID
    vec3 debugSunDiffuse = sh.diffuseLight;
    vec3 debugSunSpecular = sh.specularLight;
#endif
    addAmbientLight(sh, s.occlusion);
#ifndef TERRAIN
    if (leaf) {
        addTransmittedAmbient(sh, transmitted, s.occlusion);
    }
#endif

#ifdef ALPHA_BLEND
    // Premultiplied alpha (blend: one, one-minus-source-alpha).
    float coverage = s.alpha;
    vec3 luminance = sh.diffuseLight * coverage + sh.specularLight;
#else
    float coverage = 1.0;
    vec3 luminance = sh.diffuseLight + sh.specularLight;
#endif

    float hazeTransmittance;
    luminance = applyAerialPerspective(luminance, inRelativePosition, coverage, hazeTransmittance);
    sunRadiance *= hazeTransmittance;

    // Emissive is already in physical units (cd/m^2), not relative to the sun.
    vec3 exposed = luminance * pc.scene.sunIlluminance + s.emissive;
    outColor = vec4(exposed * pc.scene.exposure.exposure, coverage);

    vec2 currentUv = inCurrentClip.xy / inCurrentClip.w * 0.5;
    vec2 previousUv = inPreviousClip.xy / inPreviousClip.w * 0.5;
    outMotion = previousUv - currentUv;
    writeMeshDebugId(inRelativePosition, inMaterialIndex);
#if defined(DEBUG_ID) && !defined(ALPHA_BLEND)
    vec3 debugScale = pc.scene.sunIlluminance * pc.scene.exposure.exposure * hazeTransmittance;
    beginInvocationInterlockARB();
    writeDebugLight(gl_FragCoord.z, debugSunDiffuse * debugScale, debugSunSpecular * debugScale,
                    (sh.diffuseLight - debugSunDiffuse) * debugScale,
                    (sh.specularLight - debugSunSpecular) * debugScale, debugStretch);
    endInvocationInterlockARB();
#endif
#ifdef ALPHA_BLEND
    outTransparency = coverage;
    outSunLight = vec4(0.0, 0.0, 0.0, coverage);
#else
    outDiffuseAlbedo = vec4(sh.diffuseColor, 1.0);
    outSpecularAlbedo = vec4(mix(sh.dfg.xxx, sh.dfg.yyy, sh.f0), 1.0);
    outNormalRoughness = vec4(sh.N, sh.perceptualRoughness);
    outSunLight = vec4(sunRadiance * pc.scene.sunIlluminance * pc.scene.exposure.exposure, 1.0);
#endif
#endif
}
