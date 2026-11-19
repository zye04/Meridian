#version 460
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
};

layout(push_constant) uniform PushConstants {
    mat4 model;
    VertexBuffer vertexBuffer;
    SceneBuffer scene;
} pc;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outNormal;

void main() {
    Vertex vertex = pc.vertexBuffer.vertices[gl_VertexIndex];
    vec4 world = pc.model * vec4(vertex.position, 1.0);
    gl_Position = pc.scene.viewProjection * world;
    outWorldPosition = world.xyz;
    outNormal = mat3(pc.model) * vertex.normal;
}
