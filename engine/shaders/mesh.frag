#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#include "common.glsl"
#include "atmosphere.glsl"
#include "material.glsl"
#include "terrain.glsl"
#include "lighting.glsl"
#ifdef TERRAIN
#include "placement.glsl"
#endif

layout(location = 0) in vec3 inRelativePosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inCurrentClip;
layout(location = 3) in vec4 inPreviousClip;
layout(location = 4) in vec2 inUv;
layout(location = 5) in vec4 inTangent;
layout(location = 6) flat in uint inMaterialIndex;
#ifndef TERRAIN
// How far (m) this LOD cluster may deviate from the full-detail surface the
// shadow rays are traced against; 0 at full detail.
layout(location = 7) flat in float inLodError;
// Height within the mesh, 0 at its base to 1 at its top (1 where unknown).
layout(location = 8) in float inHeight;
// Placed instances' prototype tint (Prototype::tint), 1 otherwise.
layout(location = 9) flat in vec3 inTint;
// A streamed plant's shrink factor (1 = full size): it shades like the
// terrain it turns into as this goes to 0.
layout(location = 10) flat in float inStreamFade;
#endif
#ifdef ALPHA_MASK
// The cluster's terrain sun visibility (cluster_cull.comp), or -1 where the
// terrain is marched here per fragment.
layout(location = 11) flat in float inTerrainSun;
#endif

layout(location = 0) out vec4 outColor;
// Screen-space motion in UV units, pointing from this frame's position to
// last frame's (the convention DLSS also uses).
layout(location = 1) out vec2 outMotion;
#ifdef ALPHA_BLEND
// Transparency mask for the upscaler: blended coverage, blended with MAX.
layout(location = 2) out float outTransparency;
#else
// Ray Reconstruction guides: diffuse albedo, specular albedo (the DFG
// pre-integrated reflectance), world normal and perceptual roughness.
layout(location = 3) out vec4 outDiffuseAlbedo;
layout(location = 4) out vec4 outSpecularAlbedo;
layout(location = 5) out vec4 outNormalRoughness;
#endif
// The sun's exposed radiance in outColor (after haze, pre-exposed): the
// contact shadow pass removes the occluded part of it. A blended surface
// scales what is behind it by its transmittance (alpha) and adds none of
// its own, so glass never darkens.
layout(location = 6) out vec4 outSunLight;

// inRelativePosition + cameraPosition, set first thing in main(); both are
// in the flat world, before the curvature warp (common.glsl). World
// positions place the shadow rays; derivatives, distances and terrain
// texture coordinates use the relative position, which keeps its precision
// far from the origin.
vec3 worldPosition;

// ---------------------------------------------------------------- Shadows

// The sun's visibility at a surface point (traceSunVisibility). On terrain
// the normal is the smooth shading normal, elsewhere the triangle's.
float sunVisibility(vec3 position, vec3 normal) {
#ifdef TERRAIN
    // Start on the heightfield the march tests, not on the raster: coarse
    // triangles can lie below it by more than their estimated error, and
    // can face away from the sun.
    Terrain terrain = pc.scene.terrain;
    float height = terrainHeight(terrain, position.xz);
    vec3 origin = position;
    origin.y = max(origin.y, height) + kShadowRayBias;
    // Where the smooth normal faces the sun but the bilinear cell doesn't,
    // the march would shadow the cell by itself, in jagged bands along the
    // terminator: start above the cell's corners over the shading plane
    // (Chiang et al. 2019).
    vec3 marchOrigin = origin;
    marchOrigin.y = max(origin.y, height + kShadowRayBias +
                                      terrainCellAbovePlane(terrain, position.xz, height,
                                                            normal));
#else
    vec3 origin = position + normal * max(kShadowRayBias, inLodError);
#ifdef ALPHA_MASK
    if (inTerrainSun == 0.0) {
        return 0.0;
    }
    if (inTerrainSun > 0.0) {
        return traceSunVisibility(origin, origin, sampleSunDirection(pc.scene, gl_FragCoord.xy),
                                  0.0, false, inRelativePosition, normal);
    }
#endif
    // An object's base can sit below the bilinear heightfield (detail
    // displacement, coarse ridges), where the march would hit at once.
    vec3 marchOrigin = origin;
    marchOrigin.y = max(origin.y, terrainHeight(pc.scene.terrain, origin.xz) + kShadowRayBias);
#endif
    return traceSunVisibility(origin, marchOrigin, sampleSunDirection(pc.scene, gl_FragCoord.xy),
                              0.0, true, inRelativePosition, normal);
}

// ---------------------------------------------------------------- Surface

vec4 sampleTexture(uint index, vec2 uv) {
    return texture(textures[nonuniformEXT(index)], uv, pc.scene.mipBias);
}

// The heightfield's normal from the normal map, filtered (trilinear,
// anisotropic) so distant ground doesn't alias; per pixel, because vertex
// normals would change with the LOD. Near the camera the detail
// displacement's slope is added.
vec3 terrainNormal() {
    Terrain terrain = pc.scene.terrain;
    vec2 size = vec2(float(terrain.width), float(terrain.height));
    vec2 uv = (worldPosition.xz / terrain.spacing + 0.5 * (size - 1.0) + 0.5) / size;
    vec2 xz = texture(textures[terrain.normalTexture], uv).rg;
    // The sampler mirrors like the heights, but a mirrored copy's slopes
    // point the other way.
    xz *= 1.0 - 2.0 * mod(floor(uv), 2.0);
    vec2 slope = -xz / sqrt(max(1.0 - dot(xz, xz), 1e-4));
    float weight = terrainDetailWeight(terrain, length(inRelativePosition));
    if (weight > 0.0) {
        slope += weight * terrainDetail(worldPosition.xz).yz;
    }
    return normalize(vec3(-slope.x, 1.0, -slope.y));
}

#ifdef TERRAIN
// Flat ground above the snow line, fading in over 40 m.
float snowCover(vec3 normal) {
    float snowLine = pc.scene.terrain.snowLine;
    return smoothstep(snowLine, snowLine + 40.0, worldPosition.y) *
           smoothstep(0.6, 0.8, normal.y);
}

