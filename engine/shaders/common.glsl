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
const uint kMaterialTerrain = 1u;
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

// One per uploaded mesh. Its geometry lives in the shared vertex and index
// pools. Must match MeshInfo in C++.
const uint kDrawBuckets = 4u;
const uint kDrawBucketBlended = 4u;

struct MeshInfo {
    // Bounding sphere in model space, for culling.
    vec3 boundsCenter;
    float boundsRadius;
    uint firstIndex;
    uint indexCount;
    int vertexOffset;
    uint materialIndex;
    // Cluster draw list (see cluster_cull.comp); kDrawBucketBlended is drawn from
    // the CPU, sorted back to front.
    uint drawBucket;
    // Its cluster LOD hierarchy (none for blended meshes).
    uint firstCluster;
    uint clusterCount;
    uint pad0;
};

layout(buffer_reference, std430) readonly buffer MeshBuffer {
    MeshInfo meshes[];
};

// Up to 128 triangles of one level of a mesh's LOD DAG, in mesh space.
// Must match ClusterInfo in C++ (see meridian::Cluster).
struct ClusterInfo {
    // Culling sphere.
    vec3 boundsCenter;
    float boundsRadius;
    // LOD spheres of the coarser replacement (error FLT_MAX if none) and of
    // the finer group this cluster replaces (error -1 for original geometry).
    vec3 selfCenter;
    float selfRadius;
    vec3 refinedCenter;
    float refinedRadius;
    float selfError;
    float refinedError;
    // Into ClusterVertexBuffer (mesh-local vertex indices) and
    // ClusterTriangleBuffer (three 8-bit cluster-local indices per uint).
    uint firstVertex;
    uint firstTriangle;
    uint vertexCount;
    uint triangleCount;
    uint pad0;
    uint pad1;
};

layout(buffer_reference, std430) readonly buffer ClusterBuffer {
    ClusterInfo clusters[];
};

layout(buffer_reference, std430) readonly buffer ClusterVertexBuffer {
    uint vertices[];
};

layout(buffer_reference, std430) readonly buffer ClusterTriangleBuffer {
    uint triangles[];
};

// Culled clusters, kDrawBuckets lists per culling phase of
// kMaxDrawnClusters entries each: (instance, cluster).
const uint kMaxDrawnClusters = 1u << 20;

layout(buffer_reference, std430) buffer DrawListBuffer {
    uvec2 entries[];
};

// One per placed instance; gl_InstanceIndex and the TLAS instance custom
// index both refer to it. Must match InstanceData in C++.
struct InstanceData {
    mat4 transform;
    uint meshIndex;
    uint pad0;
    uint pad1;
    uint pad2;
};

layout(buffer_reference, std430) readonly buffer InstanceBuffer {
    InstanceData instances[];
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

// Must match GpuTerrainLayer in C++.
struct TerrainLayer {
    uint baseColor;
    uint normal;
    uint arm;
    uint height;
    float tileSize;
    float pad0;
    float pad1;
    float pad2;
};

// Must match GpuTerrain in C++. Layers: soil, needles, rock.
struct Terrain {
    TerrainLayer layers[3];
    uint enabled;
    uint pad0;
    uint pad1;
    uint pad2;
};

layout(buffer_reference, std430) readonly buffer SceneBuffer {
    // All view matrices here leave out the camera translation: they take
    // positions relative to the camera (world - cameraPosition, or
    // previousCameraPosition for the previous ones), which stay precise
    // far from the origin.
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
    vec3 previousCameraPosition;
    float pad2;
    // Device address of the scene's top-level acceleration structure.
    uvec2 tlas;
    // Written by last frame's exposure pass; the scene is shaded with it
    // ("pre-exposure") so the HDR target holds display-range values.
    ExposureBuffer exposure;
    SkyIrradianceBuffer skyIrradiance;
    vec2 inverseResolution;
    // Shared geometry pools and the GPU scene tables.
    VertexBuffer vertices;
    IndexBuffer indices;
    MeshBuffer meshes;
    InstanceBuffer instances;
    MaterialBuffer materials;
    uint instanceCount;
    uint pad1;
    // Camera-relative planes (xyz normal, w distance), inside where >= 0.
    vec4 frustumPlanes[6];
    AtmosphereParameters atmosphere;
    Terrain terrain;
    // For occlusion culling: unjittered view (this frame's and last frame's,
    // which made the pyramid phase 0 tests against) and the projection's
    // x/y scale and near plane.
    mat4 view;
    mat4 previousView;
    vec2 projectionScale;
    float nearPlane;
    // LOD selection: the largest allowed simplification error on screen.
    float lodErrorPixels;
    ClusterBuffer clusters;
    ClusterVertexBuffer clusterVertices;
    ClusterTriangleBuffer clusterTriangles;
    DrawListBuffer drawLists;
};

// Shaders with their own push constants (cull.comp) define
// MERIDIAN_CUSTOM_PUSH_CONSTANTS before including this file.
#ifndef MERIDIAN_CUSTOM_PUSH_CONSTANTS
layout(push_constant) uniform PushConstants {
    SceneBuffer scene;
    // The cluster draw list mesh.mesh draws.
    uint drawList;
} pc;
#endif
