// Shared by all mesh shaders. Layouts must match Vertex, SceneData and
// PushConstants in C++.
#extension GL_EXT_buffer_reference : require
#include "exposure.glsl"

struct Vertex {
    vec3 position;
    float uvX;
    vec3 normal;
    float uvY;
    // xyz: tangent direction; w: bitangent sign (glTF/MikkTSpace convention).
    vec4 tangent;
};

// glTF metallic-roughness material. Must match GpuMaterial in C++.
const uint kNoTexture = 0xFFFFFFFFu;
const uint kMaterialProceduralTerrain = 1u;
const uint kMaterialAlphaMask = 2u;

struct Material {
    vec4 baseColorFactor;
    vec3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float normalScale;
    float occlusionStrength;
    float alphaCutoff;
    uint baseColorTexture;
    uint metallicRoughnessTexture;
    uint normalTexture;
    uint occlusionTexture;
    uint emissiveTexture;
    uint flags;
};

layout(buffer_reference, std430) readonly buffer MaterialBuffer {
    Material materials[];
};

layout(buffer_reference, std430) readonly buffer VertexBuffer {
    Vertex vertices[];
};

layout(buffer_reference, std430) readonly buffer IndexBuffer {
    uint indices[];
};

// Per TLAS instance (instance custom index), so ray queries can look up the
// surface they hit. Must match InstanceInfo in C++.
struct InstanceInfo {
    VertexBuffer vertexBuffer;
    IndexBuffer indexBuffer;
    uint materialIndex;
    uint pad;
};

layout(buffer_reference, std430) readonly buffer InstanceBuffer {
    InstanceInfo instances[];
};

// Must match AtmosphereParameters in C++. Distances in km, coefficients
// per km.
struct AtmosphereParameters {
    vec3 rayleighScattering;
    float bottomRadius;
    vec3 mieScattering;
    float topRadius;
    vec3 mieExtinction;
    float rayleighDensityExpScale;
    vec3 absorptionExtinction;
    float mieDensityExpScale;
    vec3 groundAlbedo;
    float miePhaseG;
    // Ozone: a tent profile of two linear layers.
    float absorptionLayerWidth;
    float absorption0Linear;
    float absorption0Constant;
    float absorption1Linear;
    float absorption1Constant;
};

// Sky irradiance on an upward-facing surface, integrated each frame from the
// sky-view LUT (sun illuminance 1).
layout(buffer_reference, std430) buffer SkyIrradianceBuffer {
    vec3 irradiance;
};

layout(buffer_reference, std430) readonly buffer SceneBuffer {
    // Jittered for temporal anti-aliasing; used to rasterize.
    mat4 viewProjection;
    // Without jitter, this frame and last frame; used for motion vectors.
    mat4 currentViewProjection;
    mat4 previousViewProjection;
    vec3 sunDirection;
    float pad0;
    vec3 sunIlluminance;
    uint frameIndex;
    vec3 cameraPosition;
    float sunAngularRadius;
    // Device address of the scene's top-level acceleration structure.
    uvec2 tlas;
    // Written by last frame's exposure pass; the scene is shaded with it
    // ("pre-exposure") so the HDR target holds display-range values.
    ExposureBuffer exposure;
    SkyIrradianceBuffer skyIrradiance;
    vec2 inverseResolution;
    InstanceBuffer instances;
    uvec2 pad2;
    AtmosphereParameters atmosphere;
};

layout(push_constant) uniform PushConstants {
    mat4 model;
    VertexBuffer vertexBuffer;
    SceneBuffer scene;
    MaterialBuffer materialBuffer;
    uint materialIndex;
} pc;