// Grass, rock and snow chosen by slope and height: the terrain's fallback
// when no TerrainMaterial is set.
Surface proceduralTerrain(vec3 normal) {
    const vec3 kGrass = vec3(0.06, 0.13, 0.03);
    const vec3 kRock = vec3(0.20, 0.18, 0.16);
    const vec3 kSnow = vec3(0.80, 0.82, 0.85);

    Surface s;
    Terrain terrain = pc.scene.terrain;
    s.baseColor = mix(kRock, kGrass,
                      smoothstep(terrain.rockNormalY.x, terrain.rockNormalY.y, normal.y));
    float snow = snowCover(normal);
    s.baseColor = mix(s.baseColor, kSnow, snow);
    s.alpha = 1.0;
    s.metallic = 0.0;
    s.sunShadow = 1.0;
    s.perceptualRoughness = mix(0.9, 0.5, snow);
    s.occlusion = 1.0;
    s.microOcclusion = 1.0;
    s.emissive = vec3(0.0);
    s.normal = normal;
    return s;
}

// ---------------------------------------------------------------- Terrain
// Scanned ground layers projected in world space. Every sample uses
// explicit gradients so layers can be skipped per pixel without breaking
// mip selection. Texture coordinates come from the relative position plus
// the camera position wrapped by the layer's period (renderer_frame.cpp):
// world coordinates would lose float precision kilometers from the origin
// (stepping and jittering in motion), and everything below is periodic
// with that period, so a wrap changes nothing on screen.

struct TerrainTexel {
    vec3 baseColor;
    vec3 normal; // world space
    vec2 occlusionRoughness;
    float microOcclusion;
    float height;
};

TerrainTexel mixTexels(TerrainTexel a, TerrainTexel b, float t) {
    TerrainTexel r;
    r.baseColor = mix(a.baseColor, b.baseColor, t);
    r.normal = normalize(mix(a.normal, b.normal, t));
    r.occlusionRoughness = mix(a.occlusionRoughness, b.occlusionRoughness, t);
    r.microOcclusion = mix(a.microOcclusion, b.microOcclusion, t);
    r.height = mix(a.height, b.height, t);
    return r;
}

// Megascans ground AO also bakes the scan's large-scale occlusion (layer
// means 0.5-0.7), which the micro shadow turned into blotches across open
// ground at low sun. Relative to the layer's mean (its last mip) only the
// cavities darker than average shadow the sun.
float terrainMicroOcclusion(TerrainLayer layer, float occlusion) {
    float mean = textureLod(textures[nonuniformEXT(layer.arm)], vec2(0.5), 16.0).r;
    return mix(1.0, clamp(occlusion / max(mean, 1e-3), 0.0, 1.0), layer.occlusionStrength);
}

vec4 sampleGrad(uint index, vec2 uv, vec2 dx, vec2 dy) {
    // textureGrad takes no bias: scaling the gradients by 2^bias is the same.
    float scale = exp2(pc.scene.mipBias);
    return textureGrad(textures[nonuniformEXT(index)], uv, dx * scale, dy * scale);
}

// Hex-tiling (Mikkelsen 2022, "Practical Real-Time Hex-Tiling", JCGT;
// ported from github.com/mmikk/hextile-demo): each point blends three
// randomly rotated and offset copies of the texture on a hex grid, so no
// repetition shows at a distance. The blend favors the brighter copy
// instead of averaging, which keeps the texture's contrast (no mush).
const float kHexFallOffContrast = 0.6;
const float kHexExponent = 7.0;
// Grid scale relative to the texture: the reference's sqrt(12) cells per
// repeat make most of the surface a blend of copies (visible ghosting); one
// cell per repeat shows seams; two keeps blend zones thin without seams.
const float kHexCellsPerRepeat = 2.0;
// The wrap period in cells: 512 columns (256 repeats along u) by 592 rows
// (592 * sqrt(3)/4 = 256.3 repeats along v). These and kHexCellsPerRepeat
// must match gpu_types.h.
const int kHexWrapColumns = 512;
const int kHexWrapRows = 592;

// Rounds toward minus infinity (b > 0); integer division truncates.
int floorDiv(int a, int b) {
    return (a >= 0 ? a : a - b + 1) / b;
}

// A cell's index wrapped to the period, centered on the origin (identity
// within half a period), so every cell's rotation and offset repeat with
// it. Moving one period along v is the lattice step (-296, 592).
ivec2 wrapHexVertex(ivec2 v) {
    int rows = floorDiv(v.y + kHexWrapRows / 2, kHexWrapRows);
    v += rows * ivec2(kHexWrapRows / 2, -kHexWrapRows);
    v.x -= floorDiv(2 * v.x + v.y + kHexWrapColumns, 2 * kHexWrapColumns) * kHexWrapColumns;
    return v;
}

// Integer hashes (terrain.glsl) of the wrapped cell index: sin-based hashes
// and the reference's mod(x * y, 2 pi) rotation amplify last-bit float
// differences, so their tiles changed with the compiler, the GPU or any
// unrelated edit. Seeds 0-3 are the detail noise's.
vec2 hexHash(ivec2 vertex) {
    return vec2(terrainHash(vertex, 16u), terrainHash(vertex, 17u));
}

mat2 hexRotation(ivec2 vertex) {
    float angle = (terrainHash(vertex, 18u) * 2.0 - 1.0) * kPi;
    float c = cos(angle);
    float s = sin(angle);
    return mat2(c, -s, s, c);
}

// Applies the same tile placement to every map of a layer.
struct HexTiles {
    vec2 uv[3];
    vec2 dx[3];
    vec2 dy[3];
    mat2 rotation[3];
    vec3 barycentric;
};

HexTiles hexTiles(vec2 uv, vec2 dx, vec2 dy) {
    vec2 skewed = (uv * kHexCellsPerRepeat) * mat2(1.0, -0.57735027, 0.0, 1.15470054);
    ivec2 base = ivec2(floor(skewed));
    vec3 t = vec3(fract(skewed), 0.0);
    t.z = 1.0 - t.x - t.y;
    float s = step(0.0, -t.z);
    float s2 = 2.0 * s - 1.0;
    ivec2 is = ivec2(s);

    HexTiles tiles;
    tiles.barycentric = vec3(-t.z * s2, s - t.y * s2, s - t.x * s2);
    ivec2 vertices[3] = ivec2[3](base + is, base + ivec2(is.x, 1 - is.y),
                                 base + ivec2(1 - is.x, is.y));
    for (int i = 0; i < 3; ++i) {
        vec2 center = (vec2(vertices[i]) * mat2(1.0, 0.5, 0.0, 1.0 / 1.15470054)) /
                      kHexCellsPerRepeat;
        ivec2 wrapped = wrapHexVertex(vertices[i]);
        // A period along v isn't a whole number of texture repeats, so the
        // copy is placed from the wrapped cell's center.
        vec2 wrappedCenter = (vec2(wrapped) * mat2(1.0, 0.5, 0.0, 1.0 / 1.15470054)) /
                             kHexCellsPerRepeat;
        mat2 rotation = hexRotation(wrapped);
        tiles.uv[i] = rotation * (uv - center) + wrappedCenter + hexHash(wrapped);
        tiles.dx[i] = rotation * dx;
        tiles.dy[i] = rotation * dy;
        tiles.rotation[i] = rotation;
    }
    return tiles;
}

