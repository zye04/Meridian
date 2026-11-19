#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Placeholder sky: a uniform color behind everything. The physically based
// atmosphere replaces it.

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;

void main() {
    outColor = vec4(pc.scene.skyLuminance * pc.scene.exposure.exposure, 1.0);
    // Uniform color, so motion doesn't matter yet.
    outMotion = vec2(0.0);
}
