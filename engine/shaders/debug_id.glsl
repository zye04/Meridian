#ifndef DEBUG_ID_GLSL
#define DEBUG_ID_GLSL

// Scene attachment 7 (R32G32B32A32_UINT): what each pixel shows, for the
// debug views and the report's pixel pick (Renderer::pick). Only the
// DEBUG_ID shader variants write it, in the scene pipelines made while a
// debug view or the report is on (createScenePipelines); otherwise the
// writes compile away and the pass has seven attachments, as without it.
// x: kind << 24 | instance id, y: cluster index (kDebugTriangles,
// kDebugVoxels) or far list entry (kDebugFarCell), z: distance from the
// camera (float bits), w: material index (~0 where none).
// Must match PickKind in debug_view.h.
const uint kDebugSky = 0u;
const uint kDebugTriangles = 1u;
const uint kDebugVoxels = 2u;
const uint kDebugFarCell = 3u;
const uint kDebugTerrain = 4u;

// SceneData.debugFilter: mode << 30 | kDebugFilterMaterial (else an
// instance) | target. Must match DebugFilter in debug_view.h.
const uint kDebugFilterSolo = 1u;
const uint kDebugFilterHide = 2u;
const uint kDebugFilterMaterial = 1u << 29u;

// Whether solo/hide removes a surface (instance ~0u and material ~0u where
// it has none: terrain, far cells, voxels' material).
bool debugFilterRejects(uint code, uint instance, uint material) {
    uint mode = code >> 30u;
    if (mode == 0u) {
        return false;
    }
    uint target = code & (kDebugFilterMaterial - 1u);
    bool match = (code & kDebugFilterMaterial) != 0u ? material == target
                                                       : (instance & 0xFFFFFFu) == target;
    return mode == kDebugFilterSolo ? !match : match;
}

// The texture's stretch on the surface at a pixel, from screen derivatives:
// the texel map (texels per metre) in the surface plane, its two singular
// values s1 >= s2. Returns (sqrt(s1 s2) texels/m, negated on the odd squares
// of an 8 x 8 checker over the texture; s1 / s2), 0 where unknown.
vec2 textureStretch(vec2 uv, vec2 duvdx, vec2 duvdy, vec3 dpdx, vec3 dpdy, vec2 size) {
    float a = length(dpdx);
    if (a < 1e-12 || size.x <= 0.0) {
        return vec2(0.0);
    }
    vec3 e1 = dpdx / a;
    float b = dot(dpdy, e1);
    float c = length(dpdy - b * e1);
    if (c < 1e-12) {
        return vec2(0.0);
    }
    // Screen x is (a, 0) in the plane's (e1, e2), screen y (b, c).
    mat2 texels = mat2(duvdx * size, duvdy * size) * inverse(mat2(a, 0.0, b, c));
    float e = dot(texels[0], texels[0]) + dot(texels[1], texels[1]);
    float d = abs(determinant(texels));
    float s1 = sqrt(0.5 * (e + sqrt(max(e * e - 4.0 * d * d, 0.0))));
    float s2 = d / max(s1, 1e-12);
    ivec2 square = ivec2(floor(uv * 8.0));
    float parity = ((square.x + square.y) & 1) != 0 ? -1.0 : 1.0;
    return vec2(parity * sqrt(s1 * s2), s1 / max(s2, 1e-6));
}

#ifndef DEBUG_ID_CONSTANTS_ONLY
#ifdef DEBUG_ID
layout(location = 7) out uvec4 outDebugId;
#if defined(DEBUG_ID_MESH) && !defined(TERRAIN)
// (instance id, cluster index) from mesh.mesh.
layout(location = 13) flat in uvec2 inDebugId;
#endif
#endif

void writeDebugId(uint kind, uint instance, uint detail, vec3 relative, uint material) {
#ifdef DEBUG_ID
    outDebugId = uvec4(kind << 24u | (instance & 0xFFFFFFu), detail,
                       floatBitsToUint(length(relative)), material);
#endif
}

// The pixel's light split, exposed (x sunIlluminance x exposure x haze, as
// outColor): sun and ambient (sky, ground, probes), each diffuse and
// specular, and the texture stretch. Two uvec4 per render pixel: depth,
// halves (sun diffuse rgb, sun specular rgb), (ambient diffuse rgb,
// ambient specular rgb, stretch). Call between begin/endInvocationInterlockARB
// in main().
void writeDebugLight(float depth, vec3 sunDiffuse, vec3 sunSpecular, vec3 ambientDiffuse,
                     vec3 ambientSpecular, vec2 stretch) {
#ifdef DEBUG_ID
    uint stride = pc.scene.debugLightStride;
    if (stride == 0u) {
        return;
    }
    uint i = (uint(gl_FragCoord.y) * stride + uint(gl_FragCoord.x)) * 2u;
    if (depth < uintBitsToFloat(pc.scene.debugLight.pixels[i].x)) {
        return;
    }
    const float kMax = 65000.0;
    sunDiffuse = min(sunDiffuse, kMax);
    sunSpecular = min(sunSpecular, kMax);
    ambientDiffuse = clamp(ambientDiffuse, -kMax, kMax);
    ambientSpecular = clamp(ambientSpecular, -kMax, kMax);
    pc.scene.debugLight.pixels[i] =
        uvec4(floatBitsToUint(depth), packHalf2x16(sunDiffuse.rg),
              packHalf2x16(vec2(sunDiffuse.b, sunSpecular.r)), packHalf2x16(sunSpecular.gb));
    pc.scene.debugLight.pixels[i + 1u] =
        uvec4(packHalf2x16(ambientDiffuse.rg), packHalf2x16(vec2(ambientDiffuse.b, ambientSpecular.r)),
              packHalf2x16(ambientSpecular.gb), packHalf2x16(stretch));
#endif
}

#ifdef DEBUG_ID_MESH
// mesh.frag: clusters (mesh.mesh) and the terrain.
void writeMeshDebugId(vec3 relative, uint material) {
#if defined(DEBUG_ID) && !defined(TERRAIN)
    writeDebugId(kDebugTriangles, inDebugId.x, inDebugId.y, relative, material);
#else
    writeDebugId(kDebugTerrain, 0u, 0u, relative, ~0u);
#endif
}
#endif
#endif

#endif