// A scan's normal map also holds bumps about the size of the tile. Once a
// pixel covers a good part of a tile, only those remain after filtering and
// they repeat as a visible grid, so normals fade out with the footprint
// (in tiles).
float normalFade(vec2 dx, vec2 dy) {
    return 1.0 - smoothstep(0.02, 0.2, max(length(dx), length(dy)));
}

// Tangent-space normal as a height derivative, the form that blends and
// rotates correctly (Mikkelsen's surface gradient framework).
vec2 normalToDerivative(vec3 n) {
    vec3 a = abs(n);
    return -n.xy / max(a.z, max(a.x, a.y) / 128.0);
}

// World offset of the parallax hit; every layer is sampled there (set by
// terrainSurface before the layers are read).
vec3 terrainParallax = vec3(0.0);

vec3 layerPosition(TerrainLayer layer) {
    return inRelativePosition + vec3(layer.offset[0], layer.offset[1], layer.offset[2]) +
           terrainParallax;
}

// Top-down projection, uv = (x, -z): tangent +X, bitangent -Z, normal +Y.
TerrainTexel samplePlanar(TerrainLayer layer, vec3 normal, vec3 dpdx, vec3 dpdy) {
    float scale = 1.0 / layer.tileSize;
    vec3 position = layerPosition(layer);
    HexTiles tiles = hexTiles(vec2(position.x, -position.z) * scale,
                              vec2(dpdx.x, -dpdx.z) * scale,
                              vec2(dpdy.x, -dpdy.z) * scale);

    vec3 colors[3];
    vec3 luminance;
    for (int i = 0; i < 3; ++i) {
        colors[i] = sampleGrad(layer.baseColor, tiles.uv[i], tiles.dx[i], tiles.dy[i]).rgb;
        luminance[i] = dot(colors[i], vec3(0.299, 0.587, 0.114));
    }
    // One set of weights for every map keeps color, height and normals of
    // the layer coherent.
    vec3 w = mix(vec3(1.0), luminance, kHexFallOffContrast) *
             pow(tiles.barycentric, vec3(kHexExponent));
    w /= w.x + w.y + w.z;
    // Inside a cell one copy dominates; skipping the others' remaining
    // maps saves most of the cost with no visible change.
    w *= step(0.02, w);
    w /= w.x + w.y + w.z;

    TerrainTexel texel;
    texel.baseColor = colors[0] * w.x + colors[1] * w.y + colors[2] * w.z;
    texel.occlusionRoughness = vec2(0.0);
    texel.height = 0.0;
    vec2 derivative = vec2(0.0);
    for (int i = 0; i < 3; ++i) {
        if (w[i] == 0.0) {
            continue;
        }
        texel.occlusionRoughness +=
            sampleGrad(layer.arm, tiles.uv[i], tiles.dx[i], tiles.dy[i]).rg * w[i];
        texel.height += sampleGrad(layer.height, tiles.uv[i], tiles.dx[i], tiles.dy[i]).r * w[i];
        vec3 n = sampleGrad(layer.normal, tiles.uv[i], tiles.dx[i], tiles.dy[i]).xyz * 2.0 - 1.0;
        // OpenGL-style maps point green up the image, which is -v here.
        n.y = -n.y;
        // Back from the tile's rotated frame to the layer's.
        derivative += (normalToDerivative(n) * tiles.rotation[i]) * w[i];
    }
    derivative *= normalFade(tiles.dx[0], tiles.dy[0]);
    vec3 n = normalize(vec3(-derivative, 1.0));
    vec3 t = normalize(vec3(1, 0, 0) - normal * normal.x);
    texel.normal = normalize(mat3(t, cross(normal, t), normal) * n);
    texel.microOcclusion = terrainMicroOcclusion(layer, texel.occlusionRoughness.x);
    texel.occlusionRoughness.x = mix(1.0, texel.occlusionRoughness.x, layer.occlusionStrength);
    texel.baseColor *= vec3(layer.albedoScale[0], layer.albedoScale[1], layer.albedoScale[2]);
    return texel;
}

// One hex-tiled side projection of the triplanar rock (same weights as
// samplePlanar). n is in the map's frame: u across, green up the image.
void sampleHexSide(TerrainLayer layer, vec2 uv, vec2 dx, vec2 dy, out vec3 color, out vec2 arm,
                   out float height, out vec3 n) {
    HexTiles tiles = hexTiles(uv, dx, dy);
    vec3 colors[3];
    vec3 luminance;
    for (int i = 0; i < 3; ++i) {
        colors[i] = sampleGrad(layer.baseColor, tiles.uv[i], tiles.dx[i], tiles.dy[i]).rgb;
        luminance[i] = dot(colors[i], vec3(0.299, 0.587, 0.114));
    }
    vec3 w = mix(vec3(1.0), luminance, kHexFallOffContrast) *
             pow(tiles.barycentric, vec3(kHexExponent));
    w /= w.x + w.y + w.z;
    w *= step(0.02, w);
    w /= w.x + w.y + w.z;

    color = colors[0] * w.x + colors[1] * w.y + colors[2] * w.z;
    arm = vec2(0.0);
    height = 0.0;
    vec2 derivative = vec2(0.0);
    for (int i = 0; i < 3; ++i) {
        if (w[i] == 0.0) {
            continue;
        }
        arm += sampleGrad(layer.arm, tiles.uv[i], tiles.dx[i], tiles.dy[i]).rg * w[i];
        height += sampleGrad(layer.height, tiles.uv[i], tiles.dx[i], tiles.dy[i]).r * w[i];
        vec3 tn = sampleGrad(layer.normal, tiles.uv[i], tiles.dx[i], tiles.dy[i]).xyz * 2.0 - 1.0;
        // The tiles rotate in (u, v); the map's green points along -v.
        tn.y = -tn.y;
        derivative += (normalToDerivative(tn) * tiles.rotation[i]) * w[i];
    }
    derivative *= normalFade(tiles.dx[0], tiles.dy[0]);
    vec3 nv = normalize(vec3(-derivative, 1.0));
    n = vec3(nv.x, -nv.y, nv.z);
}

