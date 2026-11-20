// The cloud march shared by the view march (clouds_march.comp) and the sky
// capture (sky_capture.comp): the layer's span along a ray from the camera
// and the lit march through it. Rays are straight in the displayed (curved)
// frame: the layer is a spherical shell around the planet center below the
// camera, matching the curvature warp. Include after common.glsl,
// atmosphere.glsl, weather.glsl and clouds.glsl.
#ifndef CLOUDS_MARCH_GLSL
#define CLOUDS_MARCH_GLSL

// Single-scattering albedo.
const float kAlbedo = 0.99;
// Dual-lobe Henyey-Greenstein: forward, backward, backward weight.
const vec3 kPhase = vec3(0.8, -0.3, 0.3);
// Wrenninge multiple-scattering octaves: per octave, extinction x a,
// contribution x b, eccentricity x c.
const int kOctaves = 4;
const vec3 kOctaveScale = vec3(0.5, 0.5, 0.5);
// Diffuse multiple scattering: light diffuses through a thick cloud with
// an algebraic, not exponential, falloff (two-stream: transmittance
// 1 / (1 + 0.75 (1 - g) tau)), so it dominates the octaves deep inside;
// isotropic, scaled by x, added everywhere (at the lit surface too).
const vec2 kDiffuse = vec2(0.8, 0.75 * (1.0 - 0.6));
// Ambient at the far side of the layer relative to the near side: the sky
// at the bottom, the ground bounce at the top; under full coverage the
// bases see other clouds, not the sky (x clear, y overcast).
const vec2 kAmbientBottom = vec2(0.35, 0.05);
// Ambient occlusion: one density tap kAmbientProbe (m) above the sample,
// its optical depth over the probe scaled by kAmbientOcclusion.
const float kAmbientProbe = 100.0;
const float kAmbientOcclusion = 0.5;
const float kMaxDistance = 300000.0;
const float kOpaque = 0.01;
// Sun march: steps doubling from kLightStep (6 steps cover 1.26 km).
const int kLightSteps = 6;
const float kLightStep = 20.0;

// Roots (near, far) of a ray from the camera (altitude h, direction with
// vertical component dy) against the sphere at altitude a, in m. Written to
// avoid the cancellation of |o|^2 - r^2 at planet scale (stable quadratic).
vec2 shellIntersect(float radius, float h, float dy, float a) {
    float b = (radius + h) * dy;
    float c = (h - a) * (2.0 * radius + h + a);
    float disc = b * b - c;
    if (disc < 0.0) {
        return vec2(-1.0);
    }
    float q = -b - (b >= 0.0 ? 1.0 : -1.0) * sqrt(disc);
    if (q == 0.0) {
        return vec2(0.0);
    }
    float t0 = q;
    float t1 = c / q;
    return vec2(min(t0, t1), max(t0, t1));
}

// World xz (flat, for the weather and noise) and altitude on the sphere of a
// point at `rel` from the camera.
vec3 cloudPoint(vec3 rel, float radius) {
    vec3 p = pc.scene.cameraPosition;
    float altitude = length(vec3(rel.x, radius + p.y + rel.y, rel.z)) - radius;
    return vec3(p.x + rel.x, altitude, p.z + rel.z);
}

float dualLobePhase(float cosTheta, float eccentricity) {
    return mix(henyeyGreensteinPhase(kPhase.x * eccentricity, -cosTheta),
               henyeyGreensteinPhase(kPhase.y * eccentricity, -cosTheta), kPhase.z);
}

