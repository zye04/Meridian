// Physically based sky and atmosphere: a GLSL port of Sebastien Hillaire,
// "A Scalable and Production Ready Sky and Atmosphere Rendering Technique"
// (EGSR 2020), following his reference implementation
// github.com/sebh/UnrealEngineSkyAtmosphere (MIT, Copyright 2020 Epic Games).
// Changes from the reference: Y is up instead of Z, and all lookup tables are
// computed with a sun illuminance of 1 and scaled by the real sun at use.
//
// Distances are in kilometers. The planet center is at the origin of
// "atmosphere space"; world positions convert with atmospherePosition().

#ifndef ATMOSPHERE_GLSL
#define ATMOSPHERE_GLSL

const float kPi = 3.14159265;

// Keeps sample points just above the ground to avoid precision artifacts.
const float kPlanetRadiusOffset = 0.01;

const vec2 kTransmittanceLutSize = vec2(256.0, 64.0);
const float kMultiScatteringLutSize = 32.0;
const vec2 kSkyViewLutSize = vec2(192.0, 108.0);
const float kAerialPerspectiveSlices = 32.0;
const float kAerialPerspectiveKmPerSlice = 4.0;
// The slice of the first froxel's centre (the LUT's slices are quadratic).
const float kAerialPerspectiveFirstSlice = 0.5 * 0.5 / kAerialPerspectiveSlices;

layout(set = 0, binding = 0) uniform sampler2D transmittanceLut;
layout(set = 0, binding = 1) uniform sampler2D multiScatteringLut;
layout(set = 0, binding = 2) uniform sampler2D skyViewLut;
layout(set = 0, binding = 3) uniform sampler3D aerialPerspectiveLut;

// World units are meters with y = 0 at sea level.
vec3 atmospherePosition(vec3 worldPosition) {
    vec3 p = worldPosition * 0.001;
    p.y = max(p.y, kPlanetRadiusOffset);
    return p + vec3(0.0, pc.scene.atmosphere.bottomRadius, 0.0);
}

float raySphereIntersectNearest(vec3 origin, vec3 direction, float radius) {
    float b = 2.0 * dot(direction, origin);
    float c = dot(origin, origin) - radius * radius;
    float delta = b * b - 4.0 * c;
    if (delta < 0.0) {
        return -1.0;
    }
    float sol0 = (-b - sqrt(delta)) * 0.5;
    float sol1 = (-b + sqrt(delta)) * 0.5;
    if (sol0 < 0.0 && sol1 < 0.0) {
        return -1.0;
    }
    if (sol0 < 0.0) {
        return max(0.0, sol1);
    }
    if (sol1 < 0.0) {
        return max(0.0, sol0);
    }
    return max(0.0, min(sol0, sol1));
}

float fromUnitToSubUvs(float u, float resolution) {
    return (u + 0.5 / resolution) * (resolution / (resolution + 1.0));
}

float fromSubUvsToUnit(float u, float resolution) {
    return (u - 0.5 / resolution) * (resolution / (resolution - 1.0));
}

struct Medium {
    vec3 scattering;
    vec3 extinction;
    vec3 scatteringMie;
    vec3 scatteringRayleigh;
};

Medium sampleMedium(vec3 position) {
    AtmosphereParameters atmosphere = pc.scene.atmosphere;
    float height = length(position) - atmosphere.bottomRadius;

    float densityMie =
        exp(atmosphere.mieDensityExpScale * height) *
            (1.0 - smoothstep(atmosphere.mieLayerLow, atmosphere.mieLayerHigh, height)) +
        atmosphere.mieBackground * exp(atmosphere.mieBackgroundExpScale * height);
    float densityRayleigh = exp(atmosphere.rayleighDensityExpScale * height);
    float densityOzone = clamp(
        height < atmosphere.absorptionLayerWidth
            ? atmosphere.absorption0Linear * height + atmosphere.absorption0Constant
            : atmosphere.absorption1Linear * height + atmosphere.absorption1Constant,
        0.0, 1.0);

    Medium m;
    m.scatteringMie = densityMie * atmosphere.mieScattering;
    m.scatteringRayleigh = densityRayleigh * atmosphere.rayleighScattering;
    m.scattering = m.scatteringMie + m.scatteringRayleigh;
    m.extinction = densityMie * atmosphere.mieExtinction + m.scatteringRayleigh +
                   densityOzone * atmosphere.absorptionExtinction;
    return m;
}

