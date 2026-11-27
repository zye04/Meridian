#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#ifdef DEBUG_ID
#extension GL_ARB_fragment_shader_interlock : require
layout(pixel_interlock_unordered) in;
// The debug stores would otherwise move the depth test after shading.
layout(early_fragment_tests) in;
#endif
#include "common.glsl"
#include "atmosphere.glsl"
#include "material.glsl"
#include "terrain.glsl"
#include "lighting.glsl"
#include "voxel_lod.glsl"
#include "debug_id.glsl"
#include "voxel_shading.glsl"

// A voxel of a voxel cluster (voxel_instance.mesh draws it as a
// camera-facing quad), shaded flat at its centre as the leaves it stands in
// for: their mean reflected and transmitted colour (with the placed
// instance's tint, as mesh.frag), roughness and normal lobe. The aligned
// part of the lobe shades as one two-sided leaf through the same lighting
// as the triangles; the rest as leaves facing every way (the spherical-leaf
// phase, gi.glsl). The sun reaches it as it reaches a leaf, from one voxel
// toward the sun, its own model among the casters; the model shades itself
// toward the sky through the voxel's optical depth out of it; the terrain's
// sun visibility comes per brick from voxel_bricks.comp.

layout(location = 0) flat in vec3 inRelative;
layout(location = 1) flat in uvec4 inVoxel;
layout(location = 2) flat in uvec4 inInfo;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;
// The guides and the sun's exposed radiance, as in mesh.frag; the
// transparency mask (location 2) is not written (opaque).
layout(location = 3) out vec4 outDiffuseAlbedo;
layout(location = 4) out vec4 outSpecularAlbedo;
layout(location = 5) out vec4 outNormalRoughness;
layout(location = 6) out vec4 outSunLight;

void main() {
    SceneOutputs o = shadeVoxelPixel(inRelative, inVoxel, inInfo);
    outColor = o.color;
    outMotion = o.motion;
    outDiffuseAlbedo = o.diffuseAlbedo;
    outSpecularAlbedo = o.specularAlbedo;
    outNormalRoughness = o.normalRoughness;
    outSunLight = o.sunLight;
    writeDebugId(kDebugVoxels, inInfo.x, inInfo.w, inRelative, ~0u);
#ifdef DEBUG_ID
    beginInvocationInterlockARB();
    writeDebugLight(gl_FragCoord.z, o.debugSunDiffuse, o.debugSunSpecular, o.debugAmbientDiffuse,
                    o.debugAmbientSpecular, vec2(0.0));
    endInvocationInterlockARB();
#endif
}