// Transmittance between atmosphere-space points a and b on one ray
// (Bruneton 2017: a ratio of transmittances to the atmosphere's edge, taken
// upward so neither ray hits the ground).
float transmittanceBetween(vec3 a, vec3 b, vec3 direction) {
    float lb = length(b);
    vec3 ta;
    vec3 tb;
    if (dot(direction, b / lb) >= 0.0) {
        float la = length(a);
        ta = transmittanceToSun(la, dot(direction, a / la));
        tb = transmittanceToSun(lb, dot(direction, b / lb));
        return clamp(dot(ta, vec3(1.0 / 3.0)) / max(dot(tb, vec3(1.0 / 3.0)), 1e-6), 0.0, 1.0);
    }
    float la = length(a);
    ta = transmittanceToSun(la, dot(-direction, a / la));
    tb = transmittanceToSun(lb, dot(-direction, b / lb));
    return clamp(dot(tb, vec3(1.0 / 3.0)) / max(dot(ta, vec3(1.0 / 3.0)), 1e-6), 0.0, 1.0);
}

// The camera in atmosphere space (km, planet center at the origin, camera
// above it), as the haze lookups use it.
vec3 cloudCameraKm() {
    return vec3(0.0, pc.scene.atmosphere.bottomRadius +
                         max(pc.scene.cameraPosition.y * 0.001, kPlanetRadiusOffset), 0.0);
}

// Where the ray from the camera runs through the layer's envelope over all
// cloud types (cloudDensity is zero outside each sample's own layer), in m,
// ending at the ground; y <= x: it misses the layer.
vec2 cloudLayerSpan(vec3 direction) {
    Clouds c = pc.scene.clouds;
    float bottom = min(c.stratusBottom, c.cumulusBottom);
    float top = max(c.stratusTop, c.cumulusTop);
    float radius = pc.scene.atmosphere.bottomRadius * 1000.0;
    float h = pc.scene.cameraPosition.y;
    vec2 inner = shellIntersect(radius, h, direction.y, bottom);
    vec2 outer = shellIntersect(radius, h, direction.y, top);
    float tStart = 0.0;
    float tEnd = -1.0;
    if (h < bottom) {
        tStart = inner.y;
        tEnd = outer.y;
    } else if (h <= top) {
        tEnd = inner.x > 0.0 ? inner.x : outer.y;
    } else if (outer.y > 0.0) {
        tStart = max(outer.x, 0.0);
        tEnd = inner.x > 0.0 ? inner.x : outer.y;
    }
    vec2 ground = shellIntersect(radius, h, direction.y, 0.0);
    if (ground.x > 0.0) {
        tEnd = min(tEnd, ground.x);
    }
    return vec2(tStart, tEnd);
}

struct CloudMarch {
    // In-scattered luminance (sun illuminance 1), no haze.
    vec3 luminance;
    float transmittance;
    // Transmittance-weighted mean cloud distance (m); 0 = no cloud.
    float distance;
};