// Triplanar projection with the Whiteout normal blend and per-side handedness
// correction (Ben Golus, "Normal Mapping for a Triplanar Shader"). The sides
// are hex-tiled so cliffs show no tile grid; the top keeps plain tiles with a
// 4x macro sample far away (its uv, (x, z), doesn't match the hex row
// period). `scaleMul` must be a whole number: it scales the uv, and the
// wrapped offsets stay seamless only for whole multiples of the hex period.
TerrainTexel sampleTriplanar(TerrainLayer layer, float scaleMul, vec3 normal, vec3 dpdx,
                             vec3 dpdy) {
    float scale = scaleMul / layer.tileSize;
    vec3 blend = pow(abs(normal), vec3(4.0));
    blend /= blend.x + blend.y + blend.z;
    blend *= step(0.01, blend);
    blend /= blend.x + blend.y + blend.z;
    vec3 axisSign = vec3(normal.x < 0.0 ? -1.0 : 1.0, normal.y < 0.0 ? -1.0 : 1.0,
                         normal.z < 0.0 ? -1.0 : 1.0);
    // Flipping u on the back sides keeps the textures unmirrored.
    vec2 flip[3] = vec2[3](vec2(axisSign.x, 1.0), vec2(axisSign.y, 1.0),
                           vec2(-axisSign.z, 1.0));
    // v runs against the world axis so green (up the image, -v) points
    // along +y on the sides and +z on top, as the Whiteout swizzles expect;
    // it also keeps cliffs upright.
    const vec2 kV = vec2(1.0, -1.0);
    vec3 position = layerPosition(layer);
    vec2 uvs[3] = vec2[3](position.zy * kV, position.xz * kV, position.xy * kV);
    vec2 dxs[3] = vec2[3](dpdx.zy * kV, dpdx.xz * kV, dpdx.xy * kV);
    vec2 dys[3] = vec2[3](dpdy.zy * kV, dpdy.xz * kV, dpdy.xy * kV);

    TerrainTexel texel;
    texel.baseColor = vec3(0.0);
    texel.occlusionRoughness = vec2(0.0);
    texel.height = 0.0;
    vec3 tangentNormals[3] = vec3[3](vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, 1.0));
    for (int axis = 0; axis < 3; ++axis) {
        if (blend[axis] == 0.0) {
            continue;
        }
        vec2 uv = uvs[axis] * flip[axis] * scale;
        vec2 dx = dxs[axis] * flip[axis] * scale;
        vec2 dy = dys[axis] * flip[axis] * scale;
        vec3 color;
        vec2 arm;
        float height;
        vec3 n;
        if (axis == 1) {
            color = sampleGrad(layer.baseColor, uv, dx, dy).rgb;
            // Far away the top's tiles repeat as a grid: the base color mixes
            // in the same texture at 4x the tile size (a whole number of
            // repeats per wrap period), and the tile-sized bumps of the
            // normal map fade sooner than on the hex-tiled layers.
            float far = smoothstep(0.01, 0.06, max(length(dx), length(dy)));
            if (far > 0.0) {
                vec3 macro = sampleGrad(layer.baseColor, uv * 0.25 + 0.5, dx * 0.25, dy * 0.25).rgb;
                color = mix(color, macro, 0.7 * far);
            }
            arm = sampleGrad(layer.arm, uv, dx, dy).rg;
            height = sampleGrad(layer.height, uv, dx, dy).r;
            n = sampleGrad(layer.normal, uv, dx, dy).xyz * 2.0 - 1.0;
            n.xy *= 1.0 - far;
        } else {
            sampleHexSide(layer, uv, dx, dy, color, arm, height, n);
        }
        texel.baseColor += color * blend[axis];
        texel.occlusionRoughness += arm * blend[axis];
        texel.height += height * blend[axis];
        n.x *= flip[axis].x;
        tangentNormals[axis] = n;
    }
    vec3 nx = tangentNormals[0];
    vec3 ny = tangentNormals[1];
    vec3 nz = tangentNormals[2];
    nx = vec3(nx.xy + normal.zy, abs(normal.x) * nx.z * axisSign.x);
    ny = vec3(ny.xy + normal.xz, abs(normal.y) * ny.z * axisSign.y);
    nz = vec3(nz.xy + normal.xy, abs(normal.z) * nz.z * axisSign.z);
    texel.normal = normalize(nx.zyx * blend.x + ny.xzy * blend.y + nz.xyz * blend.z);
    texel.microOcclusion = terrainMicroOcclusion(layer, texel.occlusionRoughness.x);
    texel.occlusionRoughness.x = mix(1.0, texel.occlusionRoughness.x, layer.occlusionStrength);
    texel.baseColor *= vec3(layer.albedoScale[0], layer.albedoScale[1], layer.albedoScale[2]);
    return texel;
}

// Height-based blend weight of b over a (Mishkinis, "Advanced Terrain
// Texture Splatting"): near the border the taller texel wins, so needles
// fill the gaps between stones instead of cross-fading.
float heightBlend(float heightA, float heightB, float t, float kDepth) {
    float top = max(heightA + 1.0 - t, heightB + t) - kDepth;
    float a = max(heightA + 1.0 - t - top, 0.0);
    float b = max(heightB + t - top, 0.0);
    return b / (a + b);
}

// Repeats every `period` cells: lattice points are wrapped to the period,
// centered on the origin, then hashed as integers (see hexHash).
float periodicHash(ivec2 cell, int period, uint seed) {
    ivec2 wrapped = ivec2(cell.x - period * floorDiv(cell.x + period / 2, period),
                          cell.y - period * floorDiv(cell.y + period / 2, period));
    return terrainHash(wrapped, seed);
}

float valueNoise(vec2 p, int period, uint seed) {
    ivec2 cell = ivec2(floor(p));
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(periodicHash(cell, period, seed),
                   periodicHash(cell + ivec2(1, 0), period, seed), f.x),
               mix(periodicHash(cell + ivec2(0, 1), period, seed),
                   periodicHash(cell + ivec2(1, 1), period, seed), f.x),
               f.y);
}

