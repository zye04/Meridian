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
// the triangle with clip positions c0, c1, c2, at render size `size`: where
// the pixel's ray meets the triangle, k = adj(M) (nx, ny, 1) with M's columns
// the vertices' (x, y, w) and b = k / sum(k). No division by w, so a vertex
// behind the camera gives the visible part's weights.
Barycentrics barycentricsAt(vec4 c0, vec4 c1, vec4 c2, vec2 pixel, vec2 size) {
    vec3 x = vec3(c0.x, c1.x, c2.x);
    vec3 y = vec3(c0.y, c1.y, c2.y);
    vec3 w = vec3(c0.w, c1.w, c2.w);
    if (all(greaterThan(w, vec3(0.0)))) {
        // The rasterizer's vertices: snapped to its 1/256 px grid.
        vec3 sx = round((x / w * 0.5 + 0.5) * size.x * 256.0) / 256.0;
        vec3 sy = round((y / w * 0.5 + 0.5) * size.y * 256.0) / 256.0;
        x = (sx / size.x * 2.0 - 1.0) * w;
        y = (sy / size.y * 2.0 - 1.0) * w;
    }
    // Rows of adj(M): row i is the cross product of the other two columns.
    vec3 ax = y.yzx * w.zxy - w.yzx * y.zxy;
    vec3 ay = w.yzx * x.zxy - x.yzx * w.zxy;
    vec3 a1 = x.yzx * y.zxy - y.yzx * x.zxy;
    vec2 n = pixel / size * 2.0 - 1.0;
    vec2 dn = 2.0 / size;
    vec3 q = ax * n.x + ay * n.y + a1;
    float s = q.x + q.y + q.z;
    float sx = (ax.x + ax.y + ax.z) * dn.x;
    float sy = (ay.x + ay.y + ay.z) * dn.y;
    float invS = 1.0 / s;
    Barycentrics r;
    r.b = q * invS;
    r.dbdx = (ax * dn.x - r.b * sx) * invS;
    r.dbdy = (ay * dn.y - r.b * sy) * invS;
    // det(M): its sign is Vulkan's facing (negative NDC area is front).
    r.frontFacing = x.x * ax.x + y.x * ay.x + w.x * a1.x < 0.0;
    r.outside = max(0.0, -min(r.b.x, min(r.b.y, r.b.z)));
    return r;
}

#endif
