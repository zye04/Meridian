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
    float diffuseTransmission;
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
    // Its cluster LOD hierarchy (none for blended meshes) and that
    // hierarchy's DAG levels (ClusterLevelBuffer).
    uint firstCluster;
    uint clusterCount;
    uint firstLevel;
    uint levelCount;
    uint pad0;
    uint pad1;
    uint pad2;
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

// A node of a BVH over one DAG level's cluster groups. Must match
// ClusterNodeInfo in C++ (see meridian::ClusterBvhNode). Leaves' children
// are clusters, inner nodes' children nodes.
struct ClusterNodeInfo {
    // Encloses the LOD spheres of every cluster below, with their largest
    // self error.
    vec3 lodCenter;
    float lodRadius;
    // Encloses the culling spheres of every cluster below.
    vec3 boundsCenter;
    float boundsRadius;
    float lodError;
    uint firstChild;
    uint childCount;
    uint leaf;
};

layout(buffer_reference, std430) readonly buffer ClusterNodeBuffer {
    ClusterNodeInfo nodes[];
};

// One DAG level of a mesh. Must match ClusterLevelGpu in C++ (see
// meridian::ClusterLevelInfo).
struct ClusterLevelGpu {
    uint rootNode;
    float minError;
    float maxError;
    float minGroupRadius;
};

layout(buffer_reference, std430) readonly buffer ClusterLevelBuffer {
    ClusterLevelGpu levels[];
};

layout(buffer_reference, std430) readonly buffer ClusterVertexBuffer {
    uint vertices[];
};

layout(buffer_reference, std430) readonly buffer ClusterTriangleBuffer {
    uint triangles[];
};

// Culled clusters, kDrawBuckets lists per culling phase of
// kMaxDrawnClusters entries each.
const uint kMaxDrawnClusters = 1u << 21;

// Entries are (instance id | terrain sun code << 24, cluster index); the code
// is 0 (shadowed) or 1 (lit) where cluster_cull.comp traced the terrain.
layout(buffer_reference, std430) buffer DrawListBuffer {
    uvec2 entries[];
};
const uint kTerrainSunPerFragment = 255u;

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
    // Its TLAS records' instance mask (gpu_types.h).
    uint tlasMask;
    // Base colour multiplier, RGB8 over 0..2 (unpackPrototypeTint).
    uint tint;
    vec3 boundsCenter;
    float boundsRadius;
    uvec2 blasAddress;
    float transitionSize;
    float maxDrawDistance;
};

vec3 unpackPrototypeTint(uint tint) {
    return unpackUnorm4x8(tint).rgb * 2.0;
}

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

// Sky irradiance on an upward-facing surface (sun illuminance 1), integrated
// each frame from the sky-view LUT with the sky capture's clouds over it,
// and from the sky-view LUT alone (the clear sky that lights the clouds).
layout(buffer_reference, std430) buffer SkyIrradianceBuffer {
    vec3 irradiance;
    vec3 clearIrradiance;
};

// Must match GpuTerrainLayer in C++.
struct TerrainLayer {
    uint baseColor;
    uint normal;
    uint arm;
    uint height;
    float tileSize;
    float heightScale;
    // The camera position wrapped by the layer's period (renderer_frame.cpp).
    float offset[3];
    float occlusionStrength;
    float albedoScale[3];
};

layout(buffer_reference, std430) readonly buffer HeightPyramidBuffer {
    uint minMax[];
};

// Selected terrain nodes, (level, x, z) packed by terrain.glsl.
layout(buffer_reference, std430) buffer TerrainNodeBuffer {
    uvec2 nodes[];
};

// Must match GpuTerrain in C++.
struct Terrain {
    // Grouped by role (TerrainRole), rock last.
    TerrainLayer layers[16];
    // Per role: its first layer | variant count << 8.
    uint roles[9];
    uint layerCount;
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
    // The trees' ramp over the forest mask noise (placement.glsl; period
    // 0 = no forest floor).
    uint forestMaskPeriod;
    int forestMaskLo;
    int forestMaskHi;
    uint forestMaskSeed;
    uint padding[2];
};

// Must match GpuWind in C++; functions in wind.glsl. The 10 m wind (direction
// it blows toward, xz); offset is the time integral of its velocity (m).
struct Wind {
    vec2 direction;
    float speed10m;
    float gustiness;
    vec2 offset;
    float gustPhase;
    float shearExponent;
    float veer;
    float veerAltitude;
    float gustPhasePeriod;
    float pad0;
};

// Must match WeatherHistorySample in C++.
struct WeatherHistorySample {
    float travel;
    float coverage;
    float type;
    float precipitation;
};

layout(buffer_reference, std430) readonly buffer WeatherHistoryBuffer {
    WeatherHistorySample samples[];
};

// Must match GpuWeather in C++; lookup in weather.glsl. The current state;
// history oldest first, travel relative to the current travel (<= 0).
struct Weather {
    float coverage;
    float type;
    float precipitation;
    float frontLineUpwind;
    uint historyCount;
    uint pad0;
    WeatherHistoryBuffer history;
};