// The ground's noise cell (m) and its wrap in cells; must match
// gpu_types.h. Every octave's scale divides or multiplies the wrap evenly.
const float kNoiseCell = 40.0;
const int kNoiseWrapCells = 100;

// Value noise in kNoiseCell units at `scale` cells per cell (scale * 100
// must be a whole number).
float groundNoise(vec2 p, float scale, uint seed) {
    return valueNoise(p * scale, int(round(float(kNoiseWrapCells) * scale)), seed);
}

// Two octaves, the second on the lattice turned 45 degrees (and scaled by
// sqrt 2): it maps the wrap's lattice onto itself, so the sum stays periodic,
// and the two grids' axes don't line up. Contrast re-expanded around 0.5.
float rotatedFbm(vec2 p, float scale, uint seed) {
    vec2 r = vec2(p.x + p.y, p.y - p.x);
    float n = 0.6 * groundNoise(p, scale, seed) + 0.4 * groundNoise(r, scale, seed + 50u);
    return clamp(0.5 + (n - 0.5) * 1.5, 0.0, 1.0);
}

// The ground's patch and border-edge noise. A single lattice of value noise,
// thresholded, draws blocky axis-aligned borders, so both are rotated fBm of
// one shared domain warp (+-30 m, periodic noise, so still periodic).
void groundPatches(vec2 p, out float patches, out float edge) {
    vec2 warp = vec2(groundNoise(p, 0.5, 40u), groundNoise(p, 0.5, 41u)) - 0.5;
    vec2 q = p + 1.5 * warp;
    patches = rotatedFbm(q, 1.0, 21u);
    edge = (rotatedFbm(q, 8.0, 23u) - 0.5) * 0.5;
}

// The forest mask noise (placement.glsl, bit-exact with the trees') at this
// pixel, 0..1; -1 without a forest or outside the heightfield's crop, where
// nothing is placed.
float forestMask(Terrain terrain) {
    vec2 halfSize = 0.5 * (vec2(terrain.width, terrain.height) - 1.0) * terrain.spacing;
    if (terrain.forestMaskPeriod == 0u || any(greaterThan(abs(worldPosition.xz), halfSize))) {
        return -1.0;
    }
    ivec2 p = ivec2(round(worldPosition.xz * 65536.0));
    return float(placementFbm16(uint(p.x) ^ 0x80000000u, uint(p.y) ^ 0x80000000u,
                                terrain.forestMaskPeriod, kPlacementMaskOctaves,
                                terrain.forestMaskSeed ^ kPlacementMaskSalt)) / 65535.0;
}

// Laplacian of the ground height (1/m) over ~24 m, from the normal map's
// divergence: > 0 in hollows and valley bottoms, < 0 on ridges. On the
// Napf's ground under 12 degrees the 10th/90th percentiles are about
// -0.02/+0.017 (at 12 m steps from the 2 m data).
float groundCurvature(Terrain terrain) {
    const float kStep = 12.0;
    vec2 size = vec2(float(terrain.width), float(terrain.height));
    vec2 uv = (worldPosition.xz / terrain.spacing + 0.5 * (size - 1.0) + 0.5) / size;
    vec2 d = kStep / terrain.spacing / size;
    uint t = terrain.normalTexture;
    float lod = log2(kStep / terrain.spacing);
    float dx = textureLod(textures[t], uv + vec2(d.x, 0.0), lod).r -
               textureLod(textures[t], uv - vec2(d.x, 0.0), lod).r;
    float dz = textureLod(textures[t], uv + vec2(0.0, d.y), lod).g -
               textureLod(textures[t], uv - vec2(0.0, d.y), lod).g;
    return -(dx + dz) / (2.0 * kStep);
}

// b over a by height (heightBlend); a is skipped while hasA is false.
TerrainTexel overlay(TerrainTexel a, bool hasA, TerrainTexel b, float t, float depth) {
    return hasA ? mixTexels(a, b, heightBlend(a.height, b.height, t, depth)) : b;
}

// The role's variant (an index into the layers) picked by ~200 m noise, and
// the blend toward the next one.
uint roleVariant(uint role, vec2 p, out float t) {
    uint first = pc.scene.terrain.roles[role] & 0xFFu;
    uint count = pc.scene.terrain.roles[role] >> 8u;
    float n = clamp((groundNoise(p, 0.2, 30u + role) - 0.2) / 0.6, 0.0, 0.9999) * float(count);
    uint i = uint(n);
    t = i + 1u < count ? smoothstep(0.8, 1.0, fract(n)) : 0.0;
    return first + i;
}

// Rock's two variants cross-fade over ~40 m noise, so both show within one
// cliff; the second is sampled at twice the scale so their features never
// line up.
float rockVariantBlend(vec2 p) {
    return smoothstep(0.3, 0.7, groundNoise(p, 1.0, 38u));
}

// Strata bands (brightness and a slight warm/cool shift along a warped
// height) and moss in the dark crevices and on the ledges of the rock.
void weatherRock(inout TerrainTexel texel, vec2 p) {
    float warp = (groundNoise(p, 1.0, 42u) - 0.5) * 24.0 + (groundNoise(p, 4.0, 43u) - 0.5) * 4.0;
    float y = worldPosition.y + warp;
    // Value noise along the height, not sines: regular stripes read as paint.
    float band = 2.0 * (0.6 * valueNoise(vec2(y / 9.0, 0.5), 1 << 20, 45u) +
                        0.4 * valueNoise(vec2(y / 3.1, 0.5), 1 << 20, 46u)) - 1.0;
    texel.baseColor *= 1.0 + 0.1 * band;
    texel.baseColor *= 1.0 + 0.04 * band * vec3(1.0, 0.0, -1.0);

    float hollow = 1.0 - smoothstep(0.3, 0.6, texel.occlusionRoughness.x);
    float ledge = 0.7 * smoothstep(0.3, 0.6, texel.normal.y);
    float patches = smoothstep(0.35, 0.65, groundNoise(p, 4.0, 44u));
    float moss = 0.6 * max(hollow, ledge) * patches;
    texel.baseColor = mix(texel.baseColor, vec3(0.05, 0.07, 0.03), moss);
    texel.occlusionRoughness.y = mix(texel.occlusionRoughness.y, 1.0, moss);
}

