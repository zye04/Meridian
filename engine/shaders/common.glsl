// Shared by all mesh shaders. Layouts must match Vertex, SceneData and
// PushConstants in C++.
#extension GL_EXT_buffer_reference : require

struct Vertex {
    vec3 position;
    float uvX;
    vec3 normal;
    float uvY;
};

layout(buffer_reference, std430) readonly buffer VertexBuffer {
    Vertex vertices[];
};

layout(buffer_reference, std430) readonly buffer SceneBuffer {
    // Jittered for temporal anti-aliasing; used to rasterize.
    mat4 viewProjection;
    // Without jitter, this frame and last frame; used for motion vectors.
    mat4 currentViewProjection;
    mat4 previousViewProjection;
    vec3 sunDirection;
    float exposure;
    vec3 sunIlluminance;
    uint frameIndex;
    vec3 skyLuminance;
    float sunAngularRadius;
    // Device address of the scene's top-level acceleration structure.
    uvec2 tlas;
};

layout(push_constant) uniform PushConstants {
    mat4 model;
    VertexBuffer vertexBuffer;
    SceneBuffer scene;
} pc;
