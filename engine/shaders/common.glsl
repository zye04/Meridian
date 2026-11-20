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
    // The mesh it belongs to: placed instances name only a prototype.
    uint mesh;
    uint pad0;
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

// One per hand-placed instance (id < kPlacedBase). Must match InstanceData
// in C++.
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

// One placed object (id >= kPlacedBase). Must match PlacedInstance in
// placement.h: rotation xy, then z | scale (half float) << 16, where
// rotation is a unit quaternion's x, y, z in snorm16 (w >= 0 rebuilt).
struct PlacedInstance {
    float positionX;
    float positionY;
    float positionZ;
    uint rotationXY;
    uint rotationZScale;
    uint prototype;
};

layout(buffer_reference, std430) buffer PlacedInstanceBuffer {
    PlacedInstance placed[];
};

// A 64 m placement cell (x/z bounds from its index in the grid): the height
// range of its instances' bounding spheres and its run of the placed table.
// Must match Cell in C++.
struct Cell {
    float minY;
    float maxY;
    uint firstInstance;
    uint instanceCount;
};

layout(buffer_reference, std430) buffer CellBuffer {
    Cell cells[];
};

// Must match kCellSize in placement.h.
const float kCellSize = 64.0;

// A placed prototype. Must match PrototypeInfo in C++: its meshes
// (PrototypeMeshBuffer from firstMesh; a ray hit's geometry index counts
// from there), their bounding sphere and BLAS, the projected size below
// which it becomes an impostor (0: never) and its draw distance (0: none).
struct PrototypeInfo {
    uint firstMesh;
    uint meshCount;
    vec3 boundsCenter;
    float boundsRadius;
    uvec2 blasAddress;
    float transitionSize;
    float maxDrawDistance;
};

layout(buffer_reference, std430) readonly buffer PrototypeBuffer {
    PrototypeInfo prototypes[];
};

layout(buffer_reference, std430) readonly buffer PrototypeMeshBuffer {
    uint meshes[];
};

vec4 placedRotation(PlacedInstance instance) {
    vec3 xyz = vec3(unpackSnorm2x16(instance.rotationXY),
                    unpackSnorm2x16(instance.rotationZScale).x);
    float w = sqrt(max(0.0, 1.0 - dot(xyz, xyz)));
    return normalize(vec4(xyz, w));
}

float placedScale(PlacedInstance instance) {
    return unpackHalf2x16(instance.rotationZScale >> 16u).x;
}

vec3 rotateByQuaternion(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

mat4 placedTransform(PlacedInstance instance) {
    vec4 q = placedRotation(instance);
    float s = placedScale(instance);
    return mat4(vec4(rotateByQuaternion(q, vec3(s, 0.0, 0.0)), 0.0),
                vec4(rotateByQuaternion(q, vec3(0.0, s, 0.0)), 0.0),
                vec4(rotateByQuaternion(q, vec3(0.0, 0.0, s)), 0.0),
                vec4(instance.positionX, instance.positionY, instance.positionZ, 1.0));
}

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
    float occlusionStrength;
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
    // Ground turns to rock from normal.y = y down to x (kRockNormalY).
    vec2 rockNormalY;
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
    // Placement (setPlacement): the placed table and the cells, row-major
    // from the north-west; cell (cellGrid.x + column, cellGrid.y + row)
    // spans [cell, cell + 1) * 64 m.
    PlacedInstanceBuffer placedInstances;
    CellBuffer cells;
    ivec2 cellGrid;
    uvec2 cellGridSize;
    PrototypeBuffer prototypes;
    PrototypeMeshBuffer prototypeMeshes;
    uint placedCount;
    // Cells: the largest layer draw distance (0: some layer has none) and
    // how far instances' bounds reach past their cell horizontally.
    float cellDrawDistance;
    float cellMargin;
    uint pad3;
};

// Largest axis scale of a transform: a conservative bound under
// non-uniform scale.
float maxScale(mat4 transform) {
    mat3 m = mat3(transform);
    return sqrt(max(dot(m[0], m[0]), max(dot(m[1], m[1]), dot(m[2], m[2]))));
}

// One instance id space for culling, draw lists, visibility and the TLAS
// custom index: ids below kPlacedBase index InstanceBuffer, ids from it the
// placed table. Must match kPlacedBase in C++.
const uint kPlacedBase = 65536u;

struct LoadedInstance {
    mat4 transform;
    // maxScale(transform).
    float scale;
    // A mesh index, or a prototype index when placed.
    uint meshOrPrototype;
    bool placed;
};

LoadedInstance loadInstance(SceneBuffer scene, uint id) {
    LoadedInstance loaded;
    if (id >= kPlacedBase) {
        PlacedInstance placed = scene.placedInstances.placed[id - kPlacedBase];
        loaded.transform = placedTransform(placed);
        loaded.scale = placedScale(placed);
        loaded.meshOrPrototype = placed.prototype;
        loaded.placed = true;
        return loaded;
    }
    InstanceData instance = scene.instances.instances[id];
    loaded.transform = instance.transform;
    loaded.scale = maxScale(instance.transform);
    loaded.meshOrPrototype = instance.meshIndex;
    loaded.placed = false;
    return loaded;
}

// The mesh of an instance's BLAS geometry (a ray hit's geometry index):
// hand-placed instances have one mesh, placed ones their prototype's.
uint instanceMesh(SceneBuffer scene, uint id, uint geometry) {
    if (id < kPlacedBase) {
        return scene.instances.instances[id].meshIndex;
    }
    uint prototype = scene.placedInstances.placed[id - kPlacedBase].prototype;
    return scene.prototypeMeshes.meshes[scene.prototypes.prototypes[prototype].firstMesh +
                                        geometry];
}

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

// Whether a box relative to the camera, as displayed under the curvature
// warp, touches the view frustum: the drop is smallest at the box's nearest
// point and largest at its farthest corner.
bool displayedBoxInFrustum(SceneBuffer scene, vec3 lo, vec3 hi) {
    vec2 nearest = max(max(lo.xz, -hi.xz), vec2(0.0));
    vec2 farthest = max(abs(lo.xz), abs(hi.xz));
    lo.y -= curvatureDrop(scene, vec3(farthest.x, 0.0, farthest.y));
    hi.y -= curvatureDrop(scene, vec3(nearest.x, 0.0, nearest.y));
    for (int i = 0; i < 6; ++i) {
        vec4 plane = scene.frustumPlanes[i];
        vec3 farthestAlong = mix(lo, hi, greaterThanEqual(plane.xyz, vec3(0.0)));
        if (dot(plane.xyz, farthestAlong) + plane.w < 0.0) {
            return false;
        }
    }
    return true;
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
