// 2.5D clouds: the heightfield cloud slab (decisions.md "2.5D clouds").
// Each cloud is a column over the layer base: at world xz the cloud fills
// altitudes [base, top] from 2D fields only (the weather map and slices of
// the noise volumes), so a ray finds a cloud's surface by a deterministic
// search with a bisection refinement (relief mapping) and integrates the
// chord behind it, lit analytically. No per-frame jitter and no history:
// the camera's sub-pixel jitter and the upscaler antialias the edges as
// they do geometry's. Include after clouds_march.glsl (its phase, octave,
// diffuse and ambient constants light the slab the same way).
#ifndef CLOUDS_SLAB_GLSL
#define CLOUDS_SLAB_GLSL

#include "clouds_slab_field.glsl"

// The baked field (cloud_slab_bake.comp) around the camera: RGBA16F
// (base, top relative to kSlabBakeOffset, rim) and its conservative bounds
// chain (RG16F, level k = (min base, max top) of the 2^k texels' bilinear
// support). The traversal skips cells whose bounds the ray's altitudes miss:
// kSlabTraverseSteps cell visits per call, kSlabCellSteps samples across a
// finest cell, kSlabAltMargin (m) of slack for the shell's curvature.
const int kSlabBakeLevels = 9;
const int kSlabTraverseSteps = 160;
const int kSlabCellSteps = 8;
const float kSlabAltMargin = 4.0;

// Search: steps grow with distance (kSlabStepGrowth x t, at least
// kSlabMinStep), at most kSlabSearchSteps; the surface is then refined by
// kSlabRefineSteps bisection steps; the chord behind it ends at the next
// outside sample (kSlabExitSteps growing steps, bisected) and is integrated
// from kSlabChordSamples midpoints.
const float kSlabMinStep = 30.0;
const float kSlabStepGrowth = 0.02;
const int kSlabSearchSteps = 192;
const int kSlabRefineSteps = 7;
const int kSlabExitSteps = 12;
const int kSlabChordSamples = 4;
// Sun taps along the column toward the sun (m), then the cloud shadow map.
const vec4 kSlabSunTaps = vec4(40.0, 120.0, 280.0, 600.0);

// Whether xz is inside the baked window (a half texel in from its edge,
// where the bilinear lookup is exact).
bool slabInWindow(vec2 xz) {
    Clouds c = pc.scene.clouds;
    if (c.renderer != 2u) {
        return false;
    }
    vec2 texels = (xz - c.slabOrigin) * (float(c.slabResolution) / c.slabSize);
    return all(greaterThanEqual(texels, vec2(0.5))) &&
           all(lessThan(texels, vec2(float(c.slabResolution) - 0.5)));
}

// The column at xz: the baked field inside the window, else slabColumn
// (layer is left unset; slabLayer fills it where the light needs it).
SlabColumn slabSample(vec2 xz, float lod) {
    if (slabInWindow(xz)) {
        Clouds c = pc.scene.clouds;
        vec3 v = textureLod(textures[c.slabField], (xz - c.slabOrigin) / c.slabSize, 0.0).rgb;
        SlabColumn column;
        column.base = v.r + kSlabBakeOffset;
        column.top = v.g + kSlabBakeOffset;
        column.rim = v.b;
        column.layer = vec2(0.0);
        return column;
    }
    return slabColumn(xz, lod);
}

vec2 slabLayer(vec2 xz) {
    vec4 weather = weatherMapAt(vec3(xz.x, 0.0, xz.y));
    return cloudLayer(clamp(weather.g, 0.0, 1.0), max(weather.r, 1e-3));
}

// Signed distance (m, vertical) inside the column at altitude y: > 0
// inside.
float slabInside(SlabColumn column, float y) {
    return min(column.top - y, y - column.base);
}

// Density in [0, 1] at altitude y in the column.
float slabDensity(SlabColumn column, float y) {
    return smoothstep(0.0, kSlabSoftness.x, column.top - y) *
           smoothstep(0.0, kSlabSoftness.y, y - column.base) * smoothstep(0.0, kSlabRimSoftness, column.rim);
}

