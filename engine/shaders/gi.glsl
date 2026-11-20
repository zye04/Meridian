#ifndef GI_GLSL
#define GI_GLSL

// Global illumination probes (gi.h is the CPU reference): a camera-centred
// clipmap of kGiLevels levels, each kGiColumns^2 columns of kGiSlices
// probes at heights b (2^t - 1) above the column's base (the max of the
// heightfield over the column, so no probe is underground). A probe holds
// L1 SH radiance per colour channel; gi_probes.comp traces them.

const uint kGiLevels = 5u;
const int kGiColumns = 48;
const uint kGiSlices = 8u;
const float kY00 = 0.282095;
const float kY1 = 0.488603;

struct GiLevel {
    // First column of the window (world column = floor(x / spacing)).
    ivec2 origin;
    float spacing;
    float sliceBase;
};

struct GiProbe {
    // Per channel (r, g, b): Y00, Y1 {y, z, x}.
    vec4 sh[3];
    // The world column the probe was traced for; another column's data
    // (a toroidal slot reused after a scroll) doesn't count.
    ivec2 column;
    float base;
    // 0 until first traced.
    float valid;
};

layout(buffer_reference, std430) buffer GiBuffer {
    GiLevel levels[kGiLevels];
    uint frame;
    // 1: every probe is retraced without history.
    uint reset;
    uint pad0;
    uint pad1;
    GiProbe probes[];
};

// GLSL leaves % undefined for negative operands.
int giWrap(int column) {
    return column - kGiColumns * int(floor(float(column) / float(kGiColumns)));
}

uint giProbeIndex(uint level, ivec2 column, uint slice) {
    return ((level * uint(kGiColumns) + uint(giWrap(column.y))) * uint(kGiColumns) +
            uint(giWrap(column.x))) * kGiSlices + slice;
}

float giSliceHeight(GiLevel level, float slice) {
    return level.sliceBase * (exp2(slice) - 1.0);
}

float giSliceCoordinate(GiLevel level, float height) {
    return clamp(log2(1.0 + max(height, 0.0) / level.sliceBase), 0.0, float(kGiSlices - 1u));
}

// Irradiance from L1 radiance, one channel (clamped linear convolution
// with the cosine lobe, as gi.cpp's shIrradiance).
float giIrradianceChannel(vec4 c, vec3 n) {
    float e0 = kPi * kY00 * c.x;
    float e1 = (2.0 * kPi / 3.0) * kY1;
    return max(e0 + e1 * dot(c.yzw, n.yzx), 0.0);
}

// One level's trilinear probe radiance at `p`; weight 0 when the level
// doesn't hold the point or its probes aren't traced yet. `edge`: how far
// inside the window, in columns.
float giSampleLevel(GiBuffer gi, uint k, vec3 p, out vec4 sh[3], out float edge) {
    GiLevel level = gi.levels[k];
    vec2 f = p.xz / level.spacing - 0.5;
    ivec2 c0 = ivec2(floor(f));
    vec2 w = f - vec2(c0);
    ivec2 lo = c0 - level.origin;
    sh[0] = sh[1] = sh[2] = vec4(0.0);
    vec2 d = f - vec2(level.origin);
    edge = min(min(d.x, d.y), min(float(kGiColumns - 2) - d.x, float(kGiColumns - 2) - d.y));
    if (min(min(lo.x, lo.y), min(kGiColumns - 2 - lo.x, kGiColumns - 2 - lo.y)) < 0) {
        return 0.0;
    }
    float total = 0.0;
    for (int i = 0; i < 4; ++i) {
        ivec2 column = c0 + ivec2(i & 1, i >> 1);
        float wxz = ((i & 1) != 0 ? w.x : 1.0 - w.x) * ((i >> 1) != 0 ? w.y : 1.0 - w.y);
        GiProbe bottom = gi.probes[giProbeIndex(k, column, 0u)];
        if (bottom.valid == 0.0 || bottom.column != column || wxz <= 0.0) {
            continue;
        }
        float t = giSliceCoordinate(level, p.y - bottom.base);
        uint t0 = min(uint(t), kGiSlices - 2u);
        float ft = t - float(t0);
        GiProbe a = gi.probes[giProbeIndex(k, column, t0)];
        GiProbe b = gi.probes[giProbeIndex(k, column, t0 + 1u)];
        for (int ch = 0; ch < 3; ++ch) {
            sh[ch] += wxz * mix(a.sh[ch], b.sh[ch], ft);
        }
        total += wxz;
    }
    if (total < 1e-3) {
        return 0.0;
    }
    for (int ch = 0; ch < 3; ++ch) {
        sh[ch] /= total;
    }
    return 1.0;
}

struct GiSample {
    vec4 sh[3];
    // The share left to the caller's fallback (outside the clipmap).
    float remaining;
};

// The probes' radiance around a surface at `worldPosition` (offset along
// `normal`), from the finest level that holds it, blended into the next
// over its last 4 columns and into the fallback past the coarsest.
GiSample giSample(vec3 worldPosition, vec3 normal) {
    GiBuffer gi = pc.scene.gi;
    GiSample result;
    result.sh = vec4[3](vec4(0.0), vec4(0.0), vec4(0.0));
    result.remaining = 1.0;
    for (uint k = 0u; k < kGiLevels && result.remaining > 0.0; ++k) {
        vec3 p = worldPosition + normal * (0.25 * gi.levels[k].spacing);
        vec4 sh[3];
        float edge;
        if (giSampleLevel(gi, k, p, sh, edge) == 0.0) {
            continue;
        }
        float w = result.remaining * clamp(edge / 4.0, 0.0, 1.0);
        for (int ch = 0; ch < 3; ++ch) {
            result.sh[ch] += w * sh[ch];
        }
        result.remaining -= w;
    }
    return result;
}

// Irradiance on a surface facing `n`; `fallback` is the analytic sky +
// ground term for the share outside the clipmap.
vec3 giIrradiance(GiSample s, vec3 n, vec3 fallback) {
    return vec3(giIrradianceChannel(s.sh[0], n), giIrradianceChannel(s.sh[1], n),
                giIrradianceChannel(s.sh[2], n)) +
           s.remaining * fallback;
}

#endif
