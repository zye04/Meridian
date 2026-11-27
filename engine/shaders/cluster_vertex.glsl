// A cluster's vertices as mesh.mesh emits them, shared with the visibility
// resolve, which rebuilds a pixel's triangle from the same three vertices:
// both must compute the same clip positions.
#ifndef CLUSTER_VERTEX_GLSL
#define CLUSTER_VERTEX_GLSL

// Tree crowns get no sky inside: leaves deep in the crown see the sky through
// the leaves around them. The crown is the ellipsoid inscribed in the mesh's
// bounding box, a uniform leaf medium; the visibility is its transmittance
// toward the zenith, up-and-outward and low outward (normalized units),
// averaged: a trunk under a lone crown still sees the sky below it.
// Meshes shorter than this are plant tufts (plantOcclusion in mesh.frag).
const float kCrownMinHalfHeight = 2.0;
const float kCrownDensity = 2.5;

bool isLeafMesh(SceneBuffer scene, MeshInfo mesh) {
    return scene.materials.materials[mesh.materialIndex].diffuseTransmission > 0.0;
}

// The path length inside the unit sphere along q + t w, t >= 0.
float crownExit(vec3 q, vec3 w) {
    float b = dot(q, w);
    float d = b * b - dot(q, q) + 1.0;
    if (d <= 0.0) {
        return 0.0;
    }
    float root = sqrt(d);
    return max(-b + root - max(-b - root, 0.0), 0.0);
}

float crownVisibility(MeshInfo mesh, vec3 position, vec3 normal) {
    vec3 halfExtent = vec3(mesh.halfExtentX, mesh.halfExtentY, mesh.halfExtentZ);
    if (halfExtent.y < kCrownMinHalfHeight) {
        return 1.0;
    }
    vec3 q = (position - mesh.boundsCenter) / max(halfExtent, vec3(1e-3));
    // Away from the crown's axis; on it (a trunk) the way the surface faces.
    vec2 outward = q.xz + 0.5 * normal.xz;
    outward = dot(outward, outward) > 1e-6 ? normalize(outward) : vec2(0.0);
    float up = crownExit(q, vec3(0.0, 1.0, 0.0));
    float steep = crownExit(q, normalize(vec3(outward.x, 1.0, outward.y)));
    float low = crownExit(q, normalize(vec3(outward.x, 0.3, outward.y)));
    return (exp(-kCrownDensity * up) + exp(-kCrownDensity * steep) +
            exp(-kCrownDensity * low)) / 3.0;
}

struct ClusterContext {
    LoadedInstance instance;
    ClusterInfo cluster;
    MeshInfo mesh;
    // The crown: a leaf mesh's own bounds; the rest of a placed tree (bark)
    // takes its leaf mesh's, so trunks and branches under it darken too.
    bool hasCrown;
    MeshInfo crown;
    // Only tufts darken toward their base (plantOcclusion); a crown's leaves
    // are occluded by the crown itself (crown visibility or the GI probes).
    bool tuft;
    // Normals use the inverse transpose so non-uniform scale stays correct.
    mat3 normalMatrix;
    float lodError;
    vec3 tint;
};

ClusterContext loadClusterContext(SceneBuffer scene, uint instanceId, uint clusterIndex) {
    ClusterContext c;
    c.instance = loadInstance(scene, instanceId);
    c.cluster = scene.clusters.clusters[clusterIndex];
    c.mesh = scene.meshes.meshes[c.cluster.mesh];
    c.hasCrown = isLeafMesh(scene, c.mesh);
    c.crown = c.mesh;
    if (!c.hasCrown && c.instance.placed) {
        PrototypeInfo prototype = scene.prototypes.prototypes[c.instance.meshOrPrototype];
        for (uint m = 0u; m < prototype.meshCount && !c.hasCrown; ++m) {
            c.crown = scene.meshes.meshes[scene.prototypeMeshes.meshes[prototype.firstMesh + m]];
            c.hasCrown = isLeafMesh(scene, c.crown);
        }
    }
    c.tuft = c.mesh.halfExtentY < kCrownMinHalfHeight;
    c.normalMatrix = transpose(inverse(mat3(c.instance.transform)));
    c.lodError = max(c.cluster.refinedError, 0.0) * c.instance.scale;
    c.tint = c.instance.placed
                 ? unpackPrototypeTint(scene.prototypes.prototypes[c.instance.meshOrPrototype].tint)
                 : vec3(1.0);
    return c;
}

struct ClusterVertex {
    vec3 relative;
    // Jittered clip position (gl_Position), and the unjittered current and
    // previous ones for motion.
    vec4 clip;
    vec4 currentClip;
    vec4 previousClip;
    vec3 normal;
    vec4 tangent;
    vec2 uv;
    float alpha;
    float height;
    float crownVisibility;
};

// Vertex i (0 to cluster.vertexCount - 1) of the cluster.
ClusterVertex clusterVertex(SceneBuffer scene, ClusterContext c, uint i) {
    uint index = scene.clusterVertices.vertices[c.cluster.firstVertex + i] +
                 uint(c.mesh.vertexOffset);
    Vertex vertex = loadVertex(scene, c.mesh, index);
    // precise: the visibility resolve recomputes these in a fragment shader
    // and must land on the rasterizer's positions (no contraction to fma on
    // one side only).
    precise vec3 world = (c.instance.transform * vec4(vertex.position, 1.0)).xyz;
    precise vec3 relative = world - scene.cameraPosition;
    precise vec4 displayed = displayedPosition(scene, relative);
    precise vec4 clip = scene.viewProjection * displayed;
    ClusterVertex v;
    v.relative = relative;
    v.clip = clip;
    v.normal = c.normalMatrix * vertex.normal;
    v.tangent = vec4(mat3(c.instance.transform) * vertex.tangent.xyz, vertex.tangent.w);
    v.uv = vec2(vertex.uvX, vertex.uvY);
    v.alpha = vertex.alpha;
    float top = c.mesh.boundsCenter.y + c.mesh.boundsRadius;
    v.height = c.tuft ? clamp(vertex.position.y / max(top, 1e-4), 0.0, 1.0) : 1.0;
    v.crownVisibility =
        c.hasCrown ? crownVisibility(c.crown, vertex.position, vertex.normal) : 1.0;
    // Static geometry for now: only the camera moves between frames.
    v.currentClip = scene.currentViewProjection * displayed;
    v.previousClip = scene.previousViewProjection *
                     displayedPosition(scene, world - scene.previousCameraPosition);
    return v;
}

#endif
