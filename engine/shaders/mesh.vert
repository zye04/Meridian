#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec4 outCurrentClip;
layout(location = 3) out vec4 outPreviousClip;
layout(location = 4) out vec2 outUv;
layout(location = 5) out vec4 outTangent;

void main() {
    Vertex vertex = pc.vertexBuffer.vertices[gl_VertexIndex];
    vec4 world = pc.model * vec4(vertex.position, 1.0);
    gl_Position = pc.scene.viewProjection * world;
    outWorldPosition = world.xyz;
    // Normals use the inverse transpose so non-uniform scale stays correct.
    mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
    outNormal = normalMatrix * vertex.normal;
    outTangent = vec4(mat3(pc.model) * vertex.tangent.xyz, vertex.tangent.w);
    outUv = vec2(vertex.uvX, vertex.uvY);
    // Static geometry for now: only the camera moves between frames.
    outCurrentClip = pc.scene.currentViewProjection * world;
    outPreviousClip = pc.scene.previousViewProjection * world;
}