// Near a variant border the next one is height-blended in. Layers are read
// straight from the scene buffer: indexing a local copy of Terrain with a
// dynamic index spills the whole struct to scratch memory (2-3x the
// terrain's cost).
TerrainTexel sampleRole(uint role, vec2 p, vec3 normal, vec3 dpdx, vec3 dpdy) {
    bool triplanar = role == 8u;
    float t;
    uint index;
    if (triplanar) {
        index = pc.scene.terrain.roles[role] & 0xFFu;
        t = (pc.scene.terrain.roles[role] >> 8u) > 1u ? rockVariantBlend(p) : 0.0;
    } else {
        index = roleVariant(role, p, t);
    }
    TerrainTexel texel;
    if (t < 1.0) {
        TerrainLayer layer = pc.scene.terrain.layers[index];
        texel = triplanar ? sampleTriplanar(layer, 1.0, normal, dpdx, dpdy)
                          : samplePlanar(layer, normal, dpdx, dpdy);
    }
    if (t > 0.0) {
        TerrainLayer layer = pc.scene.terrain.layers[index + 1u];
        TerrainTexel next = triplanar ? sampleTriplanar(layer, 2.0, normal, dpdx, dpdy)
                                      : samplePlanar(layer, normal, dpdx, dpdy);
        texel = overlay(texel, t < 1.0, next, t, 0.2);
    }
    if (triplanar) {
        weatherRock(texel, p);
    }
    return texel;
}

// Parallax occlusion mapping (Tatarchuk 2006) on one hex-tiled layer's
// height map, the tiles' placement fixed at the unshifted position.
// Grazing views are capped at 5x the depth.
const float kParallaxMinNoV = 0.2;

// Depth below the top surface (m) at a world offset from the pixel.
float parallaxDepth(TerrainLayer layer, HexTiles tiles, vec3 weights, vec3 offset,
                    float depthMax) {
    vec2 delta = vec2(offset.x, -offset.z) / layer.tileSize;
    float height = 0.0;
    for (int i = 0; i < 3; ++i) {
        if (weights[i] != 0.0) {
            height += sampleGrad(layer.height, tiles.uv[i] + tiles.rotation[i] * delta,
                                 tiles.dx[i], tiles.dy[i]).r * weights[i];
        }
    }
    return (1.0 - height) * depthMax;
}

// The world offset where the view ray meets the layer's surface (depth at
// most depthMax); sunShadow is the sun's visibility over that point.
vec3 parallaxHit(TerrainLayer layer, vec3 normal, vec3 dpdx, vec3 dpdy, float depthMax,
                 int steps, out float sunShadow) {
    float scale = 1.0 / layer.tileSize;
    vec3 position = layerPosition(layer);
    HexTiles tiles = hexTiles(vec2(position.x, -position.z) * scale,
                              vec2(dpdx.x, -dpdx.z) * scale, vec2(dpdy.x, -dpdy.z) * scale);
    vec3 weights = pow(tiles.barycentric, vec3(kHexExponent));
    weights /= weights.x + weights.y + weights.z;
    weights *= step(0.02, weights);
    weights /= weights.x + weights.y + weights.z;

    vec3 V = normalize(-inRelativePosition);
    float NoV = max(dot(normal, V), kParallaxMinNoV);
    float previousDepth = 0.0;
    float previousGap = parallaxDepth(layer, tiles, weights, vec3(0.0), depthMax);
    float hitDepth = 0.0;
    if (previousGap > 0.0) {
        for (int k = 1; k <= steps; ++k) {
            float rayDepth = depthMax * float(k) / float(steps);
            float surface = parallaxDepth(layer, tiles, weights, -V * (rayDepth / NoV), depthMax);
            float gap = surface - rayDepth;
            if (gap <= 0.0) {
                hitDepth = mix(previousDepth, rayDepth, previousGap / (previousGap - gap));
                break;
            }
            previousDepth = rayDepth;
            previousGap = gap;
        }
    }
    vec3 hit = -V * (hitDepth / NoV);

    sunShadow = 1.0;
    vec3 L = pc.scene.sunDirection;
    float NoL = dot(normal, L);
    if (NoL > 0.0 && hitDepth > 0.0) {
        float blocked = 0.0;
        int shadowSteps = max(steps / 2, 3);
        for (int k = 1; k <= shadowSteps; ++k) {
            float rise = hitDepth * float(k) / float(shadowSteps);
            float surface = parallaxDepth(layer, tiles, weights,
                                          hit + L * (rise / max(NoL, kParallaxMinNoV)),
                                          depthMax);
            blocked = max(blocked, (hitDepth - rise) - surface);
        }
        sunShadow = 1.0 - smoothstep(0.0, 0.2 * depthMax, blocked);
    }
    return hit;
}

