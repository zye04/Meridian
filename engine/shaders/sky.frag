#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "atmosphere.glsl"

layout(location = 0) in vec3 inViewRay;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;
layout(location = 3) out vec4 outDiffuseAlbedo;
layout(location = 4) out vec4 outSpecularAlbedo;
layout(location = 5) out vec4 outNormalRoughness;

// Keeps the sun disk inside FP16 range after pre-exposure.
const float kMaxExposedLuminance = 60000.0;

void main() {
    vec3 direction = normalize(inViewRay);
    vec3 camera = atmospherePosition(pc.scene.cameraPosition);
    float viewHeight = length(camera);
    vec3 up = camera / viewHeight;
    vec3 sun = pc.scene.sunDirection;

    vec3 luminance = vec3(0.0);
    if (viewHeight < pc.scene.atmosphere.topRadius) {
        luminance = skyLuminance(camera, direction);
        bool intersectsGround = raySphereIntersectNearest(
            camera, direction, pc.scene.atmosphere.bottomRadius) >= 0.0;

        // Sun disk: its illuminance spread over its solid angle, dimmed by the
        // atmosphere along the view ray.
        float radius = pc.scene.sunAngularRadius;
        if (dot(direction, sun) > cos(radius) && !intersectsGround) {
            float solidAngle = kPi * radius * radius;
            luminance += transmittanceToSun(viewHeight, dot(up, sun)) / solidAngle;
        }
    }

    vec3 exposed = luminance * pc.scene.sunIlluminance * pc.scene.exposure.exposure;
    outColor = vec4(min(exposed, vec3(kMaxExposedLuminance)), 1.0);

    // The sky is infinitely far, so only camera rotation moves it.
    vec4 currentClip = pc.scene.currentViewProjection * vec4(direction, 0.0);
    vec4 previousClip = pc.scene.previousViewProjection * vec4(direction, 0.0);
    outDiffuseAlbedo = vec4(0.5, 0.5, 0.5, 1.0);
    outSpecularAlbedo = vec4(0.0);
    outNormalRoughness = vec4(0.0);
    outMotion = (previousClip.xy / previousClip.w - currentClip.xy / currentClip.w) * 0.5;
}