float rayleighPhase(float cosTheta) {
    return 3.0 / (16.0 * kPi) * (1.0 + cosTheta * cosTheta);
}

float henyeyGreensteinPhase(float g, float cosTheta) {
    float numerator = 1.0 - g * g;
    float denominator = 1.0 + g * g + 2.0 * g * cosTheta;
    return numerator / (4.0 * kPi * denominator * sqrt(denominator));
}

// Transmittance LUT parameterization (Bruneton 2017).
vec2 transmittanceLutUv(float viewHeight, float viewZenithCos) {
    AtmosphereParameters atmosphere = pc.scene.atmosphere;
    float top2 = atmosphere.topRadius * atmosphere.topRadius;
    float bottom2 = atmosphere.bottomRadius * atmosphere.bottomRadius;
    float H = sqrt(max(0.0, top2 - bottom2));
    float rho = sqrt(max(0.0, viewHeight * viewHeight - bottom2));

    float discriminant =
        viewHeight * viewHeight * (viewZenithCos * viewZenithCos - 1.0) + top2;
    float d = max(0.0, -viewHeight * viewZenithCos + sqrt(discriminant));
    float dMin = atmosphere.topRadius - viewHeight;
    float dMax = rho + H;
    return vec2((d - dMin) / (dMax - dMin), rho / H);
}

void transmittanceLutParameters(vec2 uv, out float viewHeight,
                                out float viewZenithCos) {
    AtmosphereParameters atmosphere = pc.scene.atmosphere;
    float bottom2 = atmosphere.bottomRadius * atmosphere.bottomRadius;
    float H = sqrt(atmosphere.topRadius * atmosphere.topRadius - bottom2);
    float rho = H * uv.y;
    viewHeight = sqrt(rho * rho + bottom2);

    float dMin = atmosphere.topRadius - viewHeight;
    float dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    viewZenithCos = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * viewHeight * d);
    viewZenithCos = clamp(viewZenithCos, -1.0, 1.0);
}

vec3 transmittanceToSun(float height, float sunZenithCos) {
    return textureLod(transmittanceLut, transmittanceLutUv(height, sunZenithCos), 0.0).rgb;
}

vec3 multipleScattering(vec3 position, float sunZenithCos) {
    AtmosphereParameters atmosphere = pc.scene.atmosphere;
    vec2 uv = clamp(vec2(sunZenithCos * 0.5 + 0.5,
                         (length(position) - atmosphere.bottomRadius) /
                             (atmosphere.topRadius - atmosphere.bottomRadius)),
                    0.0, 1.0);
    uv = vec2(fromUnitToSubUvs(uv.x, kMultiScatteringLutSize),
              fromUnitToSubUvs(uv.y, kMultiScatteringLutSize));
    return textureLod(multiScatteringLut, uv, 0.0).rgb;
}

// The sky-view LUT is indexed by view zenith (non-linear, packing detail
// near the horizon) and by the view azimuth relative to the sun.
vec2 skyViewLutUv(bool intersectsGround, float viewZenithCos,
                  float lightViewCos, float viewHeight) {
    AtmosphereParameters atmosphere = pc.scene.atmosphere;
    float horizon = sqrt(viewHeight * viewHeight -
                         atmosphere.bottomRadius * atmosphere.bottomRadius);
    float beta = acos(horizon / viewHeight);
    float zenithHorizonAngle = kPi - beta;

    vec2 uv;
    if (!intersectsGround) {
        float coord = acos(viewZenithCos) / zenithHorizonAngle;
        coord = 1.0 - sqrt(1.0 - coord);
        uv.y = coord * 0.5;
    } else {
        float coord = (acos(viewZenithCos) - zenithHorizonAngle) / beta;
        uv.y = sqrt(coord) * 0.5 + 0.5;
    }
    uv.x = sqrt(-lightViewCos * 0.5 + 0.5);
    return vec2(fromUnitToSubUvs(uv.x, kSkyViewLutSize.x),
                fromUnitToSubUvs(uv.y, kSkyViewLutSize.y));
}

