// Surface lighting shared by the scene's fragment shaders (mesh.frag and its
// variants, voxel.frag): sun shadow rays, the BRDF, sky ambient and aerial
// perspective. Include after common.glsl, atmosphere.glsl, material.glsl and
// terrain.glsl, with GL_EXT_ray_query enabled.
#ifndef LIGHTING_GLSL
#define LIGHTING_GLSL

#include "farfield.glsl"
#include "weather.glsl"
#include "clouds.glsl"
#include "gi.glsl"

// The pixel the froxel lookups (aerial perspective) take: the fragment's,
// or a compute shader's own (reference.comp defines it first).
#ifndef SHADING_PIXEL
#define SHADING_PIXEL gl_FragCoord.xy
#endif

const float kGroundAlbedo = 0.1;
const float kMinPerceptualRoughness = 0.045;

// ---------------------------------------------------------------- Shadows

// Only opaque and alpha-masked instances cast shadows (see buildTlas); the
// placed trees of far-field layers have their own bit. Must match
// gpu_types.h.
const uint kShadowCasterMask = 0x01u;
const uint kFarFieldCasterMask = 0x04u;
// Receivers farther from the camera than this see the far field's voxels
// instead of the trees in the TLAS or the cascades (when
// SceneData.voxelFarShadows is set): the far switch's distance for a 25 m
// tree at 1440p. One representation per ray, so no tree occludes twice.
const float kFarShadowDistance = 4000.0;

// Alpha-masked geometry is non-opaque in the acceleration structure, so the
// ray query reports each candidate hit; we accept it only where the base
// color alpha times the vertex alpha (COLOR_0) passes the material's cutoff.
bool alphaTestCandidate(rayQueryEXT query) {
    MeshInfo mesh = pc.scene.meshes.meshes[instanceMesh(
        pc.scene, rayQueryGetIntersectionInstanceCustomIndexEXT(query, false),
        rayQueryGetIntersectionGeometryIndexEXT(query, false))];
    uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
    vec2 barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, false);

    uint first = mesh.firstIndex + primitive * 3u;
    Vertex v0 = loadVertex(pc.scene, mesh, pc.scene.indices.indices[first] + mesh.vertexOffset);
    Vertex v1 = loadVertex(pc.scene, mesh, pc.scene.indices.indices[first + 1u] + mesh.vertexOffset);
    Vertex v2 = loadVertex(pc.scene, mesh, pc.scene.indices.indices[first + 2u] + mesh.vertexOffset);
    vec3 w = vec3(1.0 - barycentrics.x - barycentrics.y, barycentrics);
    vec2 uv = w.x * vec2(v0.uvX, v0.uvY) + w.y * vec2(v1.uvX, v1.uvY) +
              w.z * vec2(v2.uvX, v2.uvY);

    Material m = pc.scene.materials.materials[mesh.materialIndex];
    float alpha = m.baseColorFactor.a * (w.x * v0.alpha + w.y * v1.alpha + w.z * v2.alpha);
    if (m.baseColorTexture != kNoTexture) {
        alpha *= textureLod(textures[nonuniformEXT(m.baseColorTexture)], uv, 0.0).a;
    }
    return alpha >= m.alphaCutoff;
}

// Sun visibility of a far receiver from the far shadow map: a bilinear 2x2
// depth compare. The map holds the distance along the sun of the first
// 0.5-opacity point of the bricks, so a receiver deeper than that is
// shadowed. On a surface facing the sun, neighbouring texels differ in depth
// by about texel * cot(elevation), so the receiver moves one texel off its
// surface along the normal and takes a depth bias that grows with the
// surface's slope to the sun (clamped). Texels outside the map are lit.
const float kFarShadowBias = 2.0;
const float kFarShadowMaxSlope = 4.0;

