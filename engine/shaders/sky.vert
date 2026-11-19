#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Fullscreen triangle that also outputs the world-space view ray, so the sky
// can be looked up by direction.

layout(location = 0) out vec3 outViewRay;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 ndc = uv * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);

    // A point on the near plane (depth 1 with reverse-Z), unjittered.
    vec4 nearPoint = inverse(pc.scene.currentViewProjection) * vec4(ndc, 1.0, 1.0);
    outViewRay = nearPoint.xyz / nearPoint.w - pc.scene.cameraPosition;
}
