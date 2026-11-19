// Cloud functions shared by the weather map and the cloud passes: tileable
// noise, the weather map with its analytic fallback beyond the window, the
// layer's vertical profiles and the density. Include after common.glsl and
// weather.glsl; reads pc.scene.
#ifndef CLOUDS_GLSL
#define CLOUDS_GLSL

#include "material.glsl"

// The noise volumes are 3D views in the same bindless table.
layout(set = 1, binding = 1) uniform sampler3D textures3D[];

// The weather map's noise octaves; beyond the window, and blended into its
// outer kWeatherMapBlendTexels, only the first (the coarse fallback).
const int kWeatherMapOctaves = 3;
const int kWeatherFallbackOctaves = 1;
const float kWeatherMapBlendTexels = 64.0;
// Width of the coverage threshold's soft edge in weather noise units.
const float kCoverageEdge = 0.15;
// Detail erosion: how far curl noise displaces the detail lookup (m) and how
// much of the base shape the detail can carve away (stratus, cumulus).
const float kCurlDistortion = 150.0;
const vec2 kDetailErosion = vec2(0.5, 0.35);
// The shape cut (stratus, cumulus): the product coverage x profile x shape
// below it is clear air. It carves the gaps and rounds the cumulus tops
// (the profile thins upward) and cell edges (coverage falls); low for
// stratus so overcast stays a nearly continuous sheet.
const vec2 kShapeCut = vec2(0.1, 0.35);
// The base shape's range, stretched to [0, 1]: the dilated Perlin-Worley
// spans only about 0.78-0.92 (measured: 6% below 0.8, median 0.84, 4%
// above 0.9), too little contrast to carve gaps on its own.
const vec2 kShapeRange = vec2(0.78, 0.92);
// Shape floor at full stratus coverage: lifts the shape so overcast reads as
// a continuous sheet (gaps only where the profile thins at its top and
// bottom and the detail erodes it), while broken stratocumulus keeps holes.
const float kStratusShapeFloor = 0.6;

float remap(float v, float lo, float hi, float newLo, float newHi) {
    return newLo + (v - lo) * (newHi - newLo) / (hi - lo);
}

uint cloudHash(uvec3 p) {
    uint h = p.x * 0x8da6b343u ^ p.y * 0xd8163841u ^ p.z * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// Three values in [0, 1] per lattice point; the lattice wraps at `period`,
// so the noise built on it tiles.
vec3 cloudHash3(ivec3 cell, ivec3 period) {
    uvec3 p = uvec3(mod(vec3(cell), vec3(period)));
    return vec3(cloudHash(p), cloudHash(p ^ uvec3(0x68e31da4u)), cloudHash(p ^ uvec3(0xb5297a4du))) *
           (1.0 / 4294967295.0);
}

// Perlin gradient noise in [0, 1].
float gradientNoise(vec3 p, ivec3 period) {
    ivec3 i = ivec3(floor(p));
    vec3 f = p - vec3(i);
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float corners[8];
    for (int k = 0; k < 8; ++k) {
        ivec3 o = ivec3(k & 1, (k >> 1) & 1, k >> 2);
        corners[k] = dot(cloudHash3(i + o, period) * 2.0 - 1.0, f - vec3(o));
    }
    float n = mix(mix(mix(corners[0], corners[1], u.x), mix(corners[2], corners[3], u.x), u.y),
                  mix(mix(corners[4], corners[5], u.x), mix(corners[6], corners[7], u.x), u.y), u.z);
    return clamp(n * 0.5 + 0.5, 0.0, 1.0);
}

// Inverted Worley noise (distance to the nearest feature point, one per
// cell): 1 at the feature points, so cells are bright at their cores.
float worleyNoise(vec3 p, ivec3 period) {
    ivec3 i = ivec3(floor(p));
    vec3 f = p - vec3(i);
    float nearest = 1.0;
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                ivec3 o = ivec3(x, y, z);
                vec3 v = vec3(o) + cloudHash3(i + o, period) - f;
                nearest = min(nearest, dot(v, v));
            }
        }
    }
    return 1.0 - sqrt(nearest);
}

