// Surface lighting shared by the scene's fragment shaders (mesh.frag and its
// variants, voxel.frag): sun shadow rays, the BRDF, sky ambient and aerial
// perspective. Include after common.glsl, atmosphere.glsl, material.glsl and
// terrain.glsl, with GL_EXT_ray_query enabled.
#ifndef LIGHTING_GLSL
#define LIGHTING_GLSL

#include "farfield.glsl"

const float kShadowRayBias = 0.05;
const float kShadowRayLength = 20000.0;
const float kGroundAlbedo = 0.1;
const float kMinPerceptualRoughness = 0.045;

// ---------------------------------------------------------------- Shadows

// Interleaved gradient noise (Jimenez 2014), shifted every frame so the
// temporal resolve averages successive samples.
float interleavedGradientNoise(vec2 pixel, uint frame) {
    pixel += 5.588238 * float(frame % 64u);
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// Picks a direction inside the sun's disk. One ray per pixel per frame;
// accumulated over frames this gives penumbrae that widen with distance from
// the occluder, like real sun shadows.
vec3 sampleSunDirection(vec2 pixel) {
    vec3 sun = pc.scene.sunDirection;
    vec3 tangent = normalize(cross(abs(sun.y) < 0.99 ? vec3(0, 1, 0)
                                                      : vec3(1, 0, 0), sun));
    vec3 bitangent = cross(sun, tangent);

    float radius = tan(pc.scene.sunAngularRadius) *
                   sqrt(interleavedGradientNoise(pixel, pc.scene.frameIndex));
    float angle = 2.0 * kPi *
                  interleavedGradientNoise(pixel + vec2(17.0, 31.0),
                                           pc.scene.frameIndex);
    return normalize(sun + radius * (cos(angle) * tangent +
                                     sin(angle) * bitangent));
}

// Only opaque and alpha-masked instances cast shadows (see buildTlas); the
// placed trees of far-field layers have their own bit. Must match
// gpu_types.h.
const uint kShadowCasterMask = 0x01u;
const uint kFarFieldCasterMask = 0x04u;
// Receivers farther from the camera than this see the far field's voxels
// instead of the trees in the TLAS (when SceneData.voxelFarShadows is set):
// the far switch's distance for a 25 m tree at 1440p. One representation
// per ray, so no tree occludes twice.
const float kFarShadowDistance = 4000.0;

// Alpha-masked geometry is non-opaque in the acceleration structure, so the
// ray query reports each candidate hit; we accept it only where the base
// color alpha passes the material's cutoff.
bool alphaTestCandidate(rayQueryEXT query) {
    MeshInfo mesh = pc.scene.meshes.meshes[instanceMesh(
        pc.scene, rayQueryGetIntersectionInstanceCustomIndexEXT(query, false),
        rayQueryGetIntersectionGeometryIndexEXT(query, false))];
    uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
    vec2 barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, false);

    uint first = mesh.firstIndex + primitive * 3u;
    Vertex v0 = pc.scene.vertices.vertices[pc.scene.indices.indices[first] + mesh.vertexOffset];
    Vertex v1 = pc.scene.vertices.vertices[pc.scene.indices.indices[first + 1u] + mesh.vertexOffset];
    Vertex v2 = pc.scene.vertices.vertices[pc.scene.indices.indices[first + 2u] + mesh.vertexOffset];
    vec3 w = vec3(1.0 - barycentrics.x - barycentrics.y, barycentrics);
    vec2 uv = w.x * vec2(v0.uvX, v0.uvY) + w.y * vec2(v1.uvX, v1.uvY) +
              w.z * vec2(v2.uvX, v2.uvY);

    Material m = pc.scene.materials.materials[mesh.materialIndex];
    float alpha = m.baseColorFactor.a;
    if (m.baseColorTexture != kNoTexture) {
        alpha *= textureLod(textures[nonuniformEXT(m.baseColorTexture)], uv, 0.0).a;
    }
    return alpha >= m.alphaCutoff;
}

