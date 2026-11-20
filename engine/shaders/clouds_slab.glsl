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

// Fraction of the type's layer height a full column reaches: cumulus wider
// than tall, as the loaves in the reference frames.
const float kSlabHeight = 0.4;
// The cut shape s in [0, 1] reaches the full height at 1 / kSlabRimWidth
// (a circular profile over that rim); kSlabTopHeap: the billow noise's
// relative swing of the thickness on cumulus tops.
const float kSlabRimWidth = 2.0;
const float kSlabTopHeap = 0.8;
// Rim fraction over which the density fades in.
const float kSlabRimSoftness = 0.3;
// The rim lifts above the flat base by this fraction of the missing
// thickness, so the edge of a cumulus curls up instead of ending in a
// knife-edge plate along the base.
const float kSlabRimLift = 0.15;
// Density ramps (m) inside the top and base surfaces: soft tops, a firmer
// base.
const vec2 kSlabSoftness = vec2(120.0, 40.0);
// Extinction (1/m) at density 1.
const float kSlabExtinction = 0.04;
// Billows: the detail volume at kBillowScale x its period carves the
// cumulus outline (scaled by type).
const float kSlabBillowErosion = 0.6;
// The noise slice advances with the weather time (wraps with it: an
// integer number of tiles per time period), so clouds form and dissolve.
const float kSlabSliceTiles = 16.0;
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

struct SlabColumn {
    float base;
    float top;
    // The type's layer (bottom, top), for the ambient height gradient.
    vec2 layer;
    // 0 at the rim to 1 where the full height is reached: the density
    // fades in over the rim, so thin rims are translucent, not walls.
    float rim;
};

// The column at world xz; lod: the noise mip for the search step (coarser
// far away, so features finer than a step blur instead of aliasing).
SlabColumn slabColumn(vec2 xz, float lod) {
    SlabColumn column;
    vec4 weather = weatherMapAt(vec3(xz.x, 0.0, xz.y));
    float coverage = weather.r;
    float type = clamp(weather.g, 0.0, 1.0);
    column.layer = cloudLayer(type, max(coverage, 1e-3));
    column.base = column.layer.x;
    column.top = column.layer.x;
    column.rim = 0.0;
    if (coverage <= 0.0) {
        return column;
    }
    Clouds c = pc.scene.clouds;
    vec2 p = xz - c.windOffset;
    float slice = c.time * kSlabSliceTiles / c.timePeriod;
    vec4 base = textureLod(textures3D[c.baseNoise], vec3(p / c.baseNoisePeriod, slice), lod);
    float worley = dot(base.gba, vec3(0.625, 0.25, 0.125));
    float shape = remap(base.r, worley - 1.0, 1.0, 0.0, 1.0);
    shape = clamp(remap(shape, kShapeRange.x, kShapeRange.y, 0.0, 1.0), 0.0, 1.0);
    // Billow texels are 4x the detail's: two mips fewer for the same step.
    vec3 billowNoise = textureLod(textures3D[c.detailNoise],
                                  vec3(p / (c.detailNoisePeriod * kBillowScale), slice * kBillowScale),
                                  max(0.0, lod - 2.0)).rgb;
    float billows = 1.0 - dot(billowNoise, vec3(0.5, 0.3, 0.2));
    shape = clamp(remap(shape, billows * kSlabBillowErosion * type, 1.0, 0.0, 1.0), 0.0, 1.0);
    shape = mix(shape, 1.0, kStratusShapeFloor * (1.0 - type));
    float cut = mix(kShapeCut.x, kShapeCut.y, type);
    float s = coverage * clamp(remap(shape, cut, 1.0, 0.0, 1.0), 0.0, 1.0);
    // Circular profile over the rim (s up to 1 / kSlabRimWidth): steep
    // sides and a flat-topped loaf; heaped tops from the billow noise,
    // modulating the thickness by a fraction of itself only, since
    // high-frequency height on a height field reads as vertical streaks
    // from the side.
    column.rim = min(s * kSlabRimWidth, 1.0);
    float r = 1.0 - column.rim;
    float dome = sqrt(max(1.0 - r * r, 0.0));
    float heap = 1.0 + kSlabTopHeap * type * (billows - 0.5);
    float height = kSlabHeight * (column.layer.y - column.layer.x);
    float thickness = height * dome * heap * (s > 0.0 ? 1.0 : 0.0);
    column.base = column.layer.x + kSlabRimLift * (height - thickness);
    column.top = column.base + thickness;
    return column;
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
    SlabColumn column = slabColumn(world.xz, lod);
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
    float inside = slabInside(slabColumn(world.xz, lod), world.y);
    for (int i = 0; i < maxSearch && t < tEnd; ++i) {
        float tEntry = t;
        if (inside <= 0.0) {
            // Search: the next step where the ray is inside a column.
            float dt = min(max(minStep, t * stepGrowth), tEnd - t);
            float tNext = t + dt;
            lod = max(0.0, log2(dt / 100.0));
            vec3 next = cloudPoint(direction * tNext, radius);
            float insideNext = slabInside(slabColumn(next.xz, lod), next.y);
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
                if (slabInside(slabColumn(m.xz, lod), m.y) > 0.0) {
                    t1 = tm;
                } else {
                    t0 = tm;
                }
            }
            tEntry = t1;
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
            if (slabInside(slabColumn(q.xz, lod), q.y) <= 0.0) {
                float t0 = tIn;
                float t1 = tn;
                for (int j = 0; j < kSlabRefineSteps; ++j) {
                    float tm = 0.5 * (t0 + t1);
                    vec3 m = cloudPoint(direction * tm, radius);
                    if (slabInside(slabColumn(m.xz, lod), m.y) > 0.0) {
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
            meanDensity += slabDensity(slabColumn(q.xz, lod), q.y);
        }
        meanDensity /= float(kSlabChordSamples);
        float sigma = meanDensity * kSlabExtinction;
        float chordTransmittance = exp(-sigma * chordLength);
        if (chordTransmittance < 1.0) {
            float tLight = tEntry + min(0.5 * chordLength, 0.7 / max(sigma, 1e-6));
            vec3 rel = direction * tLight;
            vec3 p = cloudPoint(rel, radius);
            SlabColumn column = slabColumn(p.xz, lod);
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
        inside = slabInside(slabColumn(world.xz, lod), world.y);
    }
    if (weightSum > 0.0) {
        result.distance = depthSum / weightSum;
    }
    return result;
}

#endif
