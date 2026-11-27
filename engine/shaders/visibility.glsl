// The visibility buffer's id and barycentrics: mirrors visibility.h /
// visibility.cpp (the CPU reference, tests/visibility_test.cpp) operation
// for operation.
#ifndef VISIBILITY_GLSL
#define VISIBILITY_GLSL

const uint kVisibilityNone = 0u;
const uint kVisibilityClusterTriangle = 1u;
const uint kVisibilityVoxel = 2u;
const uint kVisibilityTerrainSunPerPixel = 3u;

// terrainSunCode: the draw list entry's (0, 1 or 255).
uvec2 packClusterTriangle(uint instance, uint cluster, uint triangle, uint terrainSunCode) {
    uint sun = terrainSunCode > 1u ? kVisibilityTerrainSunPerPixel : terrainSunCode;
    return uvec2((instance & 0xFFFFFFu) | (triangle & 0x7Fu) << 24,
                 (cluster & 0x3FFFFFFu) | sun << 26 | kVisibilityClusterTriangle << 30);
}

uvec2 packVoxel(uint phase, uint index) {
    return uvec2(0u, (index & 0x3FFFFFFu) | (phase & 1u) << 26 | kVisibilityVoxel << 30);
}

uint visibilityKind(uvec2 id) { return id.y >> 30; }
uint visibilityInstance(uvec2 id) { return id.x & 0xFFFFFFu; }
uint visibilityTriangle(uvec2 id) { return (id.x >> 24) & 0x7Fu; }
uint visibilityCluster(uvec2 id) { return id.y & 0x3FFFFFFu; }
// 0, 1 or 255 (kTerrainSunPerFragment).
uint visibilityTerrainSunCode(uvec2 id) {
    uint sun = (id.y >> 26) & 3u;
    return sun == kVisibilityTerrainSunPerPixel ? 255u : sun;
}
uint visibilityVoxelIndex(uvec2 id) { return id.y & 0x3FFFFFFu; }
uint visibilityVoxelPhase(uvec2 id) { return (id.y >> 26) & 1u; }

struct Barycentrics {
    vec3 b;
    vec3 dbdx;
    vec3 dbdy;
    bool frontFacing;
    float outside;
};

// The pixel at framebuffer position `pixel` (y down, centres at + 0.5) in
// the triangle with clip positions c0, c1, c2, at render size `size`.
Barycentrics barycentricsAt(vec4 c0, vec4 c1, vec4 c2, vec2 pixel, vec2 size) {
    vec3 invW = 1.0 / vec3(c0.w, c1.w, c2.w);
    vec2 p0 = (c0.xy * invW.x * 0.5 + 0.5) * size;
    vec2 p1 = (c1.xy * invW.y * 0.5 + 0.5) * size;
    vec2 p2 = (c2.xy * invW.z * 0.5 + 0.5) * size;
    float area2 = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
    float invArea = 1.0 / area2;
    vec3 ax = vec3(p1.y - p2.y, p2.y - p0.y, p0.y - p1.y) * invArea;
    vec3 ay = vec3(p2.x - p1.x, p0.x - p2.x, p1.x - p0.x) * invArea;
    vec3 l = vec3((p1.x - pixel.x) * (p2.y - pixel.y) - (p2.x - pixel.x) * (p1.y - pixel.y),
                  (p2.x - pixel.x) * (p0.y - pixel.y) - (p0.x - pixel.x) * (p2.y - pixel.y),
                  (p0.x - pixel.x) * (p1.y - pixel.y) - (p1.x - pixel.x) * (p0.y - pixel.y)) *
             invArea;
    vec3 q = l * invW;
    float s = q.x + q.y + q.z;
    float sx = dot(ax, invW);
    float sy = dot(ay, invW);
    float invS = 1.0 / s;
    Barycentrics r;
    r.b = q * invS;
    r.dbdx = (ax * invW - r.b * sx) * invS;
    r.dbdy = (ay * invW - r.b * sy) * invS;
    r.frontFacing = area2 < 0.0;
    r.outside = max(0.0, -min(r.b.x, min(r.b.y, r.b.z)));
    return r;
}

#endif