// Octaves at double the frequency and half the amplitude, normalized to
// [0, 1]; `period` is the first octave's.
float gradientOctaves(vec3 p, ivec3 period, int octaves) {
    float sum = 0.0;
    float weight = 0.0;
    float amplitude = 1.0;
    for (int k = 0; k < octaves; ++k) {
        sum += amplitude * gradientNoise(p * float(1 << k), period << k);
        weight += amplitude;
        amplitude *= 0.5;
    }
    return sum / weight;
}

float worleyOctaves(vec3 p, ivec3 period, int octaves) {
    float sum = 0.0;
    float weight = 0.0;
    float amplitude = 1.0;
    for (int k = 0; k < octaves; ++k) {
        sum += amplitude * worleyNoise(p * float(1 << k), period << k);
        weight += amplitude;
        amplitude *= 0.5;
    }
    return sum / weight;
}

// The weather noise at world xz (m), carried by the wind and evolving along
// the time coordinate: Worley cells (cumulus clusters a few km across)
// modulated by Perlin noise four cells wide. Tiles every noisePeriod in xz
// and timePeriod in time. Mapped to about uniform on [0, 1] (a logistic
// approximation of the normal CDF; mean and single-octave deviation
// measured on map dumps, the deviation shrinking like a weighted mean of
// independent octaves), so the coverage threshold covers the same
// fraction of sky whatever the octave count, and the fallback matches the
// map's statistics.
float weatherNoise(vec2 xz, int octaves) {
    Clouds c = pc.scene.clouds;
    int cells = int(c.noisePeriod / c.weatherCell);
    ivec3 period = ivec3(cells, cells, int(c.timePeriod));
    vec3 p = vec3((xz - c.windOffset) / c.weatherCell, c.time);
    float cellNoise = worleyOctaves(p, period, octaves);
    float regional = gradientOctaves(p * 0.25, period / 4, octaves);
    float weightSum = 2.0 - exp2(1.0 - float(octaves));
    float weightSquares = (1.0 - exp2(-2.0 * float(octaves))) / 0.75;
    float deviation = 0.18 * sqrt(weightSquares) / weightSum;
    float z = (cellNoise * mix(0.5, 1.5, regional) - 0.46) / deviation;
    return 1.0 / (1.0 + exp(-1.702 * z));
}

// The weather map's texel value (R coverage, G type, B precipitation) at
// world xz for the weather noise value n: the local state (weatherAt, the
// history rule), with coverage applied once, as a soft threshold, so cells
// grow from their cores and dissolve back, and the type varying slowly
// around the local one. The threshold's edge slides from above the noise's
// range (coverage 0: R = 0) to below it (coverage 1: R = 1 everywhere);
// with n about uniform, R > 0.5 covers c (1 + w) - w / 2 of the sky.
vec4 weatherFromNoise(vec2 xz, float n) {
    WeatherSample local = weatherAt(pc.scene, vec3(xz.x, 0.0, xz.y));
    float lo = (1.0 - local.coverage) * (1.0 + kCoverageEdge) - kCoverageEdge;
    float coverage = clamp((n - lo) / kCoverageEdge, 0.0, 1.0);
    Clouds c = pc.scene.clouds;
    int cells = int(c.noisePeriod / c.weatherCell) / 8;
    vec3 p = vec3((xz - c.windOffset) / (8.0 * c.weatherCell), c.time / 8.0);
    float variation = 0.8 * (gradientNoise(p, ivec3(cells, cells, int(c.timePeriod) / 8)) - 0.5);
    // Clamped like the RGBA8 map stores it, so the window edge matches.
    float type = clamp(local.type + variation, 0.0, 1.0);
    return vec4(coverage, type, local.precipitation, 0.0);
}

// The weather at world xz (y ignored): the map inside its window (texel
// centers), beyond it the coarse fallback the map's outer texels blend to,
// so the edge is continuous.
vec4 weatherMapAt(vec3 world) {
    Clouds c = pc.scene.clouds;
    vec2 uv = (world.xz - c.mapOrigin) / c.mapSize;
    float halfTexel = 0.5 / float(c.mapResolution);
    if (all(greaterThanEqual(uv, vec2(halfTexel))) && all(lessThanEqual(uv, vec2(1.0 - halfTexel)))) {
        return textureLod(textures[c.weatherMap], uv, 0.0);
    }
    return weatherFromNoise(world.xz, weatherNoise(world.xz, kWeatherFallbackOctaves));
}