void skyViewLutParameters(vec2 uv, float viewHeight, out float viewZenithCos,
                          out float lightViewCos) {
    AtmosphereParameters atmosphere = pc.scene.atmosphere;
    uv = vec2(fromSubUvsToUnit(uv.x, kSkyViewLutSize.x),
              fromSubUvsToUnit(uv.y, kSkyViewLutSize.y));

    float horizon = sqrt(viewHeight * viewHeight -
                         atmosphere.bottomRadius * atmosphere.bottomRadius);
    float beta = acos(horizon / viewHeight);
    float zenithHorizonAngle = kPi - beta;

    if (uv.y < 0.5) {
        float coord = 1.0 - 2.0 * uv.y;
        coord = 1.0 - coord * coord;
        viewZenithCos = cos(zenithHorizonAngle * coord);
    } else {
        float coord = uv.y * 2.0 - 1.0;
        viewZenithCos = cos(zenithHorizonAngle + beta * coord * coord);
    }
    float coord = uv.x * uv.x;
    lightViewCos = -(coord * 2.0 - 1.0);
}

// Sky luminance seen from atmosphere-space position `camera` toward
// `direction` (sun illuminance 1, without the sun disk).
vec3 skyLuminance(vec3 camera, vec3 direction) {
    float viewHeight = length(camera);
    vec3 up = camera / viewHeight;
    vec3 sun = pc.scene.sunDirection;
    vec3 side = normalize(cross(up, direction));
    vec3 forward = normalize(cross(side, up));
    vec2 lightOnPlane = normalize(vec2(dot(sun, forward), dot(sun, side)) + 1e-6);
    bool intersectsGround = raySphereIntersectNearest(
        camera, direction, pc.scene.atmosphere.bottomRadius) >= 0.0;
    vec2 uv = skyViewLutUv(intersectsGround, dot(direction, up), lightOnPlane.x,
                           viewHeight);
    return textureLod(skyViewLut, uv, 0.0).rgb;
}

// Moves a point outside the atmosphere onto its boundary along the ray.
bool moveToTopAtmosphere(inout vec3 position, vec3 direction) {
    float height = length(position);
    float topRadius = pc.scene.atmosphere.topRadius;
    if (height > topRadius) {
        float tTop = raySphereIntersectNearest(position, direction, topRadius);
        if (tTop < 0.0) {
            return false;
        }
        position += direction * tTop - (position / height) * kPlanetRadiusOffset;
    }
    return true;
}

struct ScatteringResult {
    vec3 luminance;
    vec3 opticalDepth;
    vec3 transmittance;
    vec3 multiScatteringAs1;
};

