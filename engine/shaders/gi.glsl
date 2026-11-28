#ifndef GI_GLSL
#define GI_GLSL

// Global illumination probes (gi.h is the CPU reference): a camera-centred
// clipmap of kGiLevels levels, each kGiColumns^2 columns of kGiSlices
// probes at heights b (2^t - 1) above the column's base (the terrain at the
// column's centre). A probe holds
// L1 SH radiance per colour channel; gi_probes.comp traces them. The ground
// cache (gi_ground.comp, gi_albedo.comp) holds, per texel of kGiGroundLevels
// camera-centred windows of kGiGroundTexels^2, the albedo seen on screen and
// the radiance the lit ground sends back: what a probe ray that hits the
// ground reads. Needs terrain.glsl, farfield.glsl, clouds.glsl and
// atmosphere.glsl.

const uint kGiLevels = 5u;
const int kGiColumns = 48;
const uint kGiSlices = 8u;
const float kY00 = 0.282095;
const float kY1 = 0.488603;
const uint kGiGroundLevels = 4u;
const int kGiGroundTexels = 256;
const uint kGiGroundCount = kGiGroundLevels * uint(kGiGroundTexels * kGiGroundTexels);
// Probe rays and sun rays from the ground; trees farther than
// kGiTreeDistance along a probe ray, or toward the sun, are left out: the
// far-field march is the passes' main cost with --trees.
const float kGiMaxDistance = 4000.0;
const float kGiTreeDistance = 300.0;
// The sun march from a foliage hit stops here: past a crown or two the ray
// has cleared the canopy (the full kGiTreeDistance cost +0.4 ms for no
// measured gain against the reference).
const float kGiLeafSunDistance = 64.0;

// The far field's canopy is a uniform medium per voxel, which hides the gaps
// between clumped leaves: mean exp(-tau) over a voxel exceeds exp(-mean
// tau). The GI's paths through it take the clumping index (Nilson 1971):
// transmittance exp(-Omega tau), set by --effects gi-clumping (1 = none).
float giClumped(float transmittance) {
    return pow(max(transmittance, 0.0), pc.scene.giCanopyClumping);
}
// Neutral ground albedo where the ground cache has none yet (a typical
// vegetated ground's luminance, no hue: the hue comes from the cache).
const vec3 kGiGroundAlbedo = vec3(0.079);

struct GiLevel {
    // First column of the window (world column = floor(x / spacing)); the
    // ground levels use the texel size as spacing.
    ivec2 origin;
    float spacing;
    float sliceBase;
};

struct GiGroundTexel {
    // Diffuse albedo seen on screen; a = confidence, 0 = never seen.
    vec4 albedo;
    // Outgoing radiance toward a probe ray (albedo / pi x irradiance); a = 1
    // once lit.
    vec4 radiance;
    // The world texel the slot holds; another texel's data doesn't count.
    ivec2 texel;
    uvec2 pad;
};