// Must match GpuClouds in C++; functions in clouds.glsl. Heights above sea
// level (m). The weather map covers mapSize m from mapOrigin (world xz of
// its corner). windOffset is the wind offset at the clouds' altitude modulo
// noisePeriod, which every cloud noise period divides; time is the weather
// noise's time coordinate modulo timePeriod. Periods in m; textures are
// bindless indices. The cloud shadow map (cloudShadow): orthographic along
// shadowForward (the light's travel), shadowSize m across (0 = off),
// the camera sits shadowCamera m from its center in (shadowRight, shadowUp).
struct Clouds {
    float cumulusBottom;
    float cumulusTop;
    float stratusBottom;
    float stratusTop;
    vec2 mapOrigin;
    float mapSize;
    uint mapResolution;
    vec2 windOffset;
    float time;
    float timePeriod;
    float noisePeriod;
    float weatherCell;
    float baseNoisePeriod;
    float detailNoisePeriod;
    float curlNoisePeriod;
    uint weatherMap;
    uint baseNoise;
    uint detailNoise;
    uint curlNoise;
    uint skyCapture;
    vec2 windDisplacement;
    vec3 shadowRight;
    float shadowSize;
    vec3 shadowUp;
    uint shadowResolution;
    vec3 shadowForward;
    uint shadowMap;
    vec2 shadowCamera;
    float groundCoverage;
    float pad1;
};

#include "terrain_detail.glsl"
#include "placement.glsl"

// Streamed placement (renderer_placement.cpp): per streamed layer its
// (full density distance, draw distance), per ring instance its dither
// threshold | streamed layer << 16, and per streamed cell record its cell
// coordinates.
layout(buffer_reference, std430) readonly buffer StreamLayerBuffer {
    vec2 layers[];
};

layout(buffer_reference, std430) readonly buffer StreamThresholdBuffer {
    uint thresholds[];
};

layout(buffer_reference, std430) readonly buffer StreamCoordBuffer {
    ivec2 coords[];
};

const uint kShadowCascades = 5u;

// Must match GpuSunShadowCascade / GpuSunShadows in gpu_types.h. origin is
// the cascade's center relative to the camera; u, v span the map, d points
// away from the sun.
struct SunShadowCascade {
    vec3 origin;
    float halfSize;
    float texel;
    float depthNear;
    float depthFar;
    uint texture;
    float radius;
    uint valid;
};

struct SunShadows {
    SunShadowCascade cascades[kShadowCascades];
    vec3 u;
    vec3 v;
    vec3 d;
};

// Declared here, defined in farfield.glsl by the shaders that read it.
layout(buffer_reference) buffer FarFieldBuffer;
// Defined in gi.glsl.
layout(buffer_reference) buffer GiBuffer;

// The far shadow map: two 16-bit depths per word (farfield.h).
layout(buffer_reference, std430) readonly buffer FarShadowTexelBuffer {
    uint words[];
};

layout(buffer_reference, std430) buffer FarCellListBuffer {
    uint entries[];
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
    // Added to material texture LODs (textureMipBias: sharper when the
    // render size is below the display size).
    float mipBias;
    vec3 sunIlluminance;
    uint frameIndex;
    vec3 cameraPosition;
    float sunAngularRadius;
    vec3 previousCameraPosition;
    // 1: the sun sample uses a per-pixel, per-frame hash (Ray Reconstruction
    // needs uncorrelated noise); 0: interleaved gradient noise.
    uint whiteNoise;
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
    // Static placed instances (ids kPlacedBase + [0, placedCount)); the
    // streamed ring follows them.
    uint placedCount;
    // Cells: the largest layer draw distance (0: some layer has none) and
    // how far instances' bounds reach past their cell horizontally.
    float cellDrawDistance;
    float cellMargin;
    // Whether farField is set: cells past the far switch draw their voxels.
    uint farFieldEnabled;
    // The ring's instances and the streamed cell records (which follow the
    // grid's cells in `cells`). Visibility: ids (static + ring), then the
    // grid cells and the streamed records.
    uint ringCount;
    uint streamRecordCount;
    uint pad4;
    uint pad5;
    StreamLayerBuffer streamLayers;
    StreamThresholdBuffer streamThresholds;
    StreamCoordBuffer streamCoords;
    ClusterNodeBuffer clusterNodes;
    ClusterLevelBuffer clusterLevels;
    // Effects (renderer.h): parallax depth multiplier (0 = off), parallax
    // self-shadow on/off, micro shadow strength, contact shadow length multiplier.
    float parallaxDepthScale;
    float parallaxSelfShadow;
    float microShadowStrength;
    float contactLengthScale;
    // The global wind, this frame's and last frame's (for motion vectors).
    Wind wind;
    Wind previousWind;
    Weather weather;
    Clouds clouds;
    // The far field (farfield.glsl) and the far cells the cell cull appends
    // for voxel.mesh: cell | level << 30, one entry per cell.
    FarFieldBuffer farField;
    FarCellListBuffer farCellList;
    // Whether receivers beyond kFarShadowDistance (lighting.glsl) trace the
    // far field instead of the TLAS's far-field trees.
    uint voxelFarShadows;
    // GI (gi.glsl): 0 off, 1 probes.
    uint giMode;
    GiBuffer gi;
    // The far shadow map receivers read (must match SceneData): texels, the
    // frame (axes, minima, texel and depth in m, size), whether it serves.
    FarShadowTexelBuffer farShadowTexels;
    float farShadowU[3];
    float farShadowV[3];
    float farShadowD[3];
    float farShadowMinU, farShadowMinV, farShadowMinD;
    float farShadowTexel, farShadowDepth;
    uint farShadowWidth, farShadowHeight;
    uint farShadowMode;
    uint pad7;
    // Raster sun shadows (SceneData in gpu_types.h).
    uint shadowView;
    float orthoPixelsPerMeter;
    uint shadowMode;
    float shadowDrawMargin;
    SunShadows sunShadows;
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
    // Streamed instances: the shrink factor (streamFade), 1 otherwise.
    float fade;
};