// Bottom and top of the layer (m above sea level) for a cloud type (0
// stratus .. 1 cumulus); cumulus towers grow with coverage.
vec2 cloudLayer(float type, float coverage) {
    Clouds c = pc.scene.clouds;
    float t = clamp(type, 0.0, 1.0);
    float cumulusTop = mix(mix(c.cumulusBottom, c.cumulusTop, 0.4), c.cumulusTop, coverage);
    return vec2(mix(c.stratusBottom, c.cumulusBottom, t), mix(c.stratusTop, cumulusTop, t));
}

// Density by normalized height in the layer: stratus a slab with soft
// edges, cumulus a flat base rising fast and thinning toward the top.
float cloudProfile(float type, float h) {
    float stratus = smoothstep(0.0, 0.15, h) * (1.0 - smoothstep(0.7, 1.0, h));
    float cumulus = smoothstep(0.0, 0.07, h) * (1.0 - smoothstep(0.3, 1.0, h));
    return mix(stratus, cumulus, clamp(type, 0.0, 1.0));
}

// Cloud density in [0, 1] (the march scales it to extinction) at world xz
// and altitude world.y (m above sea level, on the curved layer); lod is the
// noise mip level (0 near the camera, coarser for longer steps). Weather
// coverage x vertical profile x the base Perlin-Worley shape (coverage is
// thresholded once, in the map), cut at kShapeCut, eroded by curl-distorted
// detail Worley noise (Schneider 2015). heightFraction: the normalized height in the
// local layer (lighting's ambient gradient).
float cloudDensity(vec3 world, float lod, out float heightFraction) {
    heightFraction = 0.0;
    vec4 weather = weatherMapAt(world);
    float coverage = weather.r;
    if (coverage <= 0.0) {
        return 0.0;
    }
    vec2 layer = cloudLayer(weather.g, coverage);
    float h = (world.y - layer.x) / (layer.y - layer.x);
    if (h <= 0.0 || h >= 1.0) {
        return 0.0;
    }
    heightFraction = h;
    Clouds c = pc.scene.clouds;
    vec3 p = vec3(world.x - c.windOffset.x, world.y, world.z - c.windOffset.y);
    vec4 base = textureLod(textures3D[c.baseNoise], p / c.baseNoisePeriod, lod);
    float worley = dot(base.gba, vec3(0.625, 0.25, 0.125));
    float shape = remap(base.r, worley - 1.0, 1.0, 0.0, 1.0);
    shape = clamp(remap(shape, kShapeRange.x, kShapeRange.y, 0.0, 1.0), 0.0, 1.0);
    float shapeFloor = kStratusShapeFloor * coverage * (1.0 - clamp(weather.g, 0.0, 1.0));
    shape = mix(shape, 1.0, shapeFloor);
    float cut = mix(kShapeCut.x, kShapeCut.y, clamp(weather.g, 0.0, 1.0));
    float cloud = clamp(remap(coverage * cloudProfile(weather.g, h) * shape, cut, 1.0, 0.0, 1.0),
                        0.0, 1.0);
    if (cloud <= 0.0) {
        return 0.0;
    }
    vec2 curl = textureLod(textures[c.curlNoise], p.xz / c.curlNoisePeriod, 0.0).xy;
    vec3 q = p + vec3(curl.x, 0.0, curl.y) * (kCurlDistortion * (1.0 - h));
    float detail = dot(textureLod(textures3D[c.detailNoise], q / c.detailNoisePeriod, lod).rgb,
                       vec3(0.625, 0.25, 0.125));
    // Cumulus: wispy at the base, billowy toward the top; stratus wispy and
    // eroded more throughout.
    float type = clamp(weather.g, 0.0, 1.0);
    float billow = type * clamp(h * 4.0, 0.0, 1.0);
    float erosion = mix(detail, 1.0 - detail, billow) * mix(kDetailErosion.x, kDetailErosion.y, type);
    return clamp(remap(cloud, erosion, 1.0, 0.0, 1.0), 0.0, 1.0);
}

float cloudDensity(vec3 world, float lod) {
    float heightFraction;
    return cloudDensity(world, lod, heightFraction);
}

#endif
