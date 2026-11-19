#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#include "common.glsl"
#include "atmosphere.glsl"
#include "material.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inCurrentClip;
layout(location = 3) in vec4 inPreviousClip;
layout(location = 4) in vec2 inUv;
layout(location = 5) in vec4 inTangent;
layout(location = 6) flat in uint inMaterialIndex;

layout(location = 0) out vec4 outColor;
// Screen-space motion in UV units, pointing from this frame's position to
// last frame's (the convention DLSS also uses).
layout(location = 1) out vec2 outMotion;

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

// Only opaque and alpha-masked instances cast shadows (see buildTlas).
const uint kShadowCasterMask = 0x01u;

// Alpha-masked geometry is non-opaque in the acceleration structure, so the
// ray query reports each candidate hit; we accept it only where the base
// color alpha passes the material's cutoff.
bool alphaTestCandidate(rayQueryEXT query) {
    InstanceData instance = pc.scene.instances.instances[
        rayQueryGetIntersectionInstanceCustomIndexEXT(query, false)];
    MeshInfo mesh = pc.scene.meshes.meshes[instance.meshIndex];
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

// Traces one ray toward the sun: 1 if it escapes, 0 if anything blocks it.
float sunVisibility(vec3 position, vec3 geometricNormal) {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, accelerationStructureEXT(pc.scene.tlas),
                          gl_RayFlagsTerminateOnFirstHitEXT, kShadowCasterMask,
                          position + geometricNormal * kShadowRayBias, 0.0,
                          sampleSunDirection(gl_FragCoord.xy), kShadowRayLength);
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
                gl_RayQueryCandidateIntersectionTriangleEXT &&
            alphaTestCandidate(query)) {
            rayQueryConfirmIntersectionEXT(query);
        }
    }
    return rayQueryGetIntersectionTypeEXT(query, true) ==
                   gl_RayQueryCommittedIntersectionNoneEXT
               ? 1.0
               : 0.0;
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

vec4 sampleTexture(uint index, vec2 uv) {
    return texture(textures[nonuniformEXT(index)], uv);
}

// Grass, rock and snow chosen by slope and height, until terrain gets real
// material layers.
Surface proceduralTerrain(vec3 normal) {
    const vec3 kGrass = vec3(0.06, 0.13, 0.03);
    const vec3 kRock = vec3(0.20, 0.18, 0.16);
    const vec3 kSnow = vec3(0.80, 0.82, 0.85);

    Surface s;
    s.baseColor = mix(kRock, kGrass, smoothstep(0.75, 0.9, normal.y));
    float snow = smoothstep(150.0, 190.0, inWorldPosition.y) *
                 smoothstep(0.6, 0.8, normal.y);
    s.baseColor = mix(s.baseColor, kSnow, snow);
    s.alpha = 1.0;
    s.metallic = 0.0;
    s.perceptualRoughness = mix(0.9, 0.5, snow);
    s.occlusion = 1.0;
    s.emissive = vec3(0.0);
    s.normal = normal;
    return s;
}

Surface materialSurface(Material m, vec3 normal) {
    Surface s;
    vec4 baseColor = m.baseColorFactor;
    if (m.baseColorTexture != kNoTexture) {
        baseColor *= sampleTexture(m.baseColorTexture, inUv);
    }
    s.baseColor = baseColor.rgb;
    s.alpha = baseColor.a;

    // glTF packs roughness in green and metalness in blue.
    s.metallic = m.metallicFactor;
    s.perceptualRoughness = m.roughnessFactor;
    if (m.metallicRoughnessTexture != kNoTexture) {
        vec4 mr = sampleTexture(m.metallicRoughnessTexture, inUv);
        s.perceptualRoughness *= mr.g;
        s.metallic *= mr.b;
    }

    s.occlusion = 1.0;
    if (m.occlusionTexture != kNoTexture) {
        s.occlusion = mix(1.0, sampleTexture(m.occlusionTexture, inUv).r,
                          m.occlusionStrength);
    }

    s.emissive = m.emissiveFactor;
    if (m.emissiveTexture != kNoTexture) {
        s.emissive *= sampleTexture(m.emissiveTexture, inUv).rgb;
    }

    s.normal = normal;
    if (m.normalTexture != kNoTexture && dot(inTangent.xyz, inTangent.xyz) > 0.0) {
        vec3 t = normalize(inTangent.xyz - normal * dot(normal, inTangent.xyz));
        vec3 b = cross(normal, t) * inTangent.w;
        vec3 n = sampleTexture(m.normalTexture, inUv).xyz * 2.0 - 1.0;
        n.xy *= m.normalScale;
        s.normal = normalize(mat3(t, b, normal) * n);
    }
    return s;
}

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

