#ifndef CROWN_GLSL
#define CROWN_GLSL

// The crown ellipsoid for leaf voxels (voxel_instance.mesh): the same
// medium as mesh.mesh's crownVisibility, which keeps its own copy so the
// triangle pipelines stay as they were (the crown fill is voxels only,
// decisions.md "Crown fill").
// Tree crowns get no sky inside: leaves deep in the crown see the sky through
// the leaves around them. The crown is the ellipsoid inscribed in the mesh's
// bounding box, a uniform leaf medium; the visibility is its transmittance
// toward the zenith, up-and-outward and low outward (normalized units),
// averaged: a trunk under a lone crown still sees the sky below it.
// Meshes shorter than this are plant tufts (plantOcclusion in mesh.frag).
const float kCrownMinHalfHeight = 2.0;
const float kCrownDensity = 2.5;

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

// The crown's optical depth from a point toward the sun (mesh space, unit
// sun), through the same uniform leaf medium as crownVisibility.
float crownSunDepth(MeshInfo mesh, vec3 position, vec3 sun) {
    vec3 halfExtent = max(vec3(mesh.halfExtentX, mesh.halfExtentY, mesh.halfExtentZ), vec3(1e-3));
    vec3 q = (position - mesh.boundsCenter) / halfExtent;
    return kCrownDensity * crownExit(q, normalize(sun / halfExtent));
}

#endif
