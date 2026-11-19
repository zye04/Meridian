#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#include "common.glsl"
#include "atmosphere.glsl"
#include "material.glsl"

layout(location = 0) in vec3 inRelativePosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inCurrentClip;
layout(location = 3) in vec4 inPreviousClip;
layout(location = 4) in vec2 inUv;
layout(location = 5) in vec4 inTangent;
layout(location = 6) flat in uint inMaterialIndex;
// How far (m) this LOD cluster may deviate from the full-detail surface the
// shadow rays are traced against; 0 at full detail.
layout(location = 7) flat in float inLodError;

layout(location = 0) out vec4 outColor;
// Screen-space motion in UV units, pointing from this frame's position to
// last frame's (the convention DLSS also uses).
layout(location = 1) out vec2 outMotion;

const float kShadowRayBias = 0.05;
const float kShadowRayLength = 20000.0;
const float kGroundAlbedo = 0.1;
const float kMinPerceptualRoughness = 0.045;

// inRelativePosition + cameraPosition, set first thing in main(). World
// positions place textures and the shadow rays; derivatives and distances
// use the relative position, which keeps its precision far from the origin.
vec3 worldPosition;

// ---------------------------------------------------------------- Shadows

// Interleaved gradient noise (Jimenez 2014), shifted every frame so the
// temporal resolve averages successive samples.
float interleavedGradientNoise(vec2 pixel, uint frame) {
    pixel += 5.588238 * float(frame % 64u);
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// Picks a direction inside the sun's disk. One ray per pixel per frame;
// accumulated over frames this gives penumbrae that widen with distance from
// the occluder, like real sun shadows.
vec3 sampleSunDirection(vec2 pixel) {
    vec3 sun = pc.scene.sunDirection;
    vec3 tangent = normalize(cross(abs(sun.y) < 0.99 ? vec3(0, 1, 0)
                                                      : vec3(1, 0, 0), sun));
    vec3 bitangent = cross(sun, tangent);

    float radius = tan(pc.scene.sunAngularRadius) *
                   sqrt(interleavedGradientNoise(pixel, pc.scene.frameIndex));
    float angle = 2.0 * kPi *
                  interleavedGradientNoise(pixel + vec2(17.0, 31.0),
                                           pc.scene.frameIndex);
    return normalize(sun + radius * (cos(angle) * tangent +
                                     sin(angle) * bitangent));
}

// Only opaque and alpha-masked instances cast shadows (see buildTlas).
const uint kShadowCasterMask = 0x01u;

// Alpha-masked geometry is non-opaque in the acceleration structure, so the
// ray query reports each candidate hit; we accept it only where the base
// color alpha passes the material's cutoff.
bool alphaTestCandidate(rayQueryEXT query) {
    InstanceData instance = pc.scene.instances.instances[
        rayQueryGetIntersectionInstanceCustomIndexEXT(query, false)];
    MeshInfo mesh = pc.scene.meshes.meshes[instance.meshIndex];
    uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
    vec2 barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, false);

    uint first = mesh.firstIndex + primitive * 3u;
    Vertex v0 = pc.scene.vertices.vertices[pc.scene.indices.indices[first] + mesh.vertexOffset];
    Vertex v1 = pc.scene.vertices.vertices[pc.scene.indices.indices[first + 1u] + mesh.vertexOffset];
    Vertex v2 = pc.scene.vertices.vertices[pc.scene.indices.indices[first + 2u] + mesh.vertexOffset];
    vec3 w = vec3(1.0 - barycentrics.x - barycentrics.y, barycentrics);
    vec2 uv = w.x * vec2(v0.uvX, v0.uvY) + w.y * vec2(v1.uvX, v1.uvY) +
              w.z * vec2(v2.uvX, v2.uvY);

    Material m = pc.scene.materials.materials[mesh.materialIndex];
    float alpha = m.baseColorFactor.a;
    if (m.baseColorTexture != kNoTexture) {
        alpha *= textureLod(textures[nonuniformEXT(m.baseColorTexture)], uv, 0.0).a;
    }
    return alpha >= m.alphaCutoff;
}

