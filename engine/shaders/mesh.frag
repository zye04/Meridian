#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#include "common.glsl"
#include "atmosphere.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inCurrentClip;
layout(location = 3) in vec4 inPreviousClip;

layout(location = 0) out vec4 outColor;
// Screen-space motion in UV units, pointing from this frame's position to
// last frame's (the convention DLSS also uses).
layout(location = 1) out vec2 outMotion;

// Albedos are linear reflectances, roughly matching real materials.
const vec3 kGrass = vec3(0.06, 0.13, 0.03);
const vec3 kRock = vec3(0.20, 0.18, 0.16);
const vec3 kSnow = vec3(0.80, 0.82, 0.85);
const float kGroundAlbedo = 0.1;
const float kShadowRayBias = 0.05;
const float kShadowRayLength = 20000.0;

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

// Traces one ray toward the sun: 1 if it escapes, 0 if anything blocks it.
float sunVisibility(vec3 position, vec3 geometricNormal) {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, accelerationStructureEXT(pc.scene.tlas),
                          gl_RayFlagsOpaqueEXT |
                              gl_RayFlagsTerminateOnFirstHitEXT,
                          0xFF, position + geometricNormal * kShadowRayBias,
                          0.0, sampleSunDirection(gl_FragCoord.xy),
                          kShadowRayLength);
    rayQueryProceedEXT(query);
    return rayQueryGetIntersectionTypeEXT(query, true) ==
                   gl_RayQueryCommittedIntersectionNoneEXT
               ? 1.0
               : 0.0;
}

void main() {
    vec3 normal = normalize(inNormal);

    vec3 albedo = mix(kRock, kGrass, smoothstep(0.75, 0.9, normal.y));
    float snow = smoothstep(150.0, 190.0, inWorldPosition.y) *
                 smoothstep(0.6, 0.8, normal.y);
    albedo = mix(albedo, kSnow, snow);

    // Offset shadow rays along the true triangle normal: the smooth shading
    // normal can point below the surface and make the ray hit its own
    // triangle. Derivatives must be taken outside the branch below.
    vec3 geometricNormal =
        normalize(cross(dFdx(inWorldPosition), dFdy(inWorldPosition)));
    if (dot(geometricNormal, normal) < 0.0) {
        geometricNormal = -geometricNormal;
    }

    float sunCosine = max(dot(normal, pc.scene.sunDirection), 0.0);
    if (sunCosine > 0.0) {
        sunCosine *= sunVisibility(inWorldPosition, geometricNormal);
    }

    // All lighting is relative to a sun illuminance of 1 (as in the
    // atmosphere LUTs) and scaled by the real sun at the end.
    vec3 position = atmospherePosition(inWorldPosition);
    float height = length(position);
    vec3 up = position / height;
    vec3 sunAtSurface = transmittanceToSun(height, dot(up, pc.scene.sunDirection));
    vec3 direct = albedo / kPi * sunAtSurface * sunCosine;

    // Hemisphere ambient: the sky's irradiance from above, and below the light
    // bounced off the ground (a diffuse surface lit by sun and sky).
    vec3 skyIrradiance = pc.scene.skyIrradiance.irradiance;
    vec3 groundIrradiance =
        kGroundAlbedo * (sunAtSurface * max(pc.scene.sunDirection.y, 0.0) + skyIrradiance);
    vec3 ambient = albedo / kPi *
                   mix(groundIrradiance, skyIrradiance, normal.y * 0.5 + 0.5);

    vec3 luminance = direct + ambient;

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
    luminance = luminance * mix(1.0, haze.a, weight) + haze.rgb * weight;

    outColor = vec4(luminance * pc.scene.sunIlluminance * pc.scene.exposure.exposure, 1.0);

    vec2 currentUv = inCurrentClip.xy / inCurrentClip.w * 0.5;
    vec2 previousUv = inPreviousClip.xy / inPreviousClip.w * 0.5;
    outMotion = previousUv - currentUv;
}
