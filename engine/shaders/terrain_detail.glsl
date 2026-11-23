// The terrain's procedural detail displacement, shared by terrain.mesh and
// loadInstance (common.glsl, which includes this after the Terrain struct):
// streamed plants are lifted by the same displacement the raster terrain
// gets, so they sit on the displayed ground. Needs only the Terrain struct.
#ifndef TERRAIN_DETAIL_GLSL
#define TERRAIN_DETAIL_GLSL

// Procedural detail displacement on the two levels finer than the data:
// at most this many meters either way.
const float kTerrainDetailAmplitude = 0.3;

float terrainLodRange(Terrain t, uint level) {
    return t.lodRange0 * float(1u << level);
}

// Detail displacement weight: 1 within level 0's range, 0 from the end of
// level 1's, so level >= 2 vertices are exactly the heightfield.
float terrainDetailWeight(Terrain t, float distanceToCamera) {
    return t.detailScale * (1.0 - smoothstep(terrainLodRange(t, 0u), terrainLodRange(t, 1u),
                                             distanceToCamera));
}

float terrainHash(ivec2 cell, uint seed) {
    uint h = uint(cell.x) * 374761393u + uint(cell.y) * 668265263u + seed * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(h & 0xFFFFFFu) / float(0xFFFFFFu);
}

// Value noise in [-1, 1] and its gradient (Quilez, "Value noise
// derivatives").
vec3 terrainValueNoise(vec2 p, uint seed) {
    vec2 cell = floor(p);
    vec2 f = p - cell;
    vec2 u = f * f * (3.0 - 2.0 * f);
    vec2 du = 6.0 * f * (1.0 - f);
    ivec2 c = ivec2(cell);
    float a = terrainHash(c, seed);
    float b = terrainHash(c + ivec2(1, 0), seed);
    float d = terrainHash(c + ivec2(0, 1), seed);
    float e = terrainHash(c + ivec2(1, 1), seed);
    float k1 = b - a;
    float k2 = d - a;
    float k3 = a - b - d + e;
    float value = a + k1 * u.x + k2 * u.y + k3 * u.x * u.y;
    vec2 gradient = du * vec2(k1 + k3 * u.y, k2 + k3 * u.x);
    return vec3(value, gradient) * 2.0 - vec3(1.0, 0.0, 0.0);
}

// Detail displacement at world (x, z): height (m) and its x/z gradient.
// Wavelengths 4, 2, 1 and 0.5 m; the amplitudes sum to
// kTerrainDetailAmplitude.
vec3 terrainDetail(vec2 p) {
    const float kAmplitudes[4] = float[4](0.15, 0.08, 0.045, 0.025);
    vec3 sum = vec3(0.0);
    float frequency = 0.25;
    for (uint octave = 0u; octave < 4u; ++octave) {
        vec3 n = terrainValueNoise(p * frequency, octave);
        sum += kAmplitudes[octave] * vec3(n.x, n.yz * frequency);
        frequency *= 2.0;
    }
    return sum;
}

#endif
