// Shared by the cell, instance and cluster culling passes (cell_cull.comp,
// cull.comp, cluster_cull.comp). Include after common.glsl with
// MERIDIAN_CUSTOM_PUSH_CONSTANTS defined.

// Work items: up to 64 clusters of one instance, culled by one cluster_cull
// workgroup. x: instance; y: first cluster | (count - 1) << 26.
const uint kMaxWorkItems = 1u << 19;
const uint kClustersPerWorkItem = 64u;

layout(buffer_reference, std430) buffer WorkItemBuffer {
    uvec2 items[];
};

// Chunks: up to 64 consecutive instance ids, culled by one cull.comp
// workgroup. x: first id; y: count, plus kChunkCellWasVisible when phase 0
// processed the cell's instances (their visibility bits are current).
const uint kMaxChunks = 1u << 17;
const uint kChunkCellWasVisible = 1u << 31;

layout(buffer_reference, std430) buffer ChunkBuffer {
    uvec2 chunks[];
};

// Must match DrawCounters in C++: per phase, the cluster cull's dispatch
// arguments (3 uints), then per phase and draw list the mesh task
// arguments (3 uints), the retest dispatch arguments, then counts.
// gpu_types.h checks these offsets.
const uint kCounterMeshTasks = 6u;
const uint kCounterRetestDispatch = 30u;
const uint kCounterInstances = 33u;
const uint kCounterTriangles = 34u;
const uint kCounterOverflow = 35u;
const uint kCounterRetest = 36u;
const uint kCounterCells = 38u;
const uint kCounterChunkDispatch = 39u;
// Per list: chunks, work items, cluster draw list entries (terrain nodes:
// terrain_select.comp).
const uint kCounterDrops = 45u;

// Clusters phase 0 found occluded by last frame's pyramid: (instance,
// cluster), retested in phase 1 against this frame's.
const uint kMaxRetestClusters = 1u << 20;
const uint kCullRetest = 2u;

layout(buffer_reference, std430) buffer RetestBuffer {
    uvec2 entries[];
};

layout(buffer_reference, std430) buffer CounterBuffer {
    uint counters[];
};

layout(buffer_reference, std430) buffer VisibilityBuffer {
    uint visible[];
};

// Min-reduced: a linear fetch returns the farthest depth under its 2x2
// footprint.
layout(set = 0, binding = 0) uniform sampler2D depthPyramid;

// Must match CullConstants in C++.
layout(push_constant) uniform CullConstants {
    SceneBuffer scene;
    WorkItemBuffer work;
    CounterBuffer counters;
    VisibilityBuffer visibility;
    // 0, 1, or kCullRetest: phase 1 over the clusters phase 0 found occluded.
    uint phase;
    uint occlusion;
    uint pyramidLevels;
    // Whether the pyramid holds last frame's depth.
    uint previousPyramid;
    vec2 pyramidSize;
    RetestBuffer retest;
    ChunkBuffer chunks;
} cull;

// Counts entries dropped from a full list.
void countDrops(uint list, uint count) {
    atomicAdd(cull.counters.counters[kCounterOverflow], count);
    atomicAdd(cull.counters.counters[kCounterDrops + list], count);
}

// Reserves `count` consecutive entries of a list whose length is also an
// indirect dispatch's group count (counters[counter]). What doesn't fit is
// given back, so the length ends at the capacity, and counted as dropped;
// `count` becomes what fits. Returns the first entry.
uint reserveEntries(uint counter, uint capacity, uint list, inout uint count) {
    uint first = atomicAdd(cull.counters.counters[counter], count);
    uint end = min(first + count, capacity);
    uint kept = end > first ? end - first : 0u;
    if (kept < count) {
        atomicAdd(cull.counters.counters[counter], kept - count);
        countDrops(list, count - kept);
        count = kept;
    }
    return first;
}

// A sphere (relative to the camera it is seen from) as displayed under the
// curvature warp: its points drop between the drop at its nearest and
// farthest horizontal distance, so lower it by the first and grow it by the
// difference.
vec4 displayedSphere(vec3 center, float radius) {
    float d = length(center.xz);
    float nearDrop = curvatureDrop(cull.scene, vec3(max(d - radius, 0.0), 0.0, 0.0));
    float farDrop = curvatureDrop(cull.scene, vec3(d + radius, 0.0, 0.0));
    return vec4(center.x, center.y - nearDrop, center.z, radius + farDrop - nearDrop);
}

bool inFrustum(vec3 center, float radius) {
    bool inside = true;
    for (int i = 0; i < 6; ++i) {
        vec4 plane = cull.scene.frustumPlanes[i];
        inside = inside && dot(plane.xyz, center) + plane.w >= -radius;
    }
    return inside;
}

// Screen-space bounds (UV) of a view-space sphere, camera looking down +z
// here (Mara & McGuire 2013, via zeux's niagara). False if it crosses the
// near plane, where the bounds are unbounded.
bool projectSphere(vec3 c, float r, out vec4 bounds) {
    SceneBuffer scene = cull.scene;
    if (c.z < r + scene.nearPlane) {
        return false;
    }
    vec3 cr = c * r;
    float czr2 = c.z * c.z - r * r;
    float vx = sqrt(c.x * c.x + czr2);
    float minX = (vx * c.x - cr.z) / (vx * c.z + cr.x);
    float maxX = (vx * c.x + cr.z) / (vx * c.z - cr.x);
    float vy = sqrt(c.y * c.y + czr2);
    float minY = (vy * c.y - cr.z) / (vy * c.z + cr.y);
    float maxY = (vy * c.y + cr.z) / (vy * c.z - cr.y);
    // To NDC (the y scale is negative: Vulkan's y points down), then UV.
    vec4 ndc = vec4(minX, minY, maxX, maxY) * scene.projectionScale.xyxy;
    vec4 uv = ndc * 0.5 + 0.5;
    bounds = vec4(min(uv.xy, uv.zw), max(uv.xy, uv.zw));
    return true;
}

// Against the depth pyramid, as seen from `view` (the view its depth was
// rendered with).
// The sphere's center is relative to that view's camera.
bool occluded(vec4 sphere, mat4 view) {
    float radius = sphere.w;
    vec3 c = (view * vec4(sphere.xyz, 1.0)).xyz;
    c.z = -c.z;
    vec4 bounds;
    if (!projectSphere(c, radius, bounds)) {
        return false;
    }
    // The pyramid comes from jittered depth (up to half a pixel each way);
    // one level-0 texel of margin keeps the test conservative under it.
    bounds += vec4(-1.0, -1.0, 1.0, 1.0) / cull.pyramidSize.xyxy;
    vec2 size = (bounds.zw - bounds.xy) * cull.pyramidSize;
    // The level where the bounds span at most one texel; the 2x2 fetch at
    // their center then covers every texel they touch.
    float level = clamp(ceil(log2(max(size.x, size.y))), 0.0,
                        float(cull.pyramidLevels - 1u));
    float farthest = textureLod(depthPyramid, (bounds.xy + bounds.zw) * 0.5, level).r;
    // Reverse-Z with an infinite far plane: depth = near / distance.
    float nearest = cull.scene.nearPlane / (c.z - radius);
    return nearest < farthest;
}
