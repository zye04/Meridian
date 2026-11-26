// The closest surface along a ray: the placed and hand-placed objects in
// the TLAS (alpha-tested like the shadow rays) or the heightfield, with the
// attributes surface.glsl needs. For the reference path tracer.
#ifndef RAY_HIT_GLSL
#define RAY_HIT_GLSL

struct RayHit {
    bool hit;
    bool terrain;
    float t;
    vec3 position;
    // Both face the ray, as mesh.frag flips them for back faces.
    vec3 geometricNormal;
    vec3 normal;
    vec2 uv;
    vec4 tangent;
    uint materialIndex;
    vec3 tint;
    float height;
};

RayHit traceClosest(vec3 origin, vec3 direction, float tMax) {
    RayHit hit;
    hit.hit = false;
    hit.terrain = false;
    hit.t = tMax;
    rayQueryEXT query;
    rayQueryInitializeEXT(query, accelerationStructureEXT(pc.scene.tlas), gl_RayFlagsNoneEXT,
                          kShadowCasterMask | kFarFieldCasterMask, origin, 0.0, direction, tMax);
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
                gl_RayQueryCandidateIntersectionTriangleEXT &&
            alphaTestCandidate(query)) {
            rayQueryConfirmIntersectionEXT(query);
        }
    }
    bool object = rayQueryGetIntersectionTypeEXT(query, true) !=
                  gl_RayQueryCommittedIntersectionNoneEXT;
    float objectT = object ? rayQueryGetIntersectionTEXT(query, true) : tMax;
    float terrainT = terrainRayDistance(pc.scene, origin, direction, objectT);
    if (terrainT >= 0.0 && terrainT < objectT) {
        hit.hit = true;
        hit.terrain = true;
        hit.t = terrainT;
        hit.position = origin + direction * terrainT;
        hit.geometricNormal = vec3(0.0, 1.0, 0.0);
        hit.normal = hit.geometricNormal;
        hit.uv = vec2(0.0);
        hit.tangent = vec4(0.0);
        hit.materialIndex = 0u;
        hit.tint = vec3(1.0);
        hit.height = 0.0;
        return hit;
    }
    if (!object) {
        return hit;
    }
    uint id = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
    MeshInfo mesh = pc.scene.meshes.meshes[instanceMesh(
        pc.scene, id, rayQueryGetIntersectionGeometryIndexEXT(query, true))];
    uint first = mesh.firstIndex + rayQueryGetIntersectionPrimitiveIndexEXT(query, true) * 3u;
    Vertex v0 = loadVertex(pc.scene, mesh, pc.scene.indices.indices[first] + mesh.vertexOffset);
    Vertex v1 = loadVertex(pc.scene, mesh, pc.scene.indices.indices[first + 1u] + mesh.vertexOffset);
    Vertex v2 = loadVertex(pc.scene, mesh, pc.scene.indices.indices[first + 2u] + mesh.vertexOffset);
    vec2 b = rayQueryGetIntersectionBarycentricsEXT(query, true);
    vec3 w = vec3(1.0 - b.x - b.y, b);
    mat4x3 toWorld = rayQueryGetIntersectionObjectToWorldEXT(query, true);
    mat3 linear = mat3(toWorld);
    mat3 normalMatrix = transpose(inverse(linear));

    hit.hit = true;
    hit.t = objectT;
    hit.position = origin + direction * objectT;
    vec3 objectPosition = w.x * v0.position + w.y * v1.position + w.z * v2.position;
    // The winding's normal (glTF: counter-clockwise front, as the raster
    // pipeline). Seen from its back, mesh.frag flips the interpolated normal
    // (gl_FrontFacing); both then face the ray.
    hit.geometricNormal =
        normalize(normalMatrix * cross(v1.position - v0.position, v2.position - v0.position));
    hit.normal = normalize(normalMatrix * (w.x * v0.normal + w.y * v1.normal + w.z * v2.normal));
    if (dot(hit.geometricNormal, direction) > 0.0) {
        hit.geometricNormal = -hit.geometricNormal;
        hit.normal = -hit.normal;
    }
    hit.uv = w.x * vec2(v0.uvX, v0.uvY) + w.y * vec2(v1.uvX, v1.uvY) + w.z * vec2(v2.uvX, v2.uvY);
    vec4 tangent = w.x * v0.tangent + w.y * v1.tangent + w.z * v2.tangent;
    hit.tangent = vec4(linear * tangent.xyz, v0.tangent.w);
    hit.materialIndex = mesh.materialIndex;
    hit.tint = vec3(1.0);
    if (id >= kPlacedBase) {
        LoadedInstance instance = loadInstance(pc.scene, id);
        hit.tint = unpackPrototypeTint(
            pc.scene.prototypes.prototypes[instance.meshOrPrototype].tint);
    }
    float top = mesh.boundsCenter.y + mesh.boundsRadius;
    hit.height = clamp(objectPosition.y / max(top, 1e-4), 0.0, 1.0);
    return hit;
}

#endif