float slabDensityAt(vec3 world, float lod) {
    SlabColumn column = slabSample(world.xz, lod);
    return slabDensity(column, world.y);
}

// Lights a point inside a cloud (world xz flat, altitude on the shell):
// in-scattered luminance per unit sun illuminance, with the volumetric
// path's phase, octaves, diffuse multiple scattering and ambient.
vec3 slabLight(vec3 rel, vec3 world, SlabColumn column, float lod, float phases[kOctaves],
               vec3 ambient, vec3 ground, float ambientBottom, float radius) {
    vec3 sun = pc.scene.sunDirection;
    float bottomKm = pc.scene.atmosphere.bottomRadius;
    float h = pc.scene.cameraPosition.y;
    vec3 up = normalize(vec3(rel.x, radius + h + rel.y, rel.z));
    float sampleRadius = bottomKm + max(world.y * 0.001, kPlanetRadiusOffset);
    vec3 sunLight = transmittanceToSun(sampleRadius, dot(sun, up));
    if (raySphereIntersectNearest(up * sampleRadius, sun, bottomKm) >= 0.0) {
        sunLight = vec3(0.0);
    }
    float opticalDepth = 0.0;
    if (any(greaterThan(sunLight, vec3(0.0)))) {
        float previous = 0.0;
        for (int j = 0; j < 4; ++j) {
            float s = kSlabSunTaps[j];
            vec3 q = cloudPoint(rel + sun * s, radius);
            opticalDepth += slabDensityAt(q, lod + float(j) * 0.5) * (s - previous);
            previous = s;
        }
        opticalDepth *= kSlabExtinction;
        vec3 beyond = cloudPoint(rel + sun * kSlabSunTaps.w, radius);
        opticalDepth -= log(max(cloudShadow(beyond, true), 1e-6));
    }
    float scattered = 0.0;
    float a = 1.0;
    float b = 1.0;
    for (int k = 0; k < kOctaves; ++k) {
        scattered += b * exp(-opticalDepth * a) * phases[k];
        a *= kOctaveScale.x;
        b *= kOctaveScale.y;
    }
    scattered += kDiffuse.x / (4.0 * kPi * (1.0 + kDiffuse.y * opticalDepth));
    float heightFraction = clamp((world.y - column.layer.x) / (column.layer.y - column.layer.x), 0.0, 1.0);
    return kAlbedo * (sunLight * scattered + ambient * mix(ambientBottom, 1.0, heightFraction) +
                      ground * mix(1.0, kAmbientBottom.x, heightFraction));
}

