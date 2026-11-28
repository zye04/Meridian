#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) out vec3 outRelativePosition;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec4 outCurrentClip;
layout(location = 3) out vec4 outPreviousClip;
layout(location = 4) out vec2 outUv;
layout(location = 5) out vec4 outTangent;
layout(location = 6) flat out uint outMaterialIndex;
layout(location = 7) flat out float outLodError;
layout(location = 8) out float outHeight;
layout(location = 9) flat out vec3 outTint;
layout(location = 10) flat out float outStreamFade;
layout(location = 12) out vec2 outCrown;

void main() {
    // Blended draws set firstInstance to the instance's index; the indexed
    // draw has already added the mesh's vertexOffset to gl_VertexIndex.
    LoadedInstance instance = loadInstance(pc.scene, uint(gl_InstanceIndex));
    MeshInfo mesh = pc.scene.meshes.meshes[instance.meshOrPrototype];
    Vertex vertex = loadVertex(pc.scene, mesh, uint(gl_VertexIndex));

    vec3 world = (instance.transform * vec4(vertex.position, 1.0)).xyz;
    vec3 relative = world - pc.scene.cameraPosition;
    vec4 displayed = displayedPosition(pc.scene, relative);
    gl_Position = pc.scene.viewProjection * displayed;
    outRelativePosition = relative;
    // Normals use the inverse transpose so non-uniform scale stays correct.
    mat3 normalMatrix = transpose(inverse(mat3(instance.transform)));
    outNormal = normalMatrix * vertex.normal;
    outTangent = vec4(mat3(instance.transform) * vertex.tangent.xyz, vertex.tangent.w);
    outUv = vec2(vertex.uvX, vertex.uvY);
    outMaterialIndex = mesh.materialIndex;
    outLodError = 0.0;
    outHeight = 1.0;
    outCrown = vec2(1.0, -1.0);
    outTint = vec3(1.0);
    outStreamFade = instance.fade;
    // Static geometry for now: only the camera moves between frames.
    outCurrentClip = pc.scene.currentViewProjection * displayed;
    outPreviousClip = pc.scene.previousViewProjection *
                      displayedPosition(pc.scene, world - pc.scene.previousCameraPosition);
}