float farShadowVisibility(vec3 world, vec3 normal, float start) {
    SceneBuffer scene = pc.scene;
    float NoL = clamp(dot(normal, scene.sunDirection), 0.05, 1.0);
    float slope = min(sqrt(1.0 - NoL * NoL) / NoL, kFarShadowMaxSlope);
    float bias = start + kFarShadowBias + scene.farShadowTexel * slope;
    world += normal * scene.farShadowTexel;
    vec3 u = vec3(scene.farShadowU[0], scene.farShadowU[1], scene.farShadowU[2]);
    vec3 v = vec3(scene.farShadowV[0], scene.farShadowV[1], scene.farShadowV[2]);
    vec3 d = vec3(scene.farShadowD[0], scene.farShadowD[1], scene.farShadowD[2]);
    vec2 position = (vec2(dot(world, u), dot(world, v)) -
                     vec2(scene.farShadowMinU, scene.farShadowMinV)) / scene.farShadowTexel - 0.5;
    float receiver = dot(world, d) - scene.farShadowMinD - bias;
    ivec2 base = ivec2(floor(position));
    vec2 weight = position - vec2(base);
    float lit = 0.0;
    for (int i = 0; i < 4; ++i) {
        ivec2 texel = base + ivec2(i & 1, i >> 1);
        float visible = 1.0;
        if (all(greaterThanEqual(texel, ivec2(0))) &&
            all(lessThan(texel, ivec2(scene.farShadowWidth, scene.farShadowHeight)))) {
            uint index = uint(texel.y) * scene.farShadowWidth + uint(texel.x);
            uint word = scene.farShadowTexels.words[index >> 1u];
            uint q = (word >> ((index & 1u) * 16u)) & 0xFFFFu;
            visible = q >= kFarShadowStepCap ? 1.0
                      : receiver <= float(q) * (1.0 / 65534.0) * scene.farShadowDepth ? 1.0
                                                                                      : 0.0;
        }
        lit += visible * ((i & 1) != 0 ? weight.x : 1.0 - weight.x) *
               ((i >> 1) != 0 ? weight.y : 1.0 - weight.y);
    }
    return lit;
}

// Raster shadow cascades: the normal offset (in texels) fights acne on
// receivers facing away from the sun, the depth bias (in texels of depth)
// covers the filter footprint's slope; both scale with the cascade's texel.
const float kShadowNormalOffset = 1.5;
const float kShadowDepthBias = 1.0;
const float kCascadeBlendFraction = 0.15;

// Receiver in cascade i's light space: xy = map uv, z = receiver depth. The
// cascade's fields are read in place: copying the struct to a local costs
// ~1.8 ms per frame in mesh.frag.
bool cascadeCoords(uint i, vec3 relative, vec3 normal, out vec3 coords) {
    float texel = pc.scene.sunShadows.cascades[i].texel;
    float halfSize = pc.scene.sunShadows.cascades[i].halfSize;
    vec3 p = relative + normal * (kShadowNormalOffset * texel) -
             pc.scene.sunShadows.cascades[i].origin;
    vec2 lightUv = vec2(dot(p, pc.scene.sunShadows.u), dot(p, pc.scene.sunShadows.v));
    float depthNear = pc.scene.sunShadows.cascades[i].depthNear;
    float depthFar = pc.scene.sunShadows.cascades[i].depthFar;
    coords = vec3(lightUv / halfSize * 0.5 + 0.5,
                  (depthFar - dot(p, pc.scene.sunShadows.d)) / (depthNear + depthFar));
    return all(lessThanEqual(abs(lightUv), vec2(halfSize - 2.0 * texel)));
}

// The cascades' table entries carry a depth-compare sampler, so a tap is a
// hardware bilinear 2x2 compare.
layout(set = 1, binding = 1) uniform sampler2DShadow shadowMaps[];

