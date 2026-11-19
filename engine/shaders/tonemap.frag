#version 460

// AgX display transform (Troy Sobotka), following Filament's implementation
// (google/filament, src/ToneMapper.cpp, Apache-2.0). AgX works on Rec.2020
// primaries, so the scene's linear Rec.709 color is converted on the way in
// and out. Input is already exposed (pre-exposure happens in the scene pass).

layout(set = 0, binding = 0) uniform sampler2D hdrColor;

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

void main() {
    vec3 color = texelFetch(hdrColor, ivec2(gl_FragCoord.xy), 0).rgb;
    // Linear output; the sRGB swapchain applies the display encoding.
    outColor = vec4(max(agx(color), 0.0), 1.0);
}
