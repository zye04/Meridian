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
    // The camera position wrapped by the layer's period (renderer_frame.cpp).
    float offset[3];
};

layout(buffer_reference, std430) readonly buffer HeightPyramidBuffer {
    uint minMax[];
};

// Selected terrain nodes, (level, x, z) packed by terrain.glsl.
layout(buffer_reference, std430) buffer TerrainNodeBuffer {
    uvec2 nodes[];
};

// Must match GpuTerrain in C++. Layers: soil, needles, rock.
struct Terrain {
    TerrainLayer layers[3];
    // A TerrainMaterial is set (procedural colors otherwise).
    uint enabled;
    float snowLine;
    // Bindless textures (material.glsl): the samples (R16_UNORM, row 0 =
    // north; height = minHeight + sample * heightScale) and the normal map
    // (RG16_SNORM normal x and z, mipmapped, mirrored-repeat sampler).
    uint heightTexture;
    uint normalTexture;
    // Min/max pyramid levels >= 1 (min | max << 16), level k starting at
    // pyramidOffsets[k].
    HeightPyramidBuffer pyramid;
    TerrainNodeBuffer nodes;
    uint pyramidOffsets[16];
    uint pyramidLevels;
    uint width;
    uint height;
    float spacing;
    float minHeight;
    float heightScale;
    // Level 0's LOD range (m); each level doubles it.
    float lodRange0;
    uint rootLevel;
    // The camera's x/z wrapped by the mix noise's period (renderer_frame.cpp).
    vec2 noiseOffset;
    vec2 pad;
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

// Curvature as a display warp: the world stays flat (lighting, shadow rays,
// the TLAS), but rasterized positions drop by d^2 / 2R below the camera's
// horizontal plane, so far ground sinks under the horizon and meets the
// spherical atmosphere. `relative` is relative to the camera it is seen from.
float curvatureDrop(SceneBuffer scene, vec3 relative) {
    return dot(relative.xz, relative.xz) / (2000.0 * scene.atmosphere.bottomRadius);
}

vec4 displayedPosition(SceneBuffer scene, vec3 relative) {
    return vec4(relative.x, relative.y - curvatureDrop(scene, relative), relative.z, 1.0);
}

// Shaders with their own push constants (cull.comp) define
// MERIDIAN_CUSTOM_PUSH_CONSTANTS before including this file.
#ifndef MERIDIAN_CUSTOM_PUSH_CONSTANTS
layout(push_constant) uniform PushConstants {
    SceneBuffer scene;
    // The cluster draw list mesh.mesh draws.
    uint drawList;
} pc;
#endif
