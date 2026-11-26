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
layout(buffer_reference, std430) readonly buffer DebugLight {
    uvec4 pixels[];
};

layout(set = 0, binding = 1) uniform sampler2D diffuseAlbedo;
layout(set = 0, binding = 2) uniform sampler2D hdr;

layout(push_constant) uniform Push {
    ClusterLevels clusterLevels;
    DebugLight debugLight;
    uint view;
    uint pad;
    uvec2 renderSize;
    uvec2 targetSize;
} pc;

layout(location = 0) out vec4 outColor;

const uint kViewBaseColor = 1u;
const uint kViewLod = 2u;
const uint kViewStretch = 3u;
const uint kViewSun = 4u;
const uint kViewAmbient = 5u;
const uint kViewSpecular = 6u;
const uint kViewLuminance = 7u;

// EV -5..+5 relative to 18% grey (sRGB); must match kEvColors in
// renderer_debug.cpp.
const uint kEvColors[11] = uint[](0x2a0050u, 0x3a20b0u, 0x2060e0u, 0x20a8c8u, 0x30a040u, 0x808080u,
                                  0xc8c830u, 0xf0d000u, 0xf08800u, 0xe02010u, 0xffffffu);

vec3 srgbToLinear(uint rgb) {
    vec3 c = vec3((rgb >> 16u) & 0xFFu, (rgb >> 8u) & 0xFFu, rgb & 0xFFu) / 255.0;
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

// The light views' tonemap: exposed radiance over 1 + its luminance, so
// hue and the ratio between the views stay as they are.
vec3 neutralTonemap(vec3 exposed) {
    float luminance = dot(max(exposed, 0.0), vec3(0.2126, 0.7152, 0.0722));
    return max(exposed, 0.0) / (1.0 + luminance);
}

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
    if (pc.view == kViewLuminance) {
        vec3 exposed = texelFetch(hdr, pixel, 0).rgb;
        float ev = log2(max(dot(exposed, vec3(0.2126, 0.7152, 0.0722)), 1e-9) / 0.18);
        outColor = vec4(srgbToLinear(kEvColors[uint(clamp(round(ev), -5.0, 5.0) + 5.0)]), 1.0);
        return;
    }
    if (pc.view >= kViewStretch) {
        uint i = (uint(pixel.y) * pc.renderSize.x + uint(pixel.x)) * 2u;
        uvec4 a = pc.debugLight.pixels[i];
        uvec4 b = pc.debugLight.pixels[i + 1u];
        vec3 color = vec3(0.0);
        if (a.x != 0u) {
            vec3 sunDiffuse = vec3(unpackHalf2x16(a.y), unpackHalf2x16(a.z).x);
            vec3 sunSpecular = vec3(unpackHalf2x16(a.z).y, unpackHalf2x16(a.w));
            vec3 ambientDiffuse = vec3(unpackHalf2x16(b.x), unpackHalf2x16(b.y).x);
            vec3 ambientSpecular = vec3(unpackHalf2x16(b.y).y, unpackHalf2x16(b.z));
            vec2 stretch = unpackHalf2x16(b.w);
            if (pc.view == kViewStretch) {
                float t = clamp(log2(max(stretch.y, 1.0)) / 2.0, 0.0, 1.0);
                color = stretch.x == 0.0 ? vec3(0.25)
                                         : hsv(0.33 * (1.0 - t), 0.85, stretch.x > 0.0 ? 1.0 : 0.6);
            } else if (pc.view == kViewSun) {
                color = neutralTonemap(sunDiffuse + sunSpecular);
            } else if (pc.view == kViewAmbient) {
                color = neutralTonemap(ambientDiffuse + ambientSpecular);
            } else {
                color = neutralTonemap(sunSpecular + ambientSpecular);
            }
        }
        outColor = vec4(color, 1.0);
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