// Each role's weight from the ground's shape, the forest mask and noise,
// stacked bottom to top in TerrainRole order (grass at the bottom, rock on
// top) with height blends; ~5 m edge noise makes the borders irregular. A
// role is only sampled where it shows: weight above 0 and nothing above it
// at full weight.
Surface terrainSurface(vec3 normal) {
    const vec3 kSnow = vec3(0.80, 0.82, 0.85);
    vec3 dpdx = dFdx(inRelativePosition);
    vec3 dpdy = dFdy(inRelativePosition);
    Terrain terrain = pc.scene.terrain;

    vec2 p = (inRelativePosition.xz + terrain.noiseOffset) / kNoiseCell;
    float patches;
    float edge;
    groundPatches(p, patches, edge);
    float curvature = groundCurvature(terrain);
    float flatGround = smoothstep(0.93, 0.98, normal.y);
    float belowRock = 1.0 - smoothstep(terrain.rockNormalY.y, terrain.rockNormalY.y + 0.08,
                                       normal.y);
    float rock = 1.0 - smoothstep(terrain.rockNormalY.x, terrain.rockNormalY.y, normal.y);

    float w[9];
    w[0] = 1.0;
    w[1] = flatGround * smoothstep(0.006, 0.015, curvature + edge * 0.01) * smoothstep(0.42, 0.62, patches + curvature * 10.0);
    w[2] = flatGround * smoothstep(0.02, 0.04, curvature + edge * 0.02);
    w[3] = max(belowRock, smoothstep(0.03, 0.06, -curvature + edge * 0.02)) *
           smoothstep(0.42, 0.62, patches - curvature * 10.0 + edge * 0.5);
    float m = forestMask(terrain);
    float lo = float(terrain.forestMaskLo) / 65535.0;
    float d = float(terrain.forestMaskHi - terrain.forestMaskLo) / 65535.0;
    // Path over the outer half of the trees' ramp (sparse outer trees),
    // floor from its middle in, under the canopy.
    w[4] = m < 0.0 ? 0.0 : smoothstep(lo - 0.25 * d, lo + 0.25 * d, m + edge * d);
    w[5] = m < 0.0 ? 0.0 : smoothstep(lo + 0.4 * d, lo + d, m + edge * d);
    w[6] = (1.0 - flatGround) * smoothstep(0.045, 0.08, curvature + edge * 0.03);
    w[7] = (1.0 - smoothstep(terrain.rockNormalY.y, terrain.rockNormalY.y + 0.04, normal.y)) *
           smoothstep(0.3, 0.5, patches + edge);
    w[8] = rock;

    int start = 0;
    for (int r = 8; r > 0; --r) {
        if (w[r] >= 1.0) {
            start = r;
            break;
        }
    }
    // POM on the dominant layer's height; every layer is then sampled at
    // the shifted position, so the blends agree with it. Rock (triplanar)
    // has none: three projections would triple the march.
    float sunShadow = 1.0;
    int dominant = 0;
    for (int r = 8; r > 0; --r) {
        if (w[r] >= 0.5) {
            dominant = r;
            break;
        }
    }
    if (dominant != 8) {
        float variantBlend;
        uint index = roleVariant(uint(dominant), p, variantBlend) + (variantBlend >= 0.5 ? 1u : 0u);
        TerrainLayer layer = pc.scene.terrain.layers[index];
        // The largest shift in pixels (of the pixel's longer axis): parallax
        // fades out below ~1 pixel (3 cm of depth at ~40 m, 1 cm at ~10 m)
        // and needs a step per ~2 pixels (the secant refines between them).
        float NoV = max(dot(normal, normalize(-inRelativePosition)), kParallaxMinNoV);
        float depthMax = layer.heightScale * pc.scene.parallaxDepthScale;
        float pixels = depthMax * sqrt(max(1.0 - NoV * NoV, 0.0)) / NoV /
                       max(max(length(dpdx), length(dpdy)), 1e-6);
        float fade = smoothstep(0.5, 1.5, pixels) * (1.0 - clamp(w[8], 0.0, 1.0));
        // The march sees one layer: fade the relief out toward where the
        // dominant role or variant switches, so it doesn't jump there.
        float above = 0.0;
        for (int r = dominant + 1; r < 9; ++r) {
            above = max(above, w[r]);
        }
        float coverage = (dominant > 0 ? smoothstep(0.5, 0.75, w[dominant]) : 1.0) *
                         (1.0 - smoothstep(0.25, 0.5, above));
        fade *= coverage * (1.0 - smoothstep(0.25, 0.5, min(variantBlend, 1.0 - variantBlend)));
        if (fade > 0.0) {
            float shadow;
            int steps = clamp(int(ceil(0.5 * pixels * fade)), 2, 12);
            terrainParallax = parallaxHit(layer, normal, dpdx, dpdy, depthMax * fade,
                                          steps, shadow);
            sunShadow = pc.scene.parallaxSelfShadow != 0.0 ? mix(1.0, shadow, fade) : 1.0;
        }
    }

    TerrainTexel ground;
    bool has = false;
    for (int r = start; r < 9; ++r) {
        float t = clamp(w[r], 0.0, 1.0);
        if (t > 0.0) {
            ground = overlay(ground, has, sampleRole(uint(r), p, normal, dpdx, dpdy), t,
                             r < 6 ? 0.3 : 0.1);
            has = true;
        }
    }

    // Large-scale brightness and hue variation (~400 m and ~160 m), so no
    // stretch of one layer looks uniform.
    float macro = 0.6 * groundNoise(p, 0.1, 24u) + 0.4 * groundNoise(p, 0.25, 25u);
    float hue = groundNoise(p, 0.25, 26u) - 0.5;
    ground.baseColor *= (0.85 + 0.3 * macro) * vec3(1.0 + 0.1 * hue, 1.0, 1.0 - 0.1 * hue);

    Surface s;
    s.baseColor = ground.baseColor;
    s.occlusion = ground.occlusionRoughness.x;
    s.microOcclusion = ground.microOcclusion;
    s.perceptualRoughness = ground.occlusionRoughness.y;
    s.normal = ground.normal;
    float snow = snowCover(normal);
    s.baseColor = mix(s.baseColor, kSnow, snow);
    s.perceptualRoughness = mix(s.perceptualRoughness, 0.5, snow);
    s.normal = normalize(mix(s.normal, normal, snow));
    s.sunShadow = mix(sunShadow, 1.0, snow);
    s.alpha = 1.0;
    s.metallic = 0.0;
    s.emissive = vec3(0.0);
    return s;
}

#endif // TERRAIN

#ifndef TERRAIN
// The Megascans plants ship no AO: darken toward the base of the tuft. Leaf
// materials (diffuse transmission) only; the micro shadow Effects strength
// scales it.
float plantOcclusion(Material m) {
    if (m.diffuseTransmission <= 0.0) {
        return 1.0;
    }
    return mix(1.0, mix(1.0, 0.25 + 0.75 * smoothstep(0.0, 0.85, inHeight), inStreamFade),
               pc.scene.microShadowStrength);
}
#endif