// Conservative search for the first point inside a column from t: the
// bounds chain skips every cell the ray's altitudes over its crossing miss
// (climbing a level after each skip, descending where the bounds overlap),
// and the finest cells are sampled and bisected. Returns 1 with t at the
// entry, 0 when the ray reaches tEnd, 2 when it leaves the window (t there),
// 3 when the visit budget ran out (t advanced).
int slabTraverse(vec3 direction, inout float t, float tEnd, float radius) {
    Clouds c = pc.scene.clouds;
    float res = float(c.slabResolution);
    float texel = c.slabSize / res;
    vec2 origin = pc.scene.cameraPosition.xz - c.slabOrigin;
    vec2 dTexels = direction.xz / texel;
    int level = min(5, kSlabBakeLevels - 1);
    for (int i = 0; i < kSlabTraverseSteps; ++i) {
        if (t >= tEnd) {
            return 0;
        }
        vec2 p = origin / texel + dTexels * t;
        if (any(lessThan(p, vec2(0.5))) || any(greaterThanEqual(p, vec2(res - 0.5)))) {
            return 2;
        }
        float cellSize = float(1 << level);
        vec2 cell = floor(p / cellSize);
        vec2 edge = (cell + step(vec2(0.0), dTexels)) * cellSize;
        vec2 toEdge = mix(vec2(1e30), (edge - p) / dTexels, notEqual(dTexels, vec2(0.0)));
        float tExit = min(min(toEdge.x, toEdge.y) + t, tEnd);
        tExit = max(tExit, t);
        float ya = cloudPoint(direction * t, radius).y;
        float yb = cloudPoint(direction * tExit, radius).y;
        vec2 bounds = texelFetch(textures[c.slabBounds], ivec2(cell), level).rg + kSlabBakeOffset;
        if (max(ya, yb) + kSlabAltMargin < bounds.x || min(ya, yb) - kSlabAltMargin > bounds.y) {
            t = tExit + 0.05;
            level = min(level + 1, kSlabBakeLevels - 1);
            continue;
        }
        if (level > 0) {
            --level;
            continue;
        }
        float tPrevious = t;
        float dt = (tExit - t) / float(kSlabCellSteps);
        for (int k = 0; k <= kSlabCellSteps; ++k) {
            float tk = t + dt * float(k);
            vec3 q = cloudPoint(direction * tk, radius);
            if (slabInside(slabSample(q.xz, 0.0), q.y) > 0.0) {
                float t0 = tPrevious;
                float t1 = tk;
                if (k > 0) {
                    for (int j = 0; j < kSlabRefineSteps; ++j) {
                        float tm = 0.5 * (t0 + t1);
                        vec3 m = cloudPoint(direction * tm, radius);
                        if (slabInside(slabSample(m.xz, 0.0), m.y) > 0.0) {
                            t1 = tm;
                        } else {
                            t0 = tm;
                        }
                    }
                }
                t = t1;
                return 1;
            }
            tPrevious = tk;
        }
        t = tExit + 0.05;
        level = 1;
    }
    return 3;
}