// Traces one ray toward the sun: 1 if it escapes, 0 if anything blocks it.
float sunVisibility(vec3 position, vec3 geometricNormal) {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, accelerationStructureEXT(pc.scene.tlas),
                          gl_RayFlagsTerminateOnFirstHitEXT, kShadowCasterMask,
                          position + geometricNormal * max(kShadowRayBias, inLodError), 0.0,
                          sampleSunDirection(gl_FragCoord.xy), kShadowRayLength);
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
                gl_RayQueryCandidateIntersectionTriangleEXT &&
            alphaTestCandidate(query)) {
            rayQueryConfirmIntersectionEXT(query);
        }
    }
    return rayQueryGetIntersectionTypeEXT(query, true) ==
                   gl_RayQueryCommittedIntersectionNoneEXT
               ? 1.0
               : 0.0;
}

// ---------------------------------------------------------------- Surface

struct Surface {
    vec3 baseColor;
    float alpha;
    float metallic;
    float perceptualRoughness;
    float occlusion;
    vec3 emissive;
    vec3 normal;
};

vec4 sampleTexture(uint index, vec2 uv) {
    return texture(textures[nonuniformEXT(index)], uv);
}

// Grass, rock and snow chosen by slope and height: the terrain's fallback
// when no TerrainMaterial is set.
Surface proceduralTerrain(vec3 normal) {
    const vec3 kGrass = vec3(0.06, 0.13, 0.03);
    const vec3 kRock = vec3(0.20, 0.18, 0.16);
    const vec3 kSnow = vec3(0.80, 0.82, 0.85);

    Surface s;
    s.baseColor = mix(kRock, kGrass, smoothstep(0.75, 0.9, normal.y));
    float snow = smoothstep(150.0, 190.0, worldPosition.y) *
                 smoothstep(0.6, 0.8, normal.y);
    s.baseColor = mix(s.baseColor, kSnow, snow);
    s.alpha = 1.0;
    s.metallic = 0.0;
    s.perceptualRoughness = mix(0.9, 0.5, snow);
    s.occlusion = 1.0;
    s.emissive = vec3(0.0);
    s.normal = normal;
    return s;
}

// ---------------------------------------------------------------- Terrain
// Scanned ground layers projected in world space (the terrain's UVs span
// the whole mesh). Every sample uses explicit gradients so layers can be
// skipped per pixel without breaking mip selection.

struct TerrainTexel {
    vec3 baseColor;
    vec3 normal; // world space
    vec2 occlusionRoughness;
    float height;
};

TerrainTexel mixTexels(TerrainTexel a, TerrainTexel b, float t) {
    TerrainTexel r;
    r.baseColor = mix(a.baseColor, b.baseColor, t);
    r.normal = normalize(mix(a.normal, b.normal, t));
    r.occlusionRoughness = mix(a.occlusionRoughness, b.occlusionRoughness, t);
    r.height = mix(a.height, b.height, t);
    return r;
}