// 3x3 tent from 4 bilinear compare taps (Castano 2013, The Witness; the
// weights are the tent's integrals over each tap's footprint).
float cascadeFiltered(uint i, vec3 coords) {
    uint map = pc.scene.sunShadows.cascades[i].texture;
    float texel = pc.scene.sunShadows.cascades[i].texel;
    float depthRange = pc.scene.sunShadows.cascades[i].depthNear +
                       pc.scene.sunShadows.cascades[i].depthFar;
    vec2 size = vec2(textureSize(shadowMaps[nonuniformEXT(map)], 0));
    float receiverDepth = coords.z + kShadowDepthBias * texel / depthRange;
    vec2 st = coords.xy * size;
    vec2 base = floor(st + 0.5);
    vec2 f = st + 0.5 - base;
    base = (base - 0.5) / size;
    vec2 w0 = 3.0 - 2.0 * f;
    vec2 w1 = 1.0 + 2.0 * f;
    vec2 o0 = ((2.0 - f) / w0 - 1.0) / size;
    vec2 o1 = (f / w1 + 1.0) / size;
    float sum = w0.x * w0.y * texture(shadowMaps[nonuniformEXT(map)], vec3(base + vec2(o0.x, o0.y), receiverDepth)) +
                w1.x * w0.y * texture(shadowMaps[nonuniformEXT(map)], vec3(base + vec2(o1.x, o0.y), receiverDepth)) +
                w0.x * w1.y * texture(shadowMaps[nonuniformEXT(map)], vec3(base + vec2(o0.x, o1.y), receiverDepth)) +
                w1.x * w1.y * texture(shadowMaps[nonuniformEXT(map)], vec3(base + vec2(o1.x, o1.y), receiverDepth));
    return sum / 16.0;
}

// Sun visibility from the raster cascades for a receiver at relative (to the
// camera) with geometric normal; 1 where no cascade covers it.
float cascadeVisibility(vec3 relative, vec3 normal) {
    float distanceToCamera = length(relative);
    for (uint i = 0u; i < kShadowCascades; ++i) {
        float radius = pc.scene.sunShadows.cascades[i].radius;
        vec3 coords;
        if (pc.scene.sunShadows.cascades[i].valid == 0u || distanceToCamera >= radius ||
            !cascadeCoords(i, relative, normal, coords)) {
            continue;
        }
        float visibility = cascadeFiltered(i, coords);
        float blend = smoothstep(radius * (1.0 - kCascadeBlendFraction), radius,
                                 distanceToCamera);
        if (blend > 0.0 && i + 1u < kShadowCascades) {
            vec3 nextCoords;
            if (pc.scene.sunShadows.cascades[i + 1u].valid != 0u &&
                cascadeCoords(i + 1u, relative, normal, nextCoords)) {
                visibility = mix(visibility, cascadeFiltered(i + 1u, nextCoords), blend);
            }
        }
        return visibility;
    }
    return 1.0;
}

// Traces one ray toward the sun: the fraction of sunlight that arrives. 0 if
// an object (the TLAS, from origin + start direction) or the terrain (its
// heightfield, marched from marchOrigin) blocks it. The TLAS goes first:
// inside canopies it hits within a few meters, where the terrain march would
// walk the whole pyramid first. A receiver beyond kFarShadowDistance traces
// the TLAS without the far-field trees, then the far field's transmittance
// from the same start. Without marchTerrain the caller accounts for the
// terrain itself (cluster_cull.comp's per-cluster visibility).
// In raster shadow mode (shadowMode 0) objects come from the cascades at
// relative (camera-relative position) and normal instead of the ray query,
// and beyond kFarShadowDistance from nothing but the far field: the other
// casters (rocks, saplings) aren't drawn that far, and without a ray-traced
// mode there is no TLAS.
float traceSunVisibility(vec3 origin, vec3 marchOrigin, vec3 direction, float start,
                         bool marchTerrain, vec3 relative, vec3 normal) {
    bool far = pc.scene.voxelFarShadows != 0u &&
               distance(origin, pc.scene.cameraPosition) > kFarShadowDistance;
    float objects = 1.0;
    if (pc.scene.shadowMode == 0u) {
        if (!far) {
            objects = cascadeVisibility(relative, normal);
            if (objects <= 0.0) {
                return 0.0;
            }
        }
    } else {
        rayQueryEXT query;
        rayQueryInitializeEXT(query, accelerationStructureEXT(pc.scene.tlas),
                              gl_RayFlagsTerminateOnFirstHitEXT,
                              far ? kShadowCasterMask : kShadowCasterMask | kFarFieldCasterMask,
                              origin, start, direction, kShadowRayLength);
        while (rayQueryProceedEXT(query)) {
            if (rayQueryGetIntersectionTypeEXT(query, false) ==
                    gl_RayQueryCandidateIntersectionTriangleEXT &&
                alphaTestCandidate(query)) {
                rayQueryConfirmIntersectionEXT(query);
            }
        }
        if (rayQueryGetIntersectionTypeEXT(query, true) !=
            gl_RayQueryCommittedIntersectionNoneEXT) {
            return 0.0;
        }
    }
    if (marchTerrain) {
        float terrain = terrainShadowVisibility(pc.scene, marchOrigin);
        if (terrain < 0.0) {
            terrain = terrainOccludes(pc.scene, marchOrigin, direction, kShadowRayLength) ? 0.0
                                                                                        : 1.0;
        }
        if (terrain <= 0.0) {
            return 0.0;
        }
        objects *= terrain;
    }
    if (!far) {
        return objects;
    }
    if (pc.scene.farShadowMode != 0u) {
        return objects * farShadowVisibility(origin, normal, start);
    }
    FarFieldBuffer farField = pc.scene.farField;
    return objects *
           farFieldTransmittance(farField, origin, direction, start, kShadowRayLength, false);
}