// Traces one ray toward the sun: the fraction of sunlight that arrives. 0 if
// the terrain (its heightfield, marched from marchOrigin) or an object (the
// TLAS, from origin + start direction) blocks it. A receiver beyond
// kFarShadowDistance traces the TLAS without the far-field trees, then the
// far field's transmittance from the same start.
float traceSunVisibility(vec3 origin, vec3 marchOrigin, vec3 direction, float start) {
    if (terrainOccludes(pc.scene, marchOrigin, direction, kShadowRayLength)) {
        return 0.0;
    }
    bool far = pc.scene.voxelFarShadows != 0u &&
               distance(origin, pc.scene.cameraPosition) > kFarShadowDistance;
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
    if (!far) {
        return 1.0;
    }
    FarFieldBuffer farField = pc.scene.farField;
    return farFieldTransmittance(farField, origin, direction, start, kShadowRayLength, false);
}

// ---------------------------------------------------------------- Surface

struct Surface {
    vec3 baseColor;
    float alpha;
    float metallic;
    float perceptualRoughness;
    float occlusion;
    vec3 emissive;
    vec3 normal;
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
    // Diffuse and specular are kept apart: a blended surface (glass) fades
    // its diffuse with alpha but keeps its full reflection.
    vec3 diffuseLight;
    vec3 specularLight;
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
    sh.sunAtSurface = transmittanceToSun(height, dot(up, sh.L));

    sh.diffuseLight = vec3(0.0);
    sh.specularLight = vec3(0.0);
    sh.NoL = clamp(dot(sh.N, sh.L), 0.0, 1.0);
    return sh;
}

void addSunLight(inout Shading sh, float visibility) {
    vec3 H = normalize(sh.V + sh.L);
    float NoH = clamp(dot(sh.N, H), 0.0, 1.0);
    float LoH = clamp(dot(sh.L, H), 0.0, 1.0);

    vec3 specular = distributionGgx(NoH, sh.a) *
                    visibilitySmithGgxCorrelated(sh.NoV, sh.NoL, sh.a) *
                    fresnelSchlick(sh.f0, LoH) * sh.energyCompensation;
    vec3 sunLight = sh.sunAtSurface * (sh.NoL * visibility);
    sh.diffuseLight = sh.diffuseColor / kPi * sunLight;
    sh.specularLight = specular * sunLight;
}

// Ambient from the sky. Diffuse: sky irradiance from above, light bounced
// off the ground from below. Specular: the sky in the reflected direction,
// blurred toward the irradiance as roughness grows (until reflections are
// ray traced).
void addAmbientLight(inout Shading sh, float occlusion) {
    vec3 skyIrradiance = pc.scene.skyIrradiance.irradiance;
    vec3 groundIrradiance =
        kGroundAlbedo * (sh.sunAtSurface * max(sh.L.y, 0.0) + skyIrradiance);
    vec3 diffuseIrradiance = mix(groundIrradiance, skyIrradiance, sh.N.y * 0.5 + 0.5);
    vec3 R = reflect(-sh.V, sh.N);
    vec3 skyRadiance = mix(skyLuminance(sh.position, R), skyIrradiance / kPi, sh.a);
    vec3 specularReflectance = mix(sh.dfg.xxx, sh.dfg.yyy, sh.f0) * sh.energyCompensation;
    float specularOcclusion = specularAmbientOcclusion(sh.NoV, occlusion, sh.a);
    sh.diffuseLight += sh.diffuseColor / kPi * diffuseIrradiance *
                       multiBounceOcclusion(occlusion, sh.diffuseColor);
    sh.specularLight += specularReflectance * skyRadiance *
                        multiBounceOcclusion(specularOcclusion, sh.f0);
}

// Aerial perspective: haze between the camera and the point as displayed
// (`relativePosition` is the flat world's).
vec3 applyAerialPerspective(vec3 luminance, vec3 relativePosition, float coverage) {
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
                           vec3(gl_FragCoord.xy * pc.scene.inverseResolution, w), 0.0);
    return luminance * mix(1.0, haze.a, weight) + haze.rgb * (weight * coverage);
}

#endif
