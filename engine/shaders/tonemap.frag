#version 460

// AgX display transform (Troy Sobotka), following Filament's implementation
// (google/filament, src/ToneMapper.cpp, Apache-2.0). AgX works on Rec.2020
// primaries, so the scene's linear Rec.709 color is converted on the way in
// and out. Input is already exposed (pre-exposure happens in the scene pass).

#extension GL_GOOGLE_include_directive : require
#include "post.glsl"

layout(location = 0) out vec4 outColor;

const mat3 kRec709ToAgxInset = mat3(
    0.544814816, 0.140416759, 0.088810540,
    0.373786466, 0.754137100, 0.178871522,
    0.081398195, 0.105444695, 0.732317593);
const mat3 kAgxOutset = mat3(
    1.127100582, -0.141329763, -0.141329763,
    -0.110606643, 1.157823702, -0.110606643,
    -0.016493939, -0.016493939, 1.251936407);
const mat3 kRec2020ToRec709 = mat3(
    1.660490296, -0.124549970, -0.018151119,
    -0.587639106, 1.132899922, -0.100578724,
    -0.072851598, -0.008347964, 1.118729878);

const float kMinEv = -12.47393;
const float kMaxEv = 4.026069;

vec3 agxContrast(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    vec3 x6 = x4 * x2;
    return -17.86 * x6 * x + 78.01 * x6 - 126.7 * x4 * x + 92.06 * x4 -
           28.72 * x2 * x + 4.361 * x2 - 0.1718 * x + 0.002857;
}

vec3 agx(vec3 color) {
    vec3 v = kRec709ToAgxInset * max(color, 0.0);
    v = clamp((log2(max(v, 1e-10)) - kMinEv) / (kMaxEv - kMinEv), 0.0, 1.0);
    v = agxContrast(v);
    v = kAgxOutset * v;
    v = pow(max(v, 0.0), vec3(2.2));
    return kRec2020ToRec709 * v;
}

vec3 linearToSrgb(vec3 c) {
    return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
               step(0.0031308, c));
}

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)),
               step(0.04045, c));
}

// Triangular-PDF noise in [-1, 1] (Gjøl, "Banding in Games", GDC 2016) from
// interleaved gradient noise (Jimenez 2014). Triangular rather than uniform
// so the noise's variance doesn't depend on the signal.
float ditherNoise(vec2 pixel) {
    float n = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
    n = n * 2.0 - 1.0;
    return sign(n) * (1.0 - sqrt(1.0 - abs(n)));
}

void main() {
    vec3 color = texelFetch(resolved, ivec2(gl_FragCoord.xy), 0).rgb;
    // The swapchain is sRGB, so the 8-bit steps are uniform in encoded
    // space: dither there by +-1 step, then hand the hardware linear again.
    vec3 encoded = linearToSrgb(clamp(agx(color), 0.0, 1.0));
    encoded += ditherNoise(gl_FragCoord.xy) / 255.0;
    outColor = vec4(srgbToLinear(clamp(encoded, 0.0, 1.0)), 1.0);
}