// ---------------------------------------------------------------- Surface

struct Surface {
    vec3 baseColor;
    float alpha;
    float metallic;
    float perceptualRoughness;
    float occlusion;
    // AO that drives the micro shadow (see terrainMicroOcclusion).
    float microOcclusion;
    vec3 emissive;
    vec3 normal;
    // Parallax self-shadowing of the sun (1 = lit).
    float sunShadow;
};

// ---------------------------------------------------------------- BRDF
// Filament's standard model (google/filament, shaders/src/surface_brdf.fs):
// GGX distribution, height-correlated Smith visibility, Schlick Fresnel,
// Lambert diffuse, with multiple-scattering energy compensation.

float distributionGgx(float NoH, float a) {
    float a2 = a * a;
    float f = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (kPi * f * f);
}

float visibilitySmithGgxCorrelated(float NoV, float NoL, float a) {
    float a2 = a * a;
    float ggxL = NoV * sqrt((NoL - NoL * a2) * NoL + a2);
    float ggxV = NoL * sqrt((NoV - NoV * a2) * NoV + a2);
    return 0.5 / (ggxV + ggxL);
}

vec3 fresnelSchlick(vec3 f0, float VoH) {
    float f = pow(1.0 - VoH, 5.0);
    return f + f0 * (1.0 - f);
}

// ---------------------------------------------------------------- Ambient occlusion

// Material AO is single-bounce visibility; light also reaches a cavity
// after bouncing inside it, more so the brighter its walls. Jimenez et al.
// 2016 fit that multi-bounce visibility as a cubic in AO per albedo.
vec3 multiBounceOcclusion(float visibility, vec3 albedo) {
    vec3 a = 2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c = 2.7552 * albedo + 0.6903;
    return max(vec3(visibility), ((visibility * a + b) * visibility + c) * visibility);
}

// Micro shadows: the AO says how much of the hemisphere the cavity leaves
// open; a cone of that aperture around the normal hides the sun once N.L
// drops below it. Chan, "Material Advances in Call of Duty: WWII" (2018),
// as in Filament.
float microShadow(float NoL, float visibility) {
    float aperture = inversesqrt(1.0 - min(visibility, 0.9999));
    float shadow = clamp(NoL * aperture, 0.0, 1.0);
    return shadow * shadow;
}

// Diffuse AO applied to a reflection lobe (Lagarde and de Rousiers 2014):
// a narrow lobe seen head-on escapes most cavities; at grazing angles or
// with rough lobes it approaches the diffuse AO.
float specularAmbientOcclusion(float NoV, float visibility, float roughness) {
    return clamp(pow(NoV + visibility, exp2(-16.0 * roughness - 1.0)) - 1.0 + visibility,
                 0.0, 1.0);
}

// ---------------------------------------------------------------- Shading
// In three steps, so the caller decides whether to trace the sun's shadow
// ray: beginShading, addSunLight (only where NoL > 0 and the surface faces
// the sun), addAmbientLight. All lighting is relative to a sun illuminance
// of 1 (as in the atmosphere LUTs) and scaled by the real sun at the end.

