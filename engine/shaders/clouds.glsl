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
const vec2 kDetailErosion = vec2(0.5, 0.5);
// The shape cut (stratus, cumulus): thinning x shape below it is clear air.
// It carves the gaps and rounds the cumulus tops (the profile thins
// upward); low for stratus so overcast stays a nearly continuous sheet.
// Coverage multiplies the result (thresholded once, in the map).
const vec2 kShapeCut = vec2(0.1, 0.6);
// The base shape's range, stretched to [0, 1]: the dilated Perlin-Worley
// spans only about 0.78-0.92 (measured: 6% below 0.8, median 0.84, 4%
// above 0.9), too little contrast to carve gaps on its own.
const vec2 kShapeRange = vec2(0.78, 0.92);
// Shape floor for stratus (type 0, scaled down toward cumulus): lifts the
// shape so overcast reads as a continuous sheet (gaps only where the
// profile thins at its top and bottom and the detail erodes it), while
// mixed types keep holes.
const float kStratusShapeFloor = 0.6;
// Vertical frequency of the base shape over its horizontal one: cells
// wider than tall, so cumulus spread into flat-based loaves.
const float kShapeSquash = 2.0;
// Scales the eroded shape before it saturates at 1: cores reach the full
// extinction and edges sharpen.
const float kDensityGain = 6.0;
// Extinction (1/m) at density 1.
const float kCloudExtinction = 0.06;

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

// The vertical profile by normalized height in the layer: x the base ramp,
// applied after the shape cut so every cloud starts at the same height
// (flat bases); y the thinning toward the top, applied before the cut so
// clouds narrow upward (rounded tops). Stratus a slab with soft edges,
// cumulus a sharp base and a long taper.
vec2 cloudProfile(float type, float h) {
    float t = clamp(type, 0.0, 1.0);
    return vec2(smoothstep(0.0, mix(0.15, 0.07, t), h),
                1.0 - smoothstep(mix(0.7, 0.3, t), 1.0, h));
}

// Cloud density in [0, 1] (the march scales it to extinction) at world xz
// and altitude world.y (m above sea level, on the curved layer); lod is the
// noise mip level (0 near the camera, coarser for longer steps). Weather
// coverage (thresholded once, in the map) x the vertical profile times the
// base Perlin-Worley shape cut at kShapeCut, eroded by curl-distorted
// detail Worley noise (Schneider 2015). heightFraction: the normalized
// height in the local layer (lighting's ambient gradient).
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
    vec3 shapePoint = vec3(p.x, p.y * kShapeSquash, p.z);
    vec4 base = textureLod(textures3D[c.baseNoise], shapePoint / c.baseNoisePeriod, lod);
    float worley = dot(base.gba, vec3(0.625, 0.25, 0.125));
    float shape = remap(base.r, worley - 1.0, 1.0, 0.0, 1.0);
    shape = clamp(remap(shape, kShapeRange.x, kShapeRange.y, 0.0, 1.0), 0.0, 1.0);
    float type = clamp(weather.g, 0.0, 1.0);
    shape = mix(shape, 1.0, kStratusShapeFloor * (1.0 - type));
    float cut = mix(kShapeCut.x, kShapeCut.y, type);
    vec2 profile = cloudProfile(weather.g, h);
    float cloud = coverage * profile.x * clamp(remap(profile.y * shape, cut, 1.0, 0.0, 1.0), 0.0, 1.0);
    if (cloud <= 0.0) {
        return 0.0;
    }
    vec2 curl = textureLod(textures[c.curlNoise], p.xz / c.curlNoisePeriod, 0.0).xy;
    vec3 q = p + vec3(curl.x, 0.0, curl.y) * (kCurlDistortion * (1.0 - h));
    float detail = dot(textureLod(textures3D[c.detailNoise], q / c.detailNoisePeriod, lod).rgb,
                       vec3(0.625, 0.25, 0.125));
    // Cumulus: wispy at the base, billowy toward the top; stratus wispy and
    // eroded more throughout.
    float billow = type * clamp(h * 4.0, 0.0, 1.0);
    float erosion = mix(detail, 1.0 - detail, billow) * mix(kDetailErosion.x, kDetailErosion.y, type);
    return clamp(remap(cloud, erosion, 1.0, 0.0, 1.0) * kDensityGain, 0.0, 1.0);
}

float cloudDensity(vec3 world, float lod) {
    float heightFraction;
    return cloudDensity(world, lod, heightFraction);
}

// The cloud shadow map fades into the fallback over this fraction of its
// half-size at its edges (1.5 km), so its border leaves no line.
const float kCloudShadowBlend = 0.1;
// Sun height (sin of the elevation: 6 to 3 degrees) over which the map
// fades into the fallback; below it cloud_shadow.comp doesn't march. Lower
// down, the lines cross over 50 km of layer in 32 steps: the coarse noise
// mips they need erase the cumulus (the map turns clear) and finer ones
// alias into blocky self-shadow on the clouds.
const vec2 kCloudShadowSunFade = vec2(0.0523, 0.1045);
// The map serves receivers from sea level up to the layer top within half
// its size horizontally from the camera; the fallback serves the rest.
const float kCloudShadowReceiverBottom = 0.0;