// Marches [tStart, tEnd] along the ray. Steps grow with distance
// (stepGrowth x t, at least minStep), widened only where the steps left
// couldn't reach the segment's end; at most maxSteps, stops when opaque.
// Each sample sits at the same jittered fraction of its own step.
// ambient: the sky irradiance lighting the layer's top.
CloudMarch marchClouds(vec3 direction, float tStart, float tEnd, float jitter, float minStep,
                       float stepGrowth, int maxSteps, vec3 ambient) {
    CloudMarch result = CloudMarch(vec3(0.0), 1.0, 0.0);
    Clouds c = pc.scene.clouds;
    float top = max(c.stratusTop, c.cumulusTop);
    float radius = pc.scene.atmosphere.bottomRadius * 1000.0;
    float h = pc.scene.cameraPosition.y;
    vec3 sun = pc.scene.sunDirection;
    float cosTheta = dot(sun, direction);
    float phases[kOctaves];
    float eccentricity = 1.0;
    for (int k = 0; k < kOctaves; ++k) {
        phases[k] = dualLobePhase(cosTheta, eccentricity);
        eccentricity *= kOctaveScale.z;
    }
    float bottomKm = pc.scene.atmosphere.bottomRadius;
    // Ground bounce: the ground (the atmosphere's albedo, Lambertian) lit
    // by the sun through the local coverage's gaps and by the sky.
    vec3 groundSun = transmittanceToSun(bottomKm + kPlanetRadiusOffset, sun.y) * max(sun.y, 0.0);
    vec3 ground = pc.scene.atmosphere.groundAlbedo * (groundSun * (1.0 - c.groundCoverage) + ambient);
    // Isotropic in-scattering of a uniform hemisphere of irradiance E: E / 2 pi.
    ambient /= 2.0 * kPi;
    float ambientBottom = mix(kAmbientBottom.x, kAmbientBottom.y, c.groundCoverage);
    ground /= 2.0 * kPi;

    float depthSum = 0.0;
    float weightSum = 0.0;
    float t = tStart;
    for (int i = 0; i < maxSteps && t < tEnd; ++i) {
        float dt = min(max(max(minStep, t * stepGrowth), (tEnd - t) / float(maxSteps - i)),
                       tEnd - t);
        float sampleT = t + jitter * dt;
        vec3 rel = direction * sampleT;
        vec3 world = cloudPoint(rel, radius);
        float lod = max(0.0, log2(dt / 100.0));
        float heightFraction;
        float density = cloudDensity(world, lod, heightFraction);
        if (density > 0.0) {
            // Sun color from the atmosphere at the sample, zero in the
            // planet's shadow.
            vec3 up = normalize(vec3(rel.x, radius + h + rel.y, rel.z));
            float sampleRadius = bottomKm + max(world.y * 0.001, kPlanetRadiusOffset);
            vec3 sunLight = transmittanceToSun(sampleRadius, dot(sun, up));
            if (raySphereIntersectNearest(up * sampleRadius, sun, bottomKm) >= 0.0) {
                sunLight = vec3(0.0);
            }

            // Sun: optical depth toward it through the layer (none to
            // march without sun; stops once above the layer), and from
            // where the short march ends to the sun, the shadow map's,
            // blended into the fallback's mean where the map fades (far
            // away, low sun).
            float opticalDepth = 0.0;
            if (any(greaterThan(sunLight, vec3(0.0)))) {
                float s = 0.0;
                float ds = kLightStep;
                bool exited = false;
                for (int j = 0; j < kLightSteps; ++j) {
                    vec3 q = cloudPoint(rel + sun * (s + 0.5 * ds), radius);
                    if (q.y > top) {
                        exited = true;
                        break;
                    }
                    opticalDepth += cloudDensity(q, lod + float(j) * 0.5) * ds;
                    s += ds;
                    ds *= 2.0;
                }
                opticalDepth *= kCloudExtinction;
                if (!exited) {
                    opticalDepth -= log(max(cloudShadow(cloudPoint(rel + sun * s, radius), true), 1e-6));
                }
            }

            // The cloud above shades crevices between billows and the
            // undersides, which the height gradient alone leaves smooth.
            float above = cloudDensity(cloudPoint(rel + up * kAmbientProbe, radius), lod);
            float ambientVisibility = exp(-above * kCloudExtinction * kAmbientProbe * kAmbientOcclusion);

            float scattered = 0.0;
            float a = 1.0;
            float b = 1.0;
            for (int k = 0; k < kOctaves; ++k) {
                scattered += b * exp(-opticalDepth * a) * phases[k];
                a *= kOctaveScale.x;
                b *= kOctaveScale.y;
            }
            scattered += kDiffuse.x / (4.0 * kPi * (1.0 + kDiffuse.y * opticalDepth));

            vec3 source = kAlbedo * (sunLight * scattered +
                                    ambient * mix(ambientBottom, 1.0, heightFraction) * ambientVisibility +
                                    ground * mix(1.0, kAmbientBottom.x, heightFraction));
            // Energy-conserving integration over the step (Hillaire 2016).
            float sigma = density * kCloudExtinction;
            float stepTransmittance = exp(-sigma * dt);
            float opacity = result.transmittance * (1.0 - stepTransmittance);
            result.luminance += source * opacity;
            depthSum += sampleT * opacity;
            weightSum += opacity;
            result.transmittance *= stepTransmittance;
            if (result.transmittance < kOpaque) {
                break;
            }
        }
        t += dt;
    }
    if (weightSum > 0.0) {
        result.distance = depthSum / weightSum;
    }
    return result;
}

