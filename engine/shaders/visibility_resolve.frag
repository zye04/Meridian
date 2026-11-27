#version 460
// The visibility buffer's resolve: one fullscreen pass after the late scene
// lists that shades each pixel whose id (scene attachment 7) names a cluster
// triangle, once, with the forward path's functions (cluster_shading.glsl).
// The triangle is rebuilt from its three vertices with the mesh shader's own
// function (cluster_vertex.glsl), its perspective-correct barycentrics and
// their screen derivatives from visibility.glsl.
//
// No implicit derivatives here: the fullscreen pass's pixel quads span
// unrelated triangles. A shared function that takes one fails the build.
#define dFdx(x) VISIBILITY_RESOLVE_FORBIDS_dFdx
#define dFdy(x) VISIBILITY_RESOLVE_FORBIDS_dFdy
#define dFdxCoarse(x) VISIBILITY_RESOLVE_FORBIDS_dFdxCoarse
#define dFdyCoarse(x) VISIBILITY_RESOLVE_FORBIDS_dFdyCoarse
#define fwidth(x) VISIBILITY_RESOLVE_FORBIDS_fwidth
#define texture(a, b) VISIBILITY_RESOLVE_FORBIDS_implicit_lod
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#include "common.glsl"
#include "atmosphere.glsl"
#include "material.glsl"
#include "terrain.glsl"
#include "lighting.glsl"
#include "farfield.glsl"
#include "cluster_vertex.glsl"
#include "visibility.glsl"
#define SURFACE_FRAGMENT
#include "surface.glsl"
#include "cluster_shading.glsl"

layout(set = 2, binding = 0) uniform usampler2D visibilityIds;
// Barycentrics that needed clamping, counted on every 16th frame: the
// rasterizer and this pass must agree on the triangle's clip positions.
layout(set = 2, binding = 1) buffer ResolveStats {
    uint resolvedPixels;
    uint clampedPixels;
} stats;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;
layout(location = 3) out vec4 outDiffuseAlbedo;
layout(location = 4) out vec4 outSpecularAlbedo;
layout(location = 5) out vec4 outNormalRoughness;
layout(location = 6) out vec4 outSunLight;

const float kClampTolerance = 1e-3;

void main() {
    uvec2 id = texelFetch(visibilityIds, ivec2(gl_FragCoord.xy), 0).xy;
    if (visibilityKind(id) != kVisibilityClusterTriangle) {
        discard;
    }
    SceneBuffer scene = pc.scene;
    ClusterContext c =
        loadClusterContext(scene, visibilityInstance(id), visibilityCluster(id));
    uint packed = scene.clusterTriangles.triangles[c.cluster.firstTriangle + visibilityTriangle(id)];
    ClusterVertex v0 = clusterVertex(scene, c, packed & 0xFFu);
    ClusterVertex v1 = clusterVertex(scene, c, (packed >> 8) & 0xFFu);
    ClusterVertex v2 = clusterVertex(scene, c, packed >> 16);

    Barycentrics r = barycentricsAt(v0.clip, v1.clip, v2.clip, gl_FragCoord.xy,
                                    vec2(textureSize(visibilityIds, 0)));
    if ((scene.frameIndex & 15u) == 0u) {
        atomicAdd(stats.resolvedPixels, 1u);
        if (r.outside > kClampTolerance) {
            atomicAdd(stats.clampedPixels, 1u);
        }
    }
    vec3 b = max(r.b, vec3(0.0));
    b /= b.x + b.y + b.z;

    ClusterPixel p;
    p.relative = b.x * v0.relative + b.y * v1.relative + b.z * v2.relative;
    p.relativeDx = r.dbdx.x * v0.relative + r.dbdx.y * v1.relative + r.dbdx.z * v2.relative;
    p.relativeDy = r.dbdy.x * v0.relative + r.dbdy.y * v1.relative + r.dbdy.z * v2.relative;
    p.uv = b.x * v0.uv + b.y * v1.uv + b.z * v2.uv;
    p.uvDx = r.dbdx.x * v0.uv + r.dbdx.y * v1.uv + r.dbdx.z * v2.uv;
    p.uvDy = r.dbdy.x * v0.uv + r.dbdy.y * v1.uv + r.dbdy.z * v2.uv;
    // terrainNormalUv is linear in the flat world position.
    Terrain terrain = scene.terrain;
    vec2 terrainUvScale =
        1.0 / (terrain.spacing * vec2(float(terrain.width), float(terrain.height)));
    p.terrainUvDx = p.relativeDx.xz * terrainUvScale;
    p.terrainUvDy = p.relativeDy.xz * terrainUvScale;
    p.normal = b.x * v0.normal + b.y * v1.normal + b.z * v2.normal;
    p.normalDx = r.dbdx.x * v0.normal + r.dbdx.y * v1.normal + r.dbdx.z * v2.normal;
    p.normalDy = r.dbdy.x * v0.normal + r.dbdy.y * v1.normal + r.dbdy.z * v2.normal;
    p.geometricNormal = cross(v1.relative - v0.relative, v2.relative - v0.relative);
    p.frontFacing = r.frontFacing;
    p.tangent = b.x * v0.tangent + b.y * v1.tangent + b.z * v2.tangent;
    p.materialIndex = c.mesh.materialIndex;
    p.lodError = c.lodError;
    p.height = b.x * v0.height + b.y * v1.height + b.z * v2.height;
    p.tint = c.tint;
    p.streamFade = c.instance.fade;
    p.crownVisibility =
        b.x * v0.crownVisibility + b.y * v1.crownVisibility + b.z * v2.crownVisibility;
    uint sunCode = visibilityTerrainSunCode(id);
    p.terrainSun = sunCode == kTerrainSunPerFragment ? -1.0 : float(sunCode);
    p.currentClip = b.x * v0.currentClip + b.y * v1.currentClip + b.z * v2.currentClip;
    p.previousClip = b.x * v0.previousClip + b.y * v1.previousClip + b.z * v2.previousClip;

    SceneOutputs o = shadeClusterPixel(p);
    outColor = o.color;
    outMotion = o.motion;
    outDiffuseAlbedo = o.diffuseAlbedo;
    outSpecularAlbedo = o.specularAlbedo;
    outNormalRoughness = o.normalRoughness;
    outSunLight = o.sunLight;
}