// The slab along [tStart, tEnd] of a ray from the camera: search for the
// next cloud surface, refine it, integrate the chord behind it,
// repeat until opaque or past tEnd. ambient: the sky irradiance lighting
// the layer's top. maxSearch bounds the search (coarser for the sky
// capture).
CloudMarch marchSlab(vec3 direction, float tStart, float tEnd, int maxSearch, float minStep,
                     float stepGrowth, vec3 ambient) {
    CloudMarch result = CloudMarch(vec3(0.0), 1.0, 0.0);
    Clouds c = pc.scene.clouds;
    float radius = pc.scene.atmosphere.bottomRadius * 1000.0;
    vec3 sun = pc.scene.sunDirection;
    float cosTheta = dot(sun, direction);
    float phases[kOctaves];
    float eccentricity = 1.0;
    for (int k = 0; k < kOctaves; ++k) {
        phases[k] = dualLobePhase(cosTheta, eccentricity);
        eccentricity *= kOctaveScale.z;
    }
    float bottomKm = pc.scene.atmosphere.bottomRadius;
    vec3 groundSun = transmittanceToSun(bottomKm + kPlanetRadiusOffset, sun.y) * max(sun.y, 0.0);
    vec3 ground = pc.scene.atmosphere.groundAlbedo * (groundSun * (1.0 - c.groundCoverage) + ambient);
    ambient /= 2.0 * kPi;
    ground /= 2.0 * kPi;
    float ambientBottom = mix(kAmbientBottom.x, kAmbientBottom.y, c.groundCoverage);

    float depthSum = 0.0;
    float weightSum = 0.0;
    float t = tStart;
    float lod = max(0.0, log2(max(minStep, t * stepGrowth) / 100.0));
    vec3 world = cloudPoint(direction * t, radius);
    float inside = slabInside(slabSample(world.xz, lod), world.y);
    for (int i = 0; i < maxSearch && t < tEnd; ++i) {
        float tEntry = t;
        if (inside <= 0.0) {
            if (slabInWindow(pc.scene.cameraPosition.xz + direction.xz * t)) {
                int status = slabTraverse(direction, t, tEnd, radius);
                if (status != 1) {
                    world = cloudPoint(direction * t, radius);
                    inside = slabInside(slabSample(world.xz, lod), world.y);
                    continue;
                }
                tEntry = t;
            } else {
            // Search: the next step where the ray is inside a column.
            float dt = min(max(minStep, t * stepGrowth), tEnd - t);
            float tNext = t + dt;
            lod = max(0.0, log2(dt / 100.0));
            vec3 next = cloudPoint(direction * tNext, radius);
            float insideNext = slabInside(slabSample(next.xz, lod), next.y);
            if (insideNext <= 0.0) {
                t = tNext;
                inside = insideNext;
                continue;
            }
            // Bisection between the outside and inside samples: near a
            // cloud's steep side the vertical distance jumps, which stalls
            // a secant search (stair-step bands along the walls).
            float t0 = t;
            float t1 = tNext;
            for (int k = 0; k < kSlabRefineSteps; ++k) {
                float tm = 0.5 * (t0 + t1);
                vec3 m = cloudPoint(direction * tm, radius);
                if (slabInside(slabSample(m.xz, lod), m.y) > 0.0) {
                    t1 = tm;
                } else {
                    t0 = tm;
                }
            }
            tEntry = t1;
            }
        }
        // The chord's far end: step on from the entry until outside, then
        // bisect. Exact ends make the chord's opacity continuous across
        // pixels; fixed samples behind the entry striped thin rims seen
        // edge-on.
        float tIn = tEntry;
        float tExit = tEnd;
        float step = max(0.5 * minStep, 0.5 * tEntry * stepGrowth);
        for (int k = 0; k < kSlabExitSteps; ++k) {
            float tn = min(tIn + step, tEnd);
            vec3 q = cloudPoint(direction * tn, radius);
            if (slabInside(slabSample(q.xz, lod), q.y) <= 0.0) {
                float t0 = tIn;
                float t1 = tn;
                for (int j = 0; j < kSlabRefineSteps; ++j) {
                    float tm = 0.5 * (t0 + t1);
                    vec3 m = cloudPoint(direction * tm, radius);
                    if (slabInside(slabSample(m.xz, lod), m.y) > 0.0) {
                        t0 = tm;
                    } else {
                        t1 = tm;
                    }
                }
                tExit = t0;
                break;
            }
            tIn = tn;
            if (tn >= tEnd) {
                break;
            }
            step *= 1.5;
            tExit = tn;
        }
        // Mean density over the chord (midpoints), its opacity, lit once at
        // about half the opacity it reaches (a mean free path in, capped at
        // the middle).
        float chordLength = max(tExit - tEntry, 0.0);
        float meanDensity = 0.0;
        for (int k = 0; k < kSlabChordSamples; ++k) {
            vec3 q = cloudPoint(direction * (tEntry + chordLength * (float(k) + 0.5) / float(kSlabChordSamples)), radius);
            meanDensity += slabDensity(slabSample(q.xz, lod), q.y);
        }
        meanDensity /= float(kSlabChordSamples);
        float sigma = meanDensity * kSlabExtinction;
        float chordTransmittance = exp(-sigma * chordLength);
        if (chordTransmittance < 1.0) {
            float tLight = tEntry + min(0.5 * chordLength, 0.7 / max(sigma, 1e-6));
            vec3 rel = direction * tLight;
            vec3 p = cloudPoint(rel, radius);
            SlabColumn column = slabSample(p.xz, lod);
            column.layer = slabLayer(p.xz);
            vec3 light = slabLight(rel, p, column, lod, phases, ambient, ground, ambientBottom, radius);
            float opacity = result.transmittance * (1.0 - chordTransmittance);
            result.luminance += light * opacity;
            depthSum += tLight * opacity;
            weightSum += opacity;
            result.transmittance *= chordTransmittance;
        }
        float ts = max(tExit, tEntry) + 1.0;
        if (result.transmittance < kOpaque) {
            // Fully opaque: the sun disk behind (~6e4 exposed) must not
            // show through a residual 1%.
            result.transmittance = 0.0;
            break;
        }
        // Past the chord (or its last step inside a long deck): the search
        // resumes, or the next chord starts here.
        t = ts;
        world = cloudPoint(direction * t, radius);
        inside = slabInside(slabSample(world.xz, lod), world.y);
    }
    if (weightSum > 0.0) {
        result.distance = depthSum / weightSum;
    }
    return result;
}

#endif
