// Material textures, bound once for the whole frame ("bindless"): every
// texture lives in one array and materials refer to it by index.
#ifndef MATERIAL_GLSL
#define MATERIAL_GLSL

#extension GL_EXT_nonuniform_qualifier : require

// Split-sum DFG term for image-based lighting and multiple-scattering
// energy compensation, indexed by (N.V, perceptual roughness).
layout(set = 1, binding = 0) uniform sampler2D dfgLut;
layout(set = 1, binding = 1) uniform sampler2D textures[];

#endif