struct Shading {
    vec3 N;
    vec3 V;
    vec3 L;
    float NoV;
    float NoL;
    float perceptualRoughness;
    float a;
    vec3 f0;
    vec3 diffuseColor;
    vec2 dfg;
    vec3 energyCompensation;
    // The atmosphere's position of the displayed point and the sun's light
    // reaching it.
    vec3 position;
    vec3 sunAtSurface;
    // The flat world position (GI probes).
    vec3 worldPosition;
    // Diffuse and specular are kept apart: a blended surface (glass) fades
    // its diffuse with alpha but keeps its full reflection.
    vec3 diffuseLight;
    vec3 specularLight;
    // addAmbientLight's probe sample (GI on), reused by addTransmittedAmbient.
    GiSample gi;
};

// `relativePosition` is the flat world's, relative to the camera.
Shading beginShading(Surface s, vec3 relativePosition) {
    Shading sh;
    sh.N = s.normal;
    sh.V = normalize(-relativePosition);
    sh.L = pc.scene.sunDirection;
    sh.NoV = max(dot(sh.N, sh.V), 1e-4);

    sh.perceptualRoughness = clamp(s.perceptualRoughness, kMinPerceptualRoughness, 1.0);
    sh.a = sh.perceptualRoughness * sh.perceptualRoughness;
    // Dielectrics reflect ~4% at normal incidence; metals tint reflection.
    sh.f0 = mix(vec3(0.04), s.baseColor, s.metallic);
    sh.diffuseColor = s.baseColor * (1.0 - s.metallic);

    sh.dfg = textureLod(dfgLut, vec2(sh.NoV, sh.perceptualRoughness), 0.0).xy;
    sh.energyCompensation = 1.0 + sh.f0 * (1.0 / max(sh.dfg.y, 1e-4) - 1.0);

    // The atmosphere is a sphere: light the point where it is displayed,
    // not the flat point d^2 / 2R above it (shadow rays stay flat).
    sh.position = atmospherePosition(
        pc.scene.cameraPosition + displayedPosition(pc.scene, relativePosition).xyz);
    float height = length(sh.position);
    vec3 up = sh.position / height;
    sh.worldPosition = pc.scene.cameraPosition + relativePosition;
    sh.sunAtSurface = transmittanceToSun(height, dot(up, sh.L)) *
                      cloudShadow(relativePosition + pc.scene.cameraPosition);

    sh.diffuseLight = vec3(0.0);
    sh.specularLight = vec3(0.0);
    sh.NoL = clamp(dot(sh.N, sh.L), 0.0, 1.0);
    return sh;
}

// `surfaceShadow`: the surface's own sun occlusion on top of the ray's
// `visibility` (micro shadow, parallax self-shadow); 1 where it has none.
void addSunLight(inout Shading sh, float visibility, float surfaceShadow) {
    vec3 H = normalize(sh.V + sh.L);
    float NoH = clamp(dot(sh.N, H), 0.0, 1.0);
    float LoH = clamp(dot(sh.L, H), 0.0, 1.0);

    vec3 specular = distributionGgx(NoH, sh.a) *
                    visibilitySmithGgxCorrelated(sh.NoV, sh.NoL, sh.a) *
                    fresnelSchlick(sh.f0, LoH) * sh.energyCompensation;
    vec3 sunLight = sh.sunAtSurface * (sh.NoL * visibility * surfaceShadow);
    sh.diffuseLight = sh.diffuseColor / kPi * sunLight;
    sh.specularLight = specular * sunLight;
}