vec4 sampleGrad(uint index, vec2 uv, vec2 dx, vec2 dy) {
    return textureGrad(textures[nonuniformEXT(index)], uv, dx, dy);
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

vec2 hexHash(vec2 p) {
    return fract(sin(p * mat2(127.1, 311.7, 269.5, 183.3)) * 43758.5453);
}

mat2 hexRotation(ivec2 vertex) {
    float angle = abs(float(vertex.x * vertex.y)) + abs(float(vertex.x + vertex.y)) + kPi;
    angle = mod(angle, 2.0 * kPi);
    if (angle > kPi) {
        angle -= 2.0 * kPi;
    }
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
        mat2 rotation = hexRotation(vertices[i]);
        tiles.uv[i] = rotation * (uv - center) + center + hexHash(vec2(vertices[i]));
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

// Top-down projection, uv = (x, -z): tangent +X, bitangent -Z, normal +Y.
TerrainTexel samplePlanar(TerrainLayer layer, vec3 normal, vec3 dpdx, vec3 dpdy) {
    float scale = 1.0 / layer.tileSize;
    HexTiles tiles = hexTiles(vec2(worldPosition.x, -worldPosition.z) * scale,
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
    return texel;
}

// Triplanar projection with the Whiteout normal blend and per-side handedness
// correction (Ben Golus, "Normal Mapping for a Triplanar Shader").
TerrainTexel sampleTriplanar(TerrainLayer layer, vec3 normal, vec3 dpdx, vec3 dpdy) {
    float scale = 1.0 / layer.tileSize;
    vec3 blend = pow(abs(normal), vec3(4.0));
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
    vec2 uvs[3] = vec2[3](worldPosition.zy * kV, worldPosition.xz * kV,
                          worldPosition.xy * kV);
    vec2 dxs[3] = vec2[3](dpdx.zy * kV, dpdx.xz * kV, dpdx.xy * kV);
    vec2 dys[3] = vec2[3](dpdy.zy * kV, dpdy.xz * kV, dpdy.xy * kV);

    TerrainTexel texel;
    texel.baseColor = vec3(0.0);
    texel.occlusionRoughness = vec2(0.0);
    texel.height = 0.0;
    vec3 tangentNormals[3];
    for (int axis = 0; axis < 3; ++axis) {
        vec2 uv = uvs[axis] * flip[axis] * scale;
        vec2 dx = dxs[axis] * flip[axis] * scale;
        vec2 dy = dys[axis] * flip[axis] * scale;
        texel.baseColor += sampleGrad(layer.baseColor, uv, dx, dy).rgb * blend[axis];
        texel.occlusionRoughness += sampleGrad(layer.arm, uv, dx, dy).rg * blend[axis];
        texel.height += sampleGrad(layer.height, uv, dx, dy).r * blend[axis];
        vec3 n = sampleGrad(layer.normal, uv, dx, dy).xyz * 2.0 - 1.0;
        n.x *= flip[axis].x;
        n.xy *= normalFade(dx, dy);
        tangentNormals[axis] = n;
    }
    vec3 nx = tangentNormals[0];
    vec3 ny = tangentNormals[1];
    vec3 nz = tangentNormals[2];
    nx = vec3(nx.xy + normal.zy, abs(normal.x) * nx.z * axisSign.x);
    ny = vec3(ny.xy + normal.xz, abs(normal.y) * ny.z * axisSign.y);
    nz = vec3(nz.xy + normal.xy, abs(normal.z) * nz.z * axisSign.z);
    texel.normal = normalize(nx.zyx * blend.x + ny.xzy * blend.y + nz.xyz * blend.z);
    return texel;
}

// Height-based blend weight of b over a (Mishkinis, "Advanced Terrain
// Texture Splatting"): near the border the taller texel wins, so needles
// fill the gaps between stones instead of cross-fading.
float heightBlend(float heightA, float heightB, float t) {
    const float kDepth = 0.1;
    float top = max(heightA + 1.0 - t, heightB + t) - kDepth;
    float a = max(heightA + 1.0 - t - top, 0.0);
    float b = max(heightB + t - top, 0.0);
    return b / (a + b);
}

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float valueNoise(vec2 p) {
    vec2 cell = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(cell), hash(cell + vec2(1, 0)), f.x),
               mix(hash(cell + vec2(0, 1)), hash(cell + vec2(1, 1)), f.x), f.y);
}

Surface terrainSurface(vec3 normal) {
    const float kNoisePeriod = 40.0; // meters
    const vec3 kSnow = vec3(0.80, 0.82, 0.85);
    vec3 dpdx = dFdx(inRelativePosition);
    vec3 dpdy = dFdy(inRelativePosition);
    Terrain terrain = pc.scene.terrain;

    vec2 p = worldPosition.xz / kNoisePeriod;
    float noise = 0.65 * valueNoise(p) + 0.35 * valueNoise(p * 2.7 + 13.0);
    float needles = smoothstep(0.45, 0.55, noise);
    float rock = 1.0 - smoothstep(0.75, 0.9, normal.y);

    TerrainTexel ground;
    if (rock < 1.0) {
        if (needles <= 0.0) {
            ground = samplePlanar(terrain.layers[0], normal, dpdx, dpdy);
        } else if (needles >= 1.0) {
            ground = samplePlanar(terrain.layers[1], normal, dpdx, dpdy);
        } else {
            TerrainTexel soil = samplePlanar(terrain.layers[0], normal, dpdx, dpdy);
            TerrainTexel litter = samplePlanar(terrain.layers[1], normal, dpdx, dpdy);
            ground = mixTexels(soil, litter, heightBlend(soil.height, litter.height, needles));
        }
    }
    if (rock > 0.0) {
        TerrainTexel cliff = sampleTriplanar(terrain.layers[2], normal, dpdx, dpdy);
        ground = rock >= 1.0 ? cliff
                             : mixTexels(ground, cliff,
                                         heightBlend(ground.height, cliff.height, rock));
    }

    Surface s;
    s.baseColor = ground.baseColor;
    s.occlusion = ground.occlusionRoughness.x;
    s.perceptualRoughness = ground.occlusionRoughness.y;
    s.normal = ground.normal;
    float snow = smoothstep(150.0, 190.0, worldPosition.y) *
                 smoothstep(0.6, 0.8, normal.y);
    s.baseColor = mix(s.baseColor, kSnow, snow);
    s.perceptualRoughness = mix(s.perceptualRoughness, 0.5, snow);
    s.normal = normalize(mix(s.normal, normal, snow));
    s.alpha = 1.0;
    s.metallic = 0.0;
    s.emissive = vec3(0.0);
    return s;
}

Surface materialSurface(Material m, vec3 normal) {
    Surface s;
    vec4 baseColor = m.baseColorFactor;
    if (m.baseColorTexture != kNoTexture) {
        baseColor *= sampleTexture(m.baseColorTexture, inUv);
    }
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

// ---------------------------------------------------------------- BRDF
// Filament's standard model (google/filament, shaders/src/surface_brdf.fs):
// GGX distribution, height-correlated Smith visibility, Schlick Fresnel,
// Lambert diffuse, with multiple-scattering energy compensation.

float distributionGgx(float NoH, float a) {
    float a2 = a * a;
    float f = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (kPi * f * f);
}

float visibilitySmithGgxCorrelated(float NoV, float NoL, float a) {
    float a2 = a * a;
    float ggxL = NoV * sqrt((NoL - NoL * a2) * NoL + a2);
    float ggxV = NoL * sqrt((NoV - NoV * a2) * NoV + a2);
    return 0.5 / (ggxV + ggxL);
}

vec3 fresnelSchlick(vec3 f0, float VoH) {
    float f = pow(1.0 - VoH, 5.0);
    return f + f0 * (1.0 - f);
}

void main() {
    worldPosition = inRelativePosition + pc.scene.cameraPosition;
    Material material = pc.scene.materials.materials[inMaterialIndex];
    vec3 normal = normalize(inNormal);
    if (!gl_FrontFacing) {
        normal = -normal;
    }

    Surface s;
    if ((material.flags & kMaterialTerrain) == 0u) {
        s = materialSurface(material, normal);
    } else if (pc.scene.terrain.enabled != 0u) {
        s = terrainSurface(normal);
    } else {
        s = proceduralTerrain(normal);
    }
#ifdef ALPHA_MASK
    if (s.alpha < material.alphaCutoff) {
        discard;
    }
#endif

    vec3 N = s.normal;
    vec3 V = normalize(-inRelativePosition);
    vec3 L = pc.scene.sunDirection;
    float NoV = max(dot(N, V), 1e-4);

    float perceptualRoughness = clamp(s.perceptualRoughness, kMinPerceptualRoughness, 1.0);
    float a = perceptualRoughness * perceptualRoughness;
    // Dielectrics reflect ~4% at normal incidence; metals tint reflection.
    vec3 f0 = mix(vec3(0.04), s.baseColor, s.metallic);
    vec3 diffuseColor = s.baseColor * (1.0 - s.metallic);

    vec2 dfg = textureLod(dfgLut, vec2(NoV, perceptualRoughness), 0.0).xy;
    vec3 energyCompensation = 1.0 + f0 * (1.0 / max(dfg.y, 1e-4) - 1.0);

    // Offset shadow rays along the true triangle normal: the smooth shading
    // normal can point below the surface and make the ray hit its own
    // triangle. Derivatives must be taken outside the branch below.
    vec3 geometricNormal =
        normalize(cross(dFdx(inRelativePosition), dFdy(inRelativePosition)));
    if (dot(geometricNormal, normal) < 0.0) {
        geometricNormal = -geometricNormal;
    }

    // All lighting is relative to a sun illuminance of 1 (as in the
    // atmosphere LUTs) and scaled by the real sun at the end.
    vec3 position = atmospherePosition(worldPosition);
    float height = length(position);
    vec3 up = position / height;
    vec3 sunAtSurface = transmittanceToSun(height, dot(up, L));

    // Diffuse and specular are kept apart: a blended surface (glass) fades
    // its diffuse with alpha but keeps its full reflection.
    vec3 diffuseLight = vec3(0.0);
    vec3 specularLight = vec3(0.0);
    float NoL = clamp(dot(N, L), 0.0, 1.0);
    if (NoL > 0.0 && dot(geometricNormal, L) > 0.0) {
        float visibility = sunVisibility(worldPosition, geometricNormal);
        vec3 H = normalize(V + L);
        float NoH = clamp(dot(N, H), 0.0, 1.0);
        float LoH = clamp(dot(L, H), 0.0, 1.0);

        vec3 specular = distributionGgx(NoH, a) *
                        visibilitySmithGgxCorrelated(NoV, NoL, a) *
                        fresnelSchlick(f0, LoH) * energyCompensation;
        vec3 sunLight = sunAtSurface * (NoL * visibility);
        diffuseLight = diffuseColor / kPi * sunLight;
        specularLight = specular * sunLight;
    }

    // Ambient from the sky. Diffuse: sky irradiance from above, light bounced
    // off the ground from below. Specular: the sky in the reflected
    // direction, blurred toward the irradiance as roughness grows (until
    // reflections are ray traced).
    vec3 skyIrradiance = pc.scene.skyIrradiance.irradiance;
    vec3 groundIrradiance =
        kGroundAlbedo * (sunAtSurface * max(L.y, 0.0) + skyIrradiance);
    vec3 diffuseIrradiance = mix(groundIrradiance, skyIrradiance, N.y * 0.5 + 0.5);
    vec3 R = reflect(-V, N);
    vec3 skyRadiance = mix(skyLuminance(position, R), skyIrradiance / kPi, a);
    vec3 specularReflectance = mix(dfg.xxx, dfg.yyy, f0) * energyCompensation;
    diffuseLight += diffuseColor / kPi * diffuseIrradiance * s.occlusion;
    specularLight += specularReflectance * skyRadiance * s.occlusion;

#ifdef ALPHA_BLEND
    // Premultiplied alpha (blend: one, one-minus-source-alpha).
    float coverage = s.alpha;
    vec3 luminance = diffuseLight * coverage + specularLight;
#else
    float coverage = 1.0;
    vec3 luminance = diffuseLight + specularLight;
#endif

    // Aerial perspective: haze between the camera and this point.
    float distanceKm = length(inRelativePosition) * 0.001;
    float slice = distanceKm / kAerialPerspectiveKmPerSlice;
    float weight = 1.0;
    if (slice < 0.5) {
        // Fade in over the first half slice instead of jumping.
        weight = clamp(slice * 2.0, 0.0, 1.0);
        slice = 0.5;
    }
    float w = sqrt(slice / kAerialPerspectiveSlices);
    vec4 haze = textureLod(aerialPerspectiveLut,
                           vec3(gl_FragCoord.xy * pc.scene.inverseResolution, w), 0.0);
    luminance = luminance * mix(1.0, haze.a, weight) + haze.rgb * (weight * coverage);

    // Emissive is already in physical units (cd/m^2), not relative to the sun.
    vec3 exposed = luminance * pc.scene.sunIlluminance + s.emissive;
    outColor = vec4(exposed * pc.scene.exposure.exposure, coverage);

    vec2 currentUv = inCurrentClip.xy / inCurrentClip.w * 0.5;
    vec2 previousUv = inPreviousClip.xy / inPreviousClip.w * 0.5;
    outMotion = previousUv - currentUv;
}
