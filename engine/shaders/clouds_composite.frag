#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Composites the cloud march over the HDR target (premultiplied blend:
// clouds + background x transmittance), upsampled bilinearly. Skips pixels
// whose scene surface is nearer than the clouds' mean depth, so clouds
// don't draw over terrain. Task 4's temporal resolve and depth-aware
// upsample replace this.

layout(set = 2, binding = 0) uniform sampler2D sceneDepth;
layout(set = 2, binding = 3) uniform sampler2D cloudColor;
layout(set = 2, binding = 4) uniform sampler2D cloudDepth;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = gl_FragCoord.xy * pc.scene.inverseResolution;
    vec4 cloud = textureLod(cloudColor, uv, 0.0);
    float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (depth > 0.0) {
        // Both as view-space z: the scene's from reverse-Z, the clouds' from
        // their distance along this pixel's ray.
        vec2 ndc = uv * 2.0 - 1.0;
        float cosine = inversesqrt(1.0 + dot(ndc / pc.scene.projectionScale, ndc / pc.scene.projectionScale));
        if (pc.scene.nearPlane / depth < textureLod(cloudDepth, uv, 0.0).r * cosine) {
            cloud = vec4(0.0, 0.0, 0.0, 1.0);
        }
    }
    outColor = vec4(cloud.rgb, 1.0 - cloud.a);
}
