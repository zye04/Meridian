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
    mat4 viewProjection;
    vec3 sunDirection;
    vec3 sunColor;
    vec3 skyColor;
};

layout(push_constant) uniform PushConstants {
    mat4 model;
    VertexBuffer vertexBuffer;
    SceneBuffer scene;
} pc;
