#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outNormal;

void main() {
    Vertex vertex = pc.vertexBuffer.vertices[gl_VertexIndex];
    vec4 world = pc.model * vec4(vertex.position, 1.0);
    gl_Position = pc.scene.viewProjection * world;
    outWorldPosition = world.xyz;
    outNormal = mat3(pc.model) * vertex.normal;
}