void main() {
    Material material = pc.scene.materials.materials[inMaterialIndex];
    vec3 normal = normalize(inNormal);
    if (!gl_FrontFacing) {
        normal = -normal;
    }

    Surface s = (material.flags & kMaterialProceduralTerrain) != 0u
                    ? proceduralTerrain(normal)
                    : materialSurface(material, normal);
#ifdef ALPHA_MASK
    if (s.alpha < material.alphaCutoff) {
        discard;
    }
#endif

    vec3 N = s.normal;
    vec3 V = normalize(pc.scene.cameraPosition - inWorldPosition);
    vec3 L = pc.scene.sunDirection;
    float NoV = max(dot(N, V), 1e-4);

    float perceptualRoughness = clamp(s.perceptualRoughness, kMinPerceptualRoughness, 1.0);
    float a = perceptualRoughness * perceptualRoughness;
    // Dielectrics reflect ~4% at normal incidence; metals tint reflection.
    vec3 f0 = mix(vec3(0.04), s.baseColor, s.metallic);
    vec3 diffuseColor = s.baseColor * (1.0 - s.metallic);

    vec2 dfg = textureLod(dfgLut, vec2(NoV, perceptualRoughness), 0.0).xy;
    vec3 energyCompensation = 1.0 + f0 * (1.0 / max(dfg.y, 1e-4) - 1.0);

    // Offset shadow rays along the true triangle normal: the smooth shading
    // normal can point below the surface and make the ray hit its own
    // triangle. Derivatives must be taken outside the branch below.
    vec3 geometricNormal =
        normalize(cross(dFdx(inWorldPosition), dFdy(inWorldPosition)));
    if (dot(geometricNormal, normal) < 0.0) {
        geometricNormal = -geometricNormal;
    }

    // All lighting is relative to a sun illuminance of 1 (as in the
    // atmosphere LUTs) and scaled by the real sun at the end.
    vec3 position = atmospherePosition(inWorldPosition);
    float height = length(position);
    vec3 up = position / height;
    vec3 sunAtSurface = transmittanceToSun(height, dot(up, L));

    // Diffuse and specular are kept apart: a blended surface (glass) fades
    // its diffuse with alpha but keeps its full reflection.
    vec3 diffuseLight = vec3(0.0);
    vec3 specularLight = vec3(0.0);
    float NoL = clamp(dot(N, L), 0.0, 1.0);
    if (NoL > 0.0 && dot(geometricNormal, L) > 0.0) {
        float visibility = sunVisibility(inWorldPosition, geometricNormal);
        vec3 H = normalize(V + L);
        float NoH = clamp(dot(N, H), 0.0, 1.0);
        float LoH = clamp(dot(L, H), 0.0, 1.0);

        vec3 specular = distributionGgx(NoH, a) *
                        visibilitySmithGgxCorrelated(NoV, NoL, a) *
                        fresnelSchlick(f0, LoH) * energyCompensation;
        vec3 sunLight = sunAtSurface * (NoL * visibility);
        diffuseLight = diffuseColor / kPi * sunLight;
        specularLight = specular * sunLight;
    }

    // Ambient from the sky. Diffuse: sky irradiance from above, light bounced
    // off the ground from below. Specular: the sky in the reflected
    // direction, blurred toward the irradiance as roughness grows (until
    // reflections are ray traced).
    vec3 skyIrradiance = pc.scene.skyIrradiance.irradiance;
    vec3 groundIrradiance =
        kGroundAlbedo * (sunAtSurface * max(L.y, 0.0) + skyIrradiance);
    vec3 diffuseIrradiance = mix(groundIrradiance, skyIrradiance, N.y * 0.5 + 0.5);
    vec3 R = reflect(-V, N);
    vec3 skyRadiance = mix(skyLuminance(position, R), skyIrradiance / kPi, a);
    vec3 specularReflectance = mix(dfg.xxx, dfg.yyy, f0) * energyCompensation;
    diffuseLight += diffuseColor / kPi * diffuseIrradiance * s.occlusion;
    specularLight += specularReflectance * skyRadiance * s.occlusion;

#ifdef ALPHA_BLEND
    // Premultiplied alpha (blend: one, one-minus-source-alpha).
    float coverage = s.alpha;
    vec3 luminance = diffuseLight * coverage + specularLight;
#else
    float coverage = 1.0;
    vec3 luminance = diffuseLight + specularLight;
#endif

    // Aerial perspective: haze between the camera and this point.
    float distanceKm = length(inWorldPosition - pc.scene.cameraPosition) * 0.001;
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
    luminance = luminance * mix(1.0, haze.a, weight) + haze.rgb * (weight * coverage);

    // Emissive is already in physical units (cd/m^2), not relative to the sun.
    vec3 exposed = luminance * pc.scene.sunIlluminance + s.emissive;
    outColor = vec4(exposed * pc.scene.exposure.exposure, coverage);

    vec2 currentUv = inCurrentClip.xy / inCurrentClip.w * 0.5;
    vec2 previousUv = inPreviousClip.xy / inPreviousClip.w * 0.5;
    outMotion = previousUv - currentUv;
}