// The Beer shadow map's transmittance toward the sun at world (Hillaire's
// UE4 clouds: R front depth along the light in km from the camera, G mean
// extinction 1/m, B total optical depth, A 1 where marched, 0 for lines
// that serve no receiver; cloud_shadow.comp), and its weight: 1 inside,
// fading to 0 at its edges across the sun, at half its size horizontally
// from the camera, and as the sun sets.
float cloudShadowMap(vec3 world, out float weight) {
    Clouds c = pc.scene.clouds;
    vec3 rel = world - pc.scene.cameraPosition;
    vec2 p = vec2(dot(rel, c.shadowRight), dot(rel, c.shadowUp)) + c.shadowCamera;
    float halfSize = 0.5 * c.shadowSize;
    vec3 inside = 1.0 - vec3(abs(p), length(rel.xz)) / halfSize;
    weight = clamp(min(inside.x, min(inside.y, inside.z)) / kCloudShadowBlend, 0.0, 1.0) *
             smoothstep(kCloudShadowSunFade.x, kCloudShadowSunFade.y, pc.scene.sunDirection.y);
    if (weight <= 0.0) {
        return 1.0;
    }
    vec4 texel = textureLod(textures[c.shadowMap], p / c.shadowSize + 0.5, 0.0);
    weight *= texel.a;
    if (weight <= 0.0) {
        return 1.0;
    }
    // Unmarched neighbours are all zero: dividing by the filtered A
    // averages the marched texels only.
    texel.rgb /= texel.a;
    float depth = dot(rel, c.shadowForward);
    return exp(-min(texel.b, texel.g * max(0.0, depth - texel.r * 1000.0)));
}

// Beyond the shadow map: the mean transmittance under the coarse weather,
// through the local layer (cloudLayer of the local state). One crossing of
// coverage R passes 1 - k R, read where the sun ray crosses the middle of
// the layer (the weather map inside its window, its analytic fallback
// beyond); sky with coverage R is cloudy over only part of its area (the
// shape cut and erosion carve gaps), so k = 1 - a type^p, fitted to the
// shadow map's mean transmittance (decisions.md). A low sun's ray runs S =
// layer height / tan(elevation) through the layer and crosses about S / L
// independent clouds: the rest pass the regional mean, the local state's
// mean coverage (weatherFromNoise's soft threshold over an about uniform
// noise), so the fallback darkens with the map toward the horizon.
// marched: world is a cloud sample whose own cloud the march's sun steps
// already crossed, so only the regional mean over the rest of the path
// applies. 1 above the layer.
const vec2 kCloudShadowFallbackFit = vec2(0.53, 3.5); // a, p
const float kCloudShadowCrossingLength = 5500.0;

float cloudShadowFallback(vec3 world, bool marched) {
    WeatherSample here = weatherAt(pc.scene, vec3(world.x, 0.0, world.z));
    vec2 layer = cloudLayer(here.type, here.coverage);
    if (world.y >= layer.y) {
        return 1.0;
    }
    vec3 sun = pc.scene.sunDirection;
    float bottom = max(layer.x, world.y);
    float sunHeight = max(sun.y, kCloudShadowSunFade.x);
    float middle = 0.5 * (bottom + layer.y);
    vec2 xz = world.xz + sun.xz * ((middle - world.y) / sunHeight);
    float crossings = (layer.y - bottom) * length(sun.xz) / (sunHeight * kCloudShadowCrossingLength);
    WeatherSample local = weatherAt(pc.scene, vec3(xz.x, 0.0, xz.y));
    float meanCoverage = clamp(local.coverage * (1.0 + kCoverageEdge) - 0.5 * kCoverageEdge, 0.0, 1.0);
    float meanK = 1.0 - kCloudShadowFallbackFit.x * pow(clamp(local.type, 0.0, 1.0), kCloudShadowFallbackFit.y);
    // pow is undefined for 0^0 (overcast at a high sun).
    float regional = max(1.0 - meanK * meanCoverage, 1e-6);
    if (marched) {
        return pow(regional, crossings);
    }
    vec4 weather = weatherMapAt(vec3(xz.x, middle, xz.y));
    float k = 1.0 - kCloudShadowFallbackFit.x * pow(clamp(weather.g, 0.0, 1.0), kCloudShadowFallbackFit.y);
    return (1.0 - k * weather.r) * pow(regional, max(0.0, crossings - 1.0));
}

// Sun transmittance through the clouds at world (flat, m): the shadow map
// near the camera, blended into the fallback beyond it; 1 with clouds off.
// marched: as cloudShadowFallback's.
float cloudShadow(vec3 world, bool marched) {
    if (pc.scene.clouds.shadowSize <= 0.0) {
        return 1.0;
    }
    float weight;
    float shadow = cloudShadowMap(world, weight);
    return weight >= 1.0 ? shadow : mix(cloudShadowFallback(world, marched), shadow, weight);
}

float cloudShadow(vec3 world) {
    return cloudShadow(world, false);
}

// Octahedral map of the unit sphere (y up): the upper hemisphere fills the
// inner diamond, the lower one folds over the corners.
vec2 octahedralUv(vec3 direction) {
    vec3 n = direction / (abs(direction.x) + abs(direction.y) + abs(direction.z));
    vec2 p = n.xz;
    if (n.y < 0.0) {
        p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    }
    return p * 0.5 + 0.5;
}

vec3 octahedralDirection(vec2 uv) {
    vec2 p = uv * 2.0 - 1.0;
    vec3 n = vec3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);
    if (n.y < 0.0) {
        n.xz = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

// The clouds toward a direction from the camera, from the sky capture
// (sky_capture.comp): rgb the in-scattered luminance with the haze in
// front (sun illuminance 1), a the transmittance; the sky behind them is
// the sky-view LUT's (sky x a + rgb). Clear with clouds off.
vec4 skyCaptureAt(vec3 direction) {
    if (pc.scene.clouds.shadowSize <= 0.0) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    return textureLod(textures[pc.scene.clouds.skyCapture], octahedralUv(direction), 0.0);
}

#endif
