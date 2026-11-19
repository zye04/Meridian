// Shared by the instance and cluster culling passes (cull.comp,
// cluster_cull.comp). Include after common.glsl with
// MERIDIAN_CUSTOM_PUSH_CONSTANTS defined.

// Work items: up to 64 clusters of one instance, culled by one cluster_cull
// workgroup. x: instance; y: first cluster | (count - 1) << 26.
const uint kMaxWorkItems = 1u << 19;
const uint kClustersPerWorkItem = 64u;

layout(buffer_reference, std430) buffer WorkItemBuffer {
    uvec2 items[];
};

// Must match DrawCounters in C++: per phase, the cluster cull's dispatch
// arguments (3 uints), then per phase and draw list the mesh task
// arguments (3 uints), then statistics.
const uint kCounterMeshTasks = 6u;
const uint kCounterInstances = 30u;
const uint kCounterTriangles = 31u;
const uint kCounterOverflow = 32u;

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
    uint phase;
    uint occlusion;
    uint pyramidLevels;
    uint pad0;
    vec2 pyramidSize;
} cull;

// Largest axis scale of a transform: a conservative bound under
// non-uniform scale.
float maxScale(mat4 transform) {
    mat3 m = mat3(transform);
    return sqrt(max(dot(m[0], m[0]), max(dot(m[1], m[1]), dot(m[2], m[2]))));
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

bool occluded(vec3 worldCenter, float radius) {
    vec3 c = (cull.scene.view * vec4(worldCenter, 1.0)).xyz;
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