// Ray marches single scattering (plus the multiple-scattering LUT term)
// along a ray, with the sun illuminance taken as 1.
// useLuts: false while building the transmittance and multiple-scattering
// LUTs themselves.
ScatteringResult integrateScatteredLuminance(
    vec3 position, vec3 direction, vec3 sunDirection, float sampleCount,
    bool variableSampleCount, bool mieRayleighPhase, bool useMultiScattering,
    bool groundBounce, float tMaxLimit) {
    AtmosphereParameters atmosphere = pc.scene.atmosphere;
    ScatteringResult result;
    result.luminance = vec3(0.0);
    result.opticalDepth = vec3(0.0);
    result.transmittance = vec3(1.0);
    result.multiScatteringAs1 = vec3(0.0);

    float tBottom = raySphereIntersectNearest(position, direction, atmosphere.bottomRadius);
    float tTop = raySphereIntersectNearest(position, direction, atmosphere.topRadius);
    float tMax = 0.0;
    if (tBottom < 0.0) {
        if (tTop < 0.0) {
            return result;
        }
        tMax = tTop;
    } else if (tTop > 0.0) {
        tMax = min(tTop, tBottom);
    }
    tMax = min(tMax, tMaxLimit);

    float sampleCountFloor = sampleCount;
    float tMaxFloor = tMax;
    if (variableSampleCount) {
        sampleCount = mix(4.0, 14.0, clamp(tMax * 0.01, 0.0, 1.0));
        sampleCountFloor = floor(sampleCount);
        tMaxFloor = tMax * sampleCountFloor / sampleCount;
    }

    float cosTheta = dot(sunDirection, direction);
    float miePhase = henyeyGreensteinPhase(atmosphere.miePhaseG, -cosTheta);
    float rayleighPhaseValue = rayleighPhase(cosTheta);
    const float uniformPhase = 1.0 / (4.0 * kPi);

    vec3 throughput = vec3(1.0);
    float t = 0.0;
    float dt = tMax / sampleCount;
    const float kSampleSegmentT = 0.3;
    for (float s = 0.0; s < sampleCount; s += 1.0) {
        if (variableSampleCount) {
            float t0 = s / sampleCountFloor;
            float t1 = (s + 1.0) / sampleCountFloor;
            t0 = tMaxFloor * t0 * t0;
            t1 = t1 * t1 > 1.0 ? tMax : tMaxFloor * t1 * t1;
            t = t0 + (t1 - t0) * kSampleSegmentT;
            dt = t1 - t0;
        } else {
            float newT = tMax * (s + kSampleSegmentT) / sampleCount;
            dt = newT - t;
            t = newT;
        }
        vec3 p = position + t * direction;

        Medium medium = sampleMedium(p);
        vec3 sampleOpticalDepth = medium.extinction * dt;
        vec3 sampleTransmittance = exp(-sampleOpticalDepth);
        result.opticalDepth += sampleOpticalDepth;

        float height = length(p);
        vec3 up = p / height;
        float sunZenithCos = dot(sunDirection, up);
        vec3 sunTransmittance = transmittanceToSun(height, sunZenithCos);

        vec3 phaseTimesScattering = mieRayleighPhase
            ? medium.scatteringMie * miePhase + medium.scatteringRayleigh * rayleighPhaseValue
            : medium.scattering * uniformPhase;

        float tEarth = raySphereIntersectNearest(
            p - kPlanetRadiusOffset * up, sunDirection, atmosphere.bottomRadius);
        float earthShadow = tEarth >= 0.0 ? 0.0 : 1.0;

        vec3 multiScattered = useMultiScattering
                                  ? multipleScattering(p, sunZenithCos)
                                  : vec3(0.0);

#ifdef SUN_VISIBILITY
        earthShadow *= SUN_VISIBILITY(p);
#endif
        vec3 S = earthShadow * sunTransmittance * phaseTimesScattering +
                 multiScattered * medium.scattering;

        // Analytic integration over the step (Hillaire 2015, Frostbite).
        vec3 safeExtinction = max(medium.extinction, vec3(1e-9));
        vec3 multiScatteringIntegral =
            (medium.scattering - medium.scattering * sampleTransmittance) / safeExtinction;
        result.multiScatteringAs1 += throughput * multiScatteringIntegral;

        vec3 Sint = (S - S * sampleTransmittance) / safeExtinction;
        result.luminance += throughput * Sint;
        throughput *= sampleTransmittance;
    }

    if (groundBounce && tMax == tBottom && tBottom > 0.0) {
        vec3 p = position + tBottom * direction;
        float height = length(p);
        vec3 up = p / height;
        float sunZenithCos = dot(sunDirection, up);
        float NdotL = clamp(dot(up, sunDirection), 0.0, 1.0);
        result.luminance += transmittanceToSun(height, sunZenithCos) * throughput *
                            NdotL * atmosphere.groundAlbedo / kPi;
    }

    result.transmittance = throughput;
    return result;
}

#endif