Surface materialSurface(Material m, vec3 normal) {
    Surface s;
    vec4 baseColor = m.baseColorFactor;
    if (m.baseColorTexture != kNoTexture) {
        baseColor *= sampleTexture(m.baseColorTexture, inUv);
    }
#ifndef TERRAIN
    baseColor.rgb *= inTint;
#endif
    s.baseColor = baseColor.rgb;
    s.alpha = baseColor.a;

    // glTF packs roughness in green and metalness in blue.
    s.metallic = m.metallicFactor;
    s.perceptualRoughness = m.roughnessFactor;
    if (m.metallicRoughnessTexture != kNoTexture) {
        vec4 mr = sampleTexture(m.metallicRoughnessTexture, inUv);
        s.perceptualRoughness *= mr.g;
        s.metallic *= mr.b;
    }

    s.occlusion = 1.0;
    if (m.occlusionTexture != kNoTexture) {
        s.occlusion = mix(1.0, sampleTexture(m.occlusionTexture, inUv).r,
                          m.occlusionStrength);
    }
#ifndef TERRAIN
    s.occlusion *= plantOcclusion(m);
#endif
    s.microOcclusion = s.occlusion;

    s.sunShadow = 1.0;
    s.emissive = m.emissiveFactor;
    if (m.emissiveTexture != kNoTexture) {
        s.emissive *= sampleTexture(m.emissiveTexture, inUv).rgb;
    }

    s.normal = normal;
    if (m.normalTexture != kNoTexture && dot(inTangent.xyz, inTangent.xyz) > 0.0) {
        vec3 t = normalize(inTangent.xyz - normal * dot(normal, inTangent.xyz));
        vec3 b = cross(normal, t) * inTangent.w;
        vec3 n = sampleTexture(m.normalTexture, inUv).xyz * 2.0 - 1.0;
        n.xy *= m.normalScale;
        s.normal = normalize(mat3(t, b, normal) * n);
    }
    return s;
}

void main() {
    worldPosition = inRelativePosition + pc.scene.cameraPosition;
#ifdef TERRAIN
    vec3 normal = terrainNormal();
    Surface s = pc.scene.terrain.enabled != 0u ? terrainSurface(normal)
                                               : proceduralTerrain(normal);
#else
    Material material = pc.scene.materials.materials[inMaterialIndex];
    vec3 normal = normalize(inNormal);
    if (!gl_FrontFacing) {
        normal = -normal;
    }
    Surface s = materialSurface(material, normal);
    if (inStreamFade < 1.0) {
        s.normal = normalize(mix(terrainNormal(), s.normal, inStreamFade));
    }
#endif
#ifdef ALPHA_MASK
    if (s.alpha < material.alphaCutoff) {
        discard;
    }
#endif

    Shading sh = beginShading(s, inRelativePosition);

    // Offset shadow rays along the true triangle normal: the smooth shading
    // normal can point below the surface and make the ray hit its own
    // triangle. Derivatives must be taken outside the branch below.
    vec3 geometricNormal =
        normalize(cross(dFdx(inRelativePosition), dFdy(inRelativePosition)));
    if (dot(geometricNormal, normal) < 0.0) {
        geometricNormal = -geometricNormal;
    }

    vec3 sunRadiance = vec3(0.0);
#ifdef TERRAIN
    // A coarse LOD triangle can face away from the sun where the
    // heightfield (the shading normal) doesn't; its facing would cut the
    // light per triangle.
    bool facesSun = true;
#else
    // A fading plant's normal is blended toward the terrain's (up), which
    // turns off its back faces' transmission; let them take the sun.
    bool facesSun = dot(geometricNormal, sh.L) > 0.0 || inStreamFade < 1.0;
#endif
    if (sh.NoL > 0.0 && facesSun) {
#ifdef TERRAIN
        float visibility = sunVisibility(worldPosition, normal);
#else
        float visibility = sunVisibility(worldPosition, geometricNormal);
#endif
#ifdef TERRAIN
        float plantSun = 1.0;
#else
        float plantSun = mix(1.0, plantOcclusion(material), 0.5);
#endif
        addSunLight(sh, visibility,
                    plantSun *
                        mix(1.0, microShadow(sh.NoL, s.microOcclusion), pc.scene.microShadowStrength) *
                        s.sunShadow);
        sunRadiance = sh.diffuseLight + sh.specularLight;
    }
#ifndef TERRAIN
    bool leaf = material.diffuseTransmission > 0.0;
    if (leaf && dot(sh.N, sh.L) < 0.0 && dot(geometricNormal, sh.L) < 0.0) {
        float visibility = sunVisibility(worldPosition, -geometricNormal);
        sh.diffuseLight += sh.diffuseColor / kPi * sh.sunAtSurface *
                           (clamp(-dot(sh.N, sh.L), 0.0, 1.0) * visibility) *
                           material.diffuseTransmission;
    }
#endif

    addAmbientLight(sh, s.occlusion);
#ifndef TERRAIN
    if (leaf) {
        vec3 skyIrradiance = pc.scene.skyIrradiance.irradiance;
        vec3 groundIrradiance =
            kGroundAlbedo * (sh.sunAtSurface * max(sh.L.y, 0.0) + skyIrradiance);
        vec3 backIrradiance = mix(groundIrradiance, skyIrradiance, -sh.N.y * 0.5 + 0.5);
        if (pc.scene.giMode != 0u) {
            backIrradiance = giIrradiance(giSample(sh.worldPosition, -sh.N), -sh.N, backIrradiance);
        }
        sh.diffuseLight += sh.diffuseColor / kPi * backIrradiance * material.diffuseTransmission *
                           multiBounceOcclusion(s.occlusion, sh.diffuseColor);
    }
#endif

#ifdef ALPHA_BLEND
    // Premultiplied alpha (blend: one, one-minus-source-alpha).
    float coverage = s.alpha;
    vec3 luminance = sh.diffuseLight * coverage + sh.specularLight;
#else
    float coverage = 1.0;
    vec3 luminance = sh.diffuseLight + sh.specularLight;
#endif

    float hazeTransmittance;
    luminance = applyAerialPerspective(luminance, inRelativePosition, coverage, hazeTransmittance);
    sunRadiance *= hazeTransmittance;

    // Emissive is already in physical units (cd/m^2), not relative to the sun.
    vec3 exposed = luminance * pc.scene.sunIlluminance + s.emissive;
    outColor = vec4(exposed * pc.scene.exposure.exposure, coverage);

    vec2 currentUv = inCurrentClip.xy / inCurrentClip.w * 0.5;
    vec2 previousUv = inPreviousClip.xy / inPreviousClip.w * 0.5;
    outMotion = previousUv - currentUv;
#ifdef ALPHA_BLEND
    outTransparency = coverage;
    outSunLight = vec4(0.0, 0.0, 0.0, coverage);
#else
    outDiffuseAlbedo = vec4(sh.diffuseColor, 1.0);
    outSpecularAlbedo = vec4(mix(sh.dfg.xxx, sh.dfg.yyy, sh.f0), 1.0);
    outNormalRoughness = vec4(sh.N, sh.perceptualRoughness);
    outSunLight = vec4(sunRadiance * pc.scene.sunIlluminance * pc.scene.exposure.exposure, 1.0);
#endif
}
