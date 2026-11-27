#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_mesh_shader : require
// The visibility pass's cluster fragments (mesh_visibility.mesh): depth and
// the triangle's id, nothing shaded. VISIBILITY_MASKED adds the alpha test
// of the masked depth pre-pass (mesh.frag DEPTH_ONLY), which it replaces: a
// masked list is drawn once.
#include "common.glsl"
#include "material.glsl"

#ifdef VISIBILITY_MASKED
layout(location = 4) in vec2 inUv;
layout(location = 6) flat in uint inMaterialIndex;
layout(location = 14) in float inVertexAlpha;
#else
// No discard and no side effects: the depth test runs before this shader.
layout(early_fragment_tests) in;
#endif
layout(location = 15) perprimitiveEXT flat in uvec2 inVisibility;

layout(location = 7) out uvec2 outVisibility;

void main() {
#ifdef VISIBILITY_MASKED
    Material cutoutMaterial = pc.scene.materials.materials[inMaterialIndex];
    float cutoutAlpha = cutoutMaterial.baseColorFactor.a * inVertexAlpha;
    if (cutoutMaterial.baseColorTexture != kNoTexture) {
        cutoutAlpha *= texture(textures[nonuniformEXT(cutoutMaterial.baseColorTexture)], inUv,
                               pc.scene.mipBias + pc.scene.cutoutMipBias).a;
    }
    if (cutoutAlpha < cutoutMaterial.alphaCutoff) {
        discard;
    }
#endif
    outVisibility = inVisibility;
}
