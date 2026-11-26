#ifndef DEBUG_ID_GLSL
#define DEBUG_ID_GLSL

// Scene attachment 7 (R32G32B32A32_UINT): what each pixel shows, for the
// debug views and the report's pixel pick (Renderer::pick). Only the
// DEBUG_ID shader variants write it, in the scene pipelines made while a
// debug view or the report is on (createScenePipelines); otherwise the
// writes compile away and the pass has seven attachments, as without it.
// x: kind << 24 | instance id, y: cluster index (kDebugTriangles,
// kDebugVoxels) or far list entry (kDebugFarCell), z: distance from the
// camera (float bits), w: material index (~0 where none).
// Must match PickKind in debug_view.h.
const uint kDebugSky = 0u;
const uint kDebugTriangles = 1u;
const uint kDebugVoxels = 2u;
const uint kDebugFarCell = 3u;
const uint kDebugTerrain = 4u;

#ifndef DEBUG_ID_CONSTANTS_ONLY
#ifdef DEBUG_ID
layout(location = 7) out uvec4 outDebugId;
#if defined(DEBUG_ID_MESH) && !defined(TERRAIN)
// (instance id, cluster index) from mesh.mesh.
layout(location = 13) flat in uvec2 inDebugId;
#endif
#endif

void writeDebugId(uint kind, uint instance, uint detail, vec3 relative, uint material) {
#ifdef DEBUG_ID
    outDebugId = uvec4(kind << 24u | (instance & 0xFFFFFFu), detail,
                       floatBitsToUint(length(relative)), material);
#endif
}

#ifdef DEBUG_ID_MESH
// mesh.frag: clusters (mesh.mesh) and the terrain.
void writeMeshDebugId(vec3 relative, uint material) {
#if defined(DEBUG_ID) && !defined(TERRAIN)
    writeDebugId(kDebugTriangles, inDebugId.x, inDebugId.y, relative, material);
#else
    writeDebugId(kDebugTerrain, 0u, 0u, relative, ~0u);
#endif
}
#endif
#endif

#endif
