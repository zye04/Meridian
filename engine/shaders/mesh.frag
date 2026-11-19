#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec4 outColor;

// Albedos are linear reflectances, roughly matching real materials.
const vec3 kGrass = vec3(0.06, 0.13, 0.03);
const vec3 kRock = vec3(0.20, 0.18, 0.16);
const vec3 kSnow = vec3(0.80, 0.82, 0.85);
const vec3 kGroundBounce = vec3(0.05, 0.05, 0.04);

void main() {
    vec3 normal = normalize(inNormal);

    vec3 albedo = mix(kRock, kGrass, smoothstep(0.75, 0.9, normal.y));
    float snow = smoothstep(150.0, 190.0, inWorldPosition.y) *
                 smoothstep(0.6, 0.8, normal.y);
    albedo = mix(albedo, kSnow, snow);

    // Direct sunlight (Lambert) plus hemisphere ambient: light from the sky
    // above and from the ground below.
    float sun = max(dot(normal, pc.scene.sunDirection), 0.0);
    vec3 ambient = mix(kGroundBounce, pc.scene.skyColor, normal.y * 0.5 + 0.5);
    vec3 lit = albedo * (pc.scene.sunColor * sun + ambient);

    // Linear output; the sRGB swapchain applies the gamma encoding.
    outColor = vec4(lit, 1.0);
}