// Ambient from the sky (clouds included). Diffuse: sky irradiance from
// above, light bounced off the ground from below. Specular: the sky in the
// reflected direction with the sky capture's clouds over it, blurred toward
// the irradiance as roughness grows (until reflections are ray traced).
void addAmbientLight(inout Shading sh, float occlusion) {
    vec3 skyIrradiance = pc.scene.skyIrradiance.irradiance;
    vec3 groundIrradiance =
        kGroundAlbedo * (sh.sunAtSurface * max(sh.L.y, 0.0) + skyIrradiance);
    vec3 diffuseIrradiance = mix(groundIrradiance, skyIrradiance, sh.N.y * 0.5 + 0.5);
    vec3 R = reflect(-sh.V, sh.N);
    vec3 roughIrradiance = skyIrradiance;
    if (pc.scene.giMode != 0u) {
        sh.gi = giSample(sh.worldPosition, sh.N);
        diffuseIrradiance = giIrradiance(sh.gi, sh.N, diffuseIrradiance);
        roughIrradiance = giIrradiance(sh.gi, R, skyIrradiance);
    }
    vec4 cloudsInR = skyCaptureAt(R);
    // Where the probes see less than the open sky and ground would along R
    // (under a canopy), the sharp sky reflection is occluded as much.
    float skyOcclusion = 1.0;
    if (pc.scene.giMode != 0u) {
        vec3 open = mix(groundIrradiance, skyIrradiance, R.y * 0.5 + 0.5);
        const vec3 kLuminance = vec3(0.2126, 0.7152, 0.0722);
        skyOcclusion = clamp(dot(roughIrradiance, kLuminance) / max(dot(open, kLuminance), 1e-6),
                             0.0, 1.0);
    }
    vec3 skyRadiance = mix((skyLuminance(sh.position, R) * cloudsInR.a + cloudsInR.rgb) * skyOcclusion,
                           roughIrradiance / kPi, sh.a);
    vec3 specularReflectance = mix(sh.dfg.xxx, sh.dfg.yyy, sh.f0) * sh.energyCompensation;
    float specularOcclusion = specularAmbientOcclusion(sh.NoV, occlusion, sh.a);
    sh.diffuseLight += sh.diffuseColor / kPi * diffuseIrradiance *
                       multiBounceOcclusion(occlusion, sh.diffuseColor);
    sh.specularLight += specularReflectance * skyRadiance *
                        multiBounceOcclusion(specularOcclusion, sh.f0);
}

// Thin-surface diffuse transmission (KHR_materials_diffuse_transmission):
// light through the back face, `transmitted` = factor x transmission colour.
// The caller scales the front's diffuse colour by (1 - factor).
void addTransmittedSun(inout Shading sh, vec3 transmitted, float backNoL, float visibility) {
    sh.diffuseLight += transmitted / kPi * sh.sunAtSurface * (backNoL * visibility);
}

// After addAmbientLight: a thin surface's two sides read the same probes, so
// its sample serves the back direction too.
void addTransmittedAmbient(inout Shading sh, vec3 transmitted, float occlusion) {
    vec3 skyIrradiance = pc.scene.skyIrradiance.irradiance;
    vec3 groundIrradiance =
        kGroundAlbedo * (sh.sunAtSurface * max(sh.L.y, 0.0) + skyIrradiance);
    vec3 backIrradiance = mix(groundIrradiance, skyIrradiance, -sh.N.y * 0.5 + 0.5);
    if (pc.scene.giMode != 0u) {
        backIrradiance = giIrradiance(sh.gi, -sh.N, backIrradiance);
    }
    sh.diffuseLight += transmitted / kPi * backIrradiance *
                       multiBounceOcclusion(occlusion, sh.diffuseColor);
}

// Aerial perspective: haze between the camera and the point as displayed
// (`relativePosition` is the flat world's). `transmittance` is what the haze
// lets through of the light from the point.
vec3 applyAerialPerspective(vec3 luminance, vec3 relativePosition, float coverage,
                            out float transmittance) {
    float distanceKm = length(displayedPosition(pc.scene, relativePosition).xyz) * 0.001;
    float slice = distanceKm / kAerialPerspectiveKmPerSlice;
    float weight = 1.0;
    if (slice < 0.5) {
        // Fade in over the first half slice instead of jumping.
        weight = clamp(slice * 2.0, 0.0, 1.0);
        slice = 0.5;
    }
    float w = sqrt(slice / kAerialPerspectiveSlices);
    vec4 haze = textureLod(aerialPerspectiveLut,
                           vec3(SHADING_PIXEL * pc.scene.inverseResolution, w), 0.0);
    transmittance = mix(1.0, haze.a, weight);
    return luminance * transmittance + haze.rgb * (weight * coverage);
}

#endif