// Cirrus: a thin sheet of ice crystals at kCirrusAltitude, a 2D layer (no
// volume, no shadow): fibrous streaks stretched along the wind and bent by
// a slow warp, in patches over part of the sky, from gradient noise. Its
// amount follows the local coverage (none in clear weather). The streaks
// turn with the wind direction, so their noise doesn't tile with the wind
// offset's period: the pattern jumps once per 262 km of wind travel.
const float kCirrusAltitude = 9000.0;
// Noise scales (m): streaks along and across the wind, the warp, patches.
const vec4 kCirrusScale = vec4(16384.0, 1024.0, 16384.0, 32768.0);
const float kCirrusOpacity = 0.6;
// Ice crystals scatter strongly forward.
const float kCirrusPhase = 0.75;

// The cirrus sheet along a ray from the camera: in-scattered luminance (sun
// illuminance 1), transmittance and distance (m; 0 = none).
CloudMarch cirrusLayer(vec3 direction, vec3 ambient) {
    CloudMarch result = CloudMarch(vec3(0.0), 1.0, 0.0);
    float h = pc.scene.cameraPosition.y;
    if (h >= kCirrusAltitude) {
        return result;
    }
    float radius = pc.scene.atmosphere.bottomRadius * 1000.0;
    float t = shellIntersect(radius, h, direction.y, kCirrusAltitude).y;
    if (t <= 0.0 || t > kMaxDistance) {
        return result;
    }
    vec3 world = cloudPoint(direction * t, radius);
    WeatherSample local = weatherAt(pc.scene, vec3(world.x, 0.0, world.z));
    float amount = smoothstep(0.05, 0.6, local.coverage);
    if (amount <= 0.0) {
        return result;
    }
    Clouds c = pc.scene.clouds;
    vec2 wind = normalize(pc.scene.wind.direction + vec2(1e-6, 0.0));
    vec2 p = world.xz - c.windOffset * 2.0;
    ivec3 period = ivec3(1 << 12);
    vec2 r = vec2(dot(p, wind), dot(p, vec2(-wind.y, wind.x)));
    r.y += 3000.0 * (gradientNoise(vec3(p / kCirrusScale.z, 0.5), period) - 0.5);
    float streaks = gradientOctaves(vec3(r / kCirrusScale.xy, 1.5), period, 3);
    float patches = gradientOctaves(vec3(p / kCirrusScale.w, 2.5), period, 2);
    float density = smoothstep(0.5, 0.72, streaks) * smoothstep(0.45, 0.6, patches) * amount;
    if (density <= 0.0) {
        return result;
    }
    // Slant path through the thin sheet: more opaque toward the horizon.
    float slant = min(1.0 / max(abs(direction.y), 0.05), 6.0);
    float opacity = 1.0 - exp(-kCirrusOpacity * density * slant);
    vec3 sun = pc.scene.sunDirection;
    vec3 up = normalize(vec3(direction.x * t, radius + h + direction.y * t, direction.z * t));
    vec3 sunLight = transmittanceToSun(pc.scene.atmosphere.bottomRadius + kCirrusAltitude * 0.001,
                                       dot(sun, up));
    float phase = mix(henyeyGreensteinPhase(kCirrusPhase, -dot(sun, direction)), 1.0 / (4.0 * kPi), 0.3);
    result.luminance = kAlbedo * (sunLight * phase + ambient / (2.0 * kPi)) * opacity;
    result.transmittance = 1.0 - opacity;
    result.distance = t;
    return result;
}

// The cirrus sheet behind a march of the low layer.
CloudMarch withCirrus(CloudMarch march, vec3 direction, vec3 ambient) {
    CloudMarch cirrus = cirrusLayer(direction, ambient);
    if (cirrus.distance <= 0.0 || march.transmittance < kOpaque) {
        return march;
    }
    march.luminance += march.transmittance * cirrus.luminance;
    if (march.distance <= 0.0) {
        march.distance = cirrus.distance;
    }
    march.transmittance *= cirrus.transmittance;
    return march;
}

#endif