// A streamed instance (a ring id) thins out with distance and shrinks to
// nothing before it is dropped (streamFade), and sits on the displayed
// terrain: the raster terrain adds detail noise near the camera, which
// short plants would sink under. Every user of loadInstance sees the same
// scale and position, so culling, drawing and motion vectors agree.
void applyStreaming(SceneBuffer scene, uint ring, inout LoadedInstance loaded) {
    uint tag = scene.streamThresholds.thresholds[ring];
    vec2 layer = scene.streamLayers.layers[tag >> 16u];
    vec3 position = loaded.transform[3].xyz;
    float distanceToCamera = length(position - scene.cameraPosition);
    float keep = streamKeepFraction(distanceToCamera, layer.x, layer.y);
    float factor = streamFade(keep, float(tag & 0xFFFFu) / 65536.0);
    loaded.transform[0].xyz *= factor;
    loaded.transform[1].xyz *= factor;
    loaded.transform[2].xyz *= factor;
    loaded.scale *= factor;
    loaded.fade = factor;
    float weight = factor > 0.0 ? terrainDetailWeight(scene.terrain, distanceToCamera) : 0.0;
    if (weight > 0.0) {
        loaded.transform[3].y += weight * terrainDetail(position.xz).x;
    }
}

LoadedInstance loadInstance(SceneBuffer scene, uint id) {
    LoadedInstance loaded;
    if (id >= kPlacedBase) {
        uint index = id - kPlacedBase;
        PlacedInstance placed = scene.placedInstances.placed[index];
        loaded.transform = placedTransform(placed);
        loaded.scale = placedScale(placed);
        loaded.meshOrPrototype = placed.prototype;
        loaded.placed = true;
        loaded.fade = 1.0;
        if (index >= scene.placedCount) {
            applyStreaming(scene, index - scene.placedCount, loaded);
        }
        return loaded;
    }
    InstanceData instance = scene.instances.instances[id];
    loaded.transform = instance.transform;
    loaded.scale = maxScale(instance.transform);
    loaded.meshOrPrototype = instance.meshIndex;
    loaded.placed = false;
    loaded.fade = 1.0;
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
    if (scene.shadowView != 0u) {
        return 0.0;
    }
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

// Per-pixel, per-frame noise: interleaved gradient (low discrepancy), or
// pcg3d white noise, which Ray Reconstruction wants (scene.whiteNoise).
float interleavedGradientNoise(vec2 pixel, uint frame) {
    pixel += 5.588238 * float(frame % 64u);
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

uvec3 pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
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

// Sun shadow rays (lighting.glsl, cluster_cull.comp's per-cluster terrain
// march).
const float kShadowRayBias = 0.05;
const float kShadowRayLength = 20000.0;

// The direction through a point of the sun's disk, from two uniform numbers
// in [0, 1) (radius, angle).
vec3 sunDiskDirection(SceneBuffer scene, float noiseRadius, float noiseAngle) {
    vec3 sun = scene.sunDirection;
    vec3 tangent = normalize(cross(abs(sun.y) < 0.99 ? vec3(0, 1, 0)
                                                      : vec3(1, 0, 0), sun));
    vec3 bitangent = cross(sun, tangent);
    float radius = tan(scene.sunAngularRadius) * sqrt(noiseRadius);
    float angle = 6.28318531 * noiseAngle;
    return normalize(sun + radius * (cos(angle) * tangent +
                                     sin(angle) * bitangent));
}

// Picks a direction inside the sun's disk. One ray per pixel per frame;
// accumulated over frames this gives penumbrae that widen with distance from
// the occluder, like real sun shadows.
vec3 sampleSunDirection(SceneBuffer scene, vec2 pixel) {
    if (scene.whiteNoise != 0u) {
        uvec3 h = pcg3d(uvec3(uvec2(pixel), scene.frameIndex));
        return sunDiskDirection(scene, float(h.x >> 8u) * (1.0 / 16777216.0),
                                float(h.y >> 8u) * (1.0 / 16777216.0));
    }
    return sunDiskDirection(scene, interleavedGradientNoise(pixel, scene.frameIndex),
                            interleavedGradientNoise(pixel + vec2(17.0, 31.0),
                                                     scene.frameIndex));
}
