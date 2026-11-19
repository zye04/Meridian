#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec4 outCurrentClip;
layout(location = 3) out vec4 outPreviousClip;

void main() {
    Vertex vertex = pc.vertexBuffer.vertices[gl_VertexIndex];
    vec4 world = pc.model * vec4(vertex.position, 1.0);
    gl_Position = pc.scene.viewProjection * world;
    outWorldPosition = world.xyz;
    outNormal = mat3(pc.model) * vertex.normal;
    // Static geometry for now: only the camera moves between frames.
    outCurrentClip = pc.scene.currentViewProjection * world;
    outPreviousClip = pc.scene.previousViewProjection * world;
}
