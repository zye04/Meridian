#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "material.glsl"

// Alpha test for the cascades' masked bucket, as mesh.frag's ALPHA_MASK.

layout(location = 0) in vec2 inUv;
layout(location = 1) flat in uint inMaterialIndex;

void main() {
    Material material = pc.scene.materials.materials[inMaterialIndex];
    float alpha = material.baseColorFactor.a;
    if (material.baseColorTexture != kNoTexture) {
        alpha *= texture(textures[nonuniformEXT(material.baseColorTexture)], inUv).a;
    }
    if (alpha < material.alphaCutoff) {
        discard;
    }
}