layout(buffer_reference, std430) buffer GiGroundBuffer {
    GiGroundTexel texels[kGiGroundCount];
    // Per texel: albedo sum (10-bit fixed point) and pixel count.
    uvec4 accum[kGiGroundCount];
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

// The probes' SH as half floats (packHalf2x16 pairs), three channels per
// probe in probe order: what the shading reads, 24 B per probe instead of 64.
layout(buffer_reference, std430) buffer GiVolumeBuffer {
    uvec2 texels[];
};

layout(buffer_reference, std430) buffer GiBuffer {
    GiLevel levels[kGiLevels];
    GiLevel ground[kGiGroundLevels];
    uint frame;
    // 1: every probe is retraced and every ground texel relit without history.
    uint reset;
    GiGroundBuffer groundBuffer;
    GiVolumeBuffer volumeBuffer;
    uint pad[2];
    GiProbe probes[];
};

// GLSL leaves % undefined for negative operands.
int giWrapN(int value, int n) {
    return value - n * int(floor(float(value) / float(n)));
}

int giWrap(int column) {
    return giWrapN(column, kGiColumns);
}

uint giGroundIndex(uint level, ivec2 texel) {
    return (level * uint(kGiGroundTexels) + uint(giWrapN(texel.y, kGiGroundTexels))) *
               uint(kGiGroundTexels) +
           uint(giWrapN(texel.x, kGiGroundTexels));
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

vec4 giLoadSh(GiBuffer gi, uint probe, uint channel) {
    uvec2 texel = gi.volumeBuffer.texels[probe * 3u + channel];
    return vec4(unpackHalf2x16(texel.x), unpackHalf2x16(texel.y));
}

// One level's trilinear probe radiance at `p`, with the slices counted from
// `base` (the terrain under p; the probes' own bases are the terrain under
// their columns); weight 0 when the level's window doesn't hold the point.
// `edge`: how far inside the window, in columns. Every probe in the windows
// is traced before the shading reads it.
float giSampleLevel(GiBuffer gi, uint k, vec3 p, float base, out vec4 sh[3], out float edge) {
    GiLevel level = gi.levels[k];
    vec2 f = p.xz / level.spacing - 0.5;
    vec2 d = f - vec2(level.origin);
    edge = min(min(d.x, d.y), min(float(kGiColumns - 2) - d.x, float(kGiColumns - 2) - d.y));
    sh[0] = sh[1] = sh[2] = vec4(0.0);
    if (edge < 0.0) {
        return 0.0;
    }
    ivec2 c0 = ivec2(floor(f));
    vec2 w = f - vec2(c0);
    float t = giSliceCoordinate(level, p.y - base);
    uint t0 = min(uint(t), kGiSlices - 2u);
    float ft = t - float(t0);
    for (int i = 0; i < 4; ++i) {
        ivec2 column = c0 + ivec2(i & 1, i >> 1);
        float wxz = ((i & 1) != 0 ? w.x : 1.0 - w.x) * ((i >> 1) != 0 ? w.y : 1.0 - w.y);
        uint probe = giProbeIndex(k, column, t0);
        for (uint ch = 0u; ch < 3u; ++ch) {
            sh[ch] += wxz * mix(giLoadSh(gi, probe, ch), giLoadSh(gi, probe + 1u, ch), ft);
        }
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
    Terrain terrain = pc.scene.terrain;
    float base = (terrain.width > 0u ? terrainHeight(terrain, worldPosition.xz) : 0.0) + 0.05;
    for (uint k = 0u; k < kGiLevels && result.remaining > 0.0; ++k) {
        vec3 p = worldPosition + normal * (0.25 * gi.levels[k].spacing);
        vec4 sh[3];
        float edge;
        if (giSampleLevel(gi, k, p, base, sh, edge) == 0.0) {
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

// Bi-Lambertian spherical-leaf phase over pi G per unit reflectance (x) and
// transmittance (y); the phase is linear in both.
vec2 leafPhaseWeights(float cosBeta) {
    float c = clamp(cosBeta, -1.0, 1.0);
    float b = acos(c);
    float reflected = (sin(b) - b * c) / (3.0 * kPi);
    return vec2(reflected, reflected + c / 3.0) / (0.5 * kPi);
}

// leafPhase in gi.h is the CPU reference.
vec3 leafPhase(float cosBeta, vec3 reflectance, vec3 transmittance) {
    vec2 w = leafPhaseWeights(cosBeta);
    return reflectance * w.x + transmittance * w.y;
}

// The ground cache's radiance at `xz` (w: how much of it the cache holds,
// 0..1, the rest is the caller's), from the finest level that holds the
// point, blended into the next over its last 4 texels. Bilinear over the
// texels lit and stamped for their world texel.
vec4 giGroundRadiance(vec2 xz) {
    GiBuffer gi = pc.scene.gi;
    vec3 sum = vec3(0.0);
    float covered = 0.0;
    float remaining = 1.0;
    for (uint k = 0u; k < kGiGroundLevels && remaining > 0.0; ++k) {
        GiLevel level = gi.ground[k];
        vec2 f = xz / level.spacing - 0.5;
        ivec2 c0 = ivec2(floor(f));
        vec2 w = f - vec2(c0);
        vec2 d = f - vec2(level.origin);
        float edge = min(min(d.x, d.y), min(float(kGiGroundTexels - 2) - d.x,
                                            float(kGiGroundTexels - 2) - d.y));
        if (edge < 0.0) {
            continue;
        }
        vec3 levelSum = vec3(0.0);
        float total = 0.0;
        for (int i = 0; i < 4; ++i) {
            ivec2 texel = c0 + ivec2(i & 1, i >> 1);
            float wxz = ((i & 1) != 0 ? w.x : 1.0 - w.x) * ((i >> 1) != 0 ? w.y : 1.0 - w.y);
            GiGroundTexel record = gi.groundBuffer.texels[giGroundIndex(k, texel)];
            if (record.radiance.a == 0.0 || record.texel != texel || wxz <= 0.0) {
                continue;
            }
            levelSum += wxz * record.radiance.rgb;
            total += wxz;
        }
        if (total < 1e-3) {
            continue;
        }
        float weight = remaining * clamp(edge / 4.0, 0.0, 1.0);
        sum += weight * levelSum / total;
        covered += weight;
        remaining -= weight;
    }
    return vec4(covered > 0.0 ? sum / covered : vec3(0.0), covered);
}

#if defined(TERRAIN_GLSL) && defined(CLOUDS_GLSL) && defined(FARFIELD_GLSL)
vec3 terrainNormalAt(Terrain t, vec2 xz) {
    const float e = 1.0;
    float dx = terrainHeight(t, xz + vec2(e, 0.0)) - terrainHeight(t, xz - vec2(e, 0.0));
    float dz = terrainHeight(t, xz + vec2(0.0, e)) - terrainHeight(t, xz - vec2(0.0, e));
    return normalize(vec3(-dx, 2.0 * e, -dz));
}

// The share of the sun that reaches ground at `p` with normal `n`: terrain,
// far-field trees (within kGiTreeDistance) and clouds; 0 facing away.
float giGroundSunVisibility(vec3 p, vec3 n) {
    vec3 sun = pc.scene.sunDirection;
    if (dot(n, sun) <= 0.0 ||
        terrainOccludes(pc.scene, p + n * 0.05, sun, kGiMaxDistance)) {
        return 0.0;
    }
    float visibility = cloudShadow(p);
    if (pc.scene.farFieldEnabled != 0u) {
        FarFieldBuffer farField = pc.scene.farField;
        visibility *= giClumped(farFieldTransmittance(farField, p, sun, 0.0, kGiTreeDistance, false));
    }
    return visibility;
}

#endif

#endif
