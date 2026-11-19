// The global wind (Wind in common.glsl), mirrored from weather.cpp. Altitude
// in meters above the ground (vegetation) or sea level (clouds). Consumers
// evaluate these with scene.wind and scene.previousWind for motion vectors.

// Gusts: noise cells of this size (m), wrapping along the phase at
// w.gustPhasePeriod.
const float kGustCell = 40.0;

// Clockwise seen from above, like the Ekman spiral (veer), relative to the
// 10 m wind.
vec2 windVeered(Wind w, vec2 v, float altitude) {
    float angle = w.veer * clamp((altitude - 10.0) / (w.veerAltitude - 10.0), 0.0, 1.0);
    float c = cos(angle);
    float s = sin(angle);
    return vec2(v.x * c - v.y * s, v.x * s + v.y * c);
}

float windProfileScale(Wind w, float altitude) {
    return pow(max(altitude, 1.0) / 10.0, w.shearExponent);
}

vec2 windVelocity(Wind w, float altitude) {
    return windVeered(w, w.direction * w.speed10m, altitude) * windProfileScale(w, altitude);
}

// The time integral of windVelocity at this altitude: scroll anything carried
// by the wind by it, so speed changes never make it jump.
vec2 windOffset(Wind w, float altitude) {
    return windVeered(w, w.offset, altitude) * windProfileScale(w, altitude);
}

float gustHash(ivec3 p) {
    uint h = uint(p.x) * 0x8da6b343u ^ uint(p.y) * 0xd8163841u ^ uint(p.z) * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return float(h) * (1.0 / 4294967295.0);
}

// Extra fraction of the wind speed, in [0, gustiness]: value noise carried
// by the 10 m wind, evolving along the gust phase.
float gust(Wind w, vec2 worldXZ) {
    vec3 p = vec3((worldXZ - w.offset) / kGustCell, w.gustPhase);
    ivec3 i = ivec3(floor(p));
    vec3 f = p - vec3(i);
    f = f * f * (3.0 - 2.0 * f);
    int period = int(w.gustPhasePeriod);
    int z0 = i.z % period;
    int z1 = (i.z + 1) % period;
    float n0 = mix(mix(gustHash(ivec3(i.xy, z0)), gustHash(ivec3(i.x + 1, i.y, z0)), f.x),
                   mix(gustHash(ivec3(i.x, i.y + 1, z0)), gustHash(ivec3(i.xy + 1, z0)), f.x), f.y);
    float n1 = mix(mix(gustHash(ivec3(i.xy, z1)), gustHash(ivec3(i.x + 1, i.y, z1)), f.x),
                   mix(gustHash(ivec3(i.x, i.y + 1, z1)), gustHash(ivec3(i.xy + 1, z1)), f.x), f.y);
    return w.gustiness * mix(n0, n1, f.z);
}
