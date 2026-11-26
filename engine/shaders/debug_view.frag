#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#define DEBUG_ID_CONSTANTS_ONLY
#include "debug_id.glsl"

// A debug view (meridian::DebugView) in place of the tonemap: reads the
// render-sized scene attachments at the nearest render pixel, so nothing is
// filtered or smeared. The target is sRGB-encoded: write linear colour.
// The lod colours are mirrored by lodViewLegend (renderer_debug.cpp).

layout(buffer_reference, std430) readonly buffer ClusterLevels {
    uint levels[];
};

layout(set = 0, binding = 0) uniform usampler2D debugId;
layout(set = 0, binding = 1) uniform sampler2D diffuseAlbedo;

layout(push_constant) uniform Push {
    ClusterLevels clusterLevels;
    uint view;
    uint pad;
    uvec2 renderSize;
    uvec2 targetSize;
} pc;

layout(location = 0) out vec4 outColor;

const uint kViewBaseColor = 1u;
const uint kViewLod = 2u;

vec3 hsv(float h, float s, float v) {
    vec3 k = clamp(abs(fract(h + vec3(0.0, 2.0, 1.0) / 3.0) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    return v * mix(vec3(1.0), k, s);
}

float clusterShade(uint cluster) {
    uint h = cluster * 0x9E3779B1u;
    h ^= h >> 15u;
    return 0.7 + 0.3 * float(h & 0xFFu) / 255.0;
}

uint clusterLevel(uint cluster) {
    return (pc.clusterLevels.levels[cluster >> 2u] >> ((cluster & 3u) * 8u)) & 0xFFu;
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy * vec2(pc.renderSize) / vec2(pc.targetSize));
    pixel = min(pixel, ivec2(pc.renderSize) - 1);
    if (pc.view == kViewBaseColor) {
        outColor = vec4(texelFetch(diffuseAlbedo, pixel, 0).rgb, 1.0);
        return;
    }
    uvec4 id = texelFetch(debugId, pixel, 0);
    uint kind = id.x >> 24u;
    vec3 color = vec3(0.0);
    if (kind == kDebugTriangles) {
        float level = float(min(clusterLevel(id.y), 7u));
        color = hsv(0.33 * (1.0 - level / 7.0), 0.85, clusterShade(id.y));
    } else if (kind == kDebugVoxels) {
        float level = float(min(clusterLevel(id.y), 7u));
        color = hsv(0.62 + 0.18 * level / 7.0, 0.8, clusterShade(id.y));
    } else if (kind == kDebugFarCell) {
        color = hsv(0.5, 0.8, 0.6 + 0.1 * float(id.y >> 30u));
    } else if (kind == kDebugTerrain) {
        color = vec3(0.25);
    }
    outColor = vec4(color, 1.0);
}
