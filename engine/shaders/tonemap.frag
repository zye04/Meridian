#version 460

// Display transform: one fetch from the grade LUT (grade + AgX, baked by
// grade_lut.comp), indexed by the log2 shaper of the AgX-inset colour.
// Input is already exposed (pre-exposure happens in the scene pass).

#extension GL_GOOGLE_include_directive : require
#include "grade.glsl"
#include "post.glsl"

layout(location = 0) out vec4 outColor;

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
    vec3 v = kRec709ToAgxInset * max(color, 0.0);
    vec3 t = clamp((log2(max(v, 1e-10)) - kLutMinEv) / (kLutMaxEv - kLutMinEv),
                   0.0, 1.0);
    const float n = float(kGradeLutSize);
    vec3 display = texture(gradeLut, t * (n - 1.0) / n + 0.5 / n).rgb;
    // The swapchain is sRGB, so the 8-bit steps are uniform in encoded
    // space: dither there by +-1 step, then hand the hardware linear again.
    vec3 encoded = linearToSrgb(clamp(display, 0.0, 1.0));
    encoded += ditherNoise(gl_FragCoord.xy) / 255.0;
    outColor = vec4(srgbToLinear(clamp(encoded, 0.0, 1.0)), 1.0);
}
