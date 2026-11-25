#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#include "common.glsl"
#include "atmosphere.glsl"
#include "material.glsl"
#include "terrain.glsl"
#include "lighting.glsl"
#include "voxel_lod.glsl"

// A voxel of a voxel cluster (voxel_instance.mesh draws it as a
// camera-facing quad), shaded flat at its centre as the leaves it stands in
// for: their mean reflected and transmitted colour (with the placed
// instance's tint, as mesh.frag), roughness and normal lobe. The aligned
// part of the lobe shades as one two-sided leaf through the same lighting
// as the triangles; the rest as leaves facing every way (the spherical-leaf
// phase, gi.glsl). The model shades itself through the voxel's optical
// depth out of it toward the sun (sunlit tops, dark undersides) and the sky;
// the sun ray then traces the scene from where it leaves the mesh's box;
// the terrain's sun visibility comes per brick from voxel_bricks.comp.

layout(location = 0) flat in vec3 inRelative;
layout(location = 1) flat in uvec4 inVoxel;
layout(location = 2) flat in uvec4 inInfo;
layout(location = 3) flat in vec3 inLocal;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;
// The guides and the sun's exposed radiance, as in mesh.frag; the
// transparency mask (location 2) is not written (opaque).
layout(location = 3) out vec4 outDiffuseAlbedo;
layout(location = 4) out vec4 outSpecularAlbedo;
layout(location = 5) out vec4 outNormalRoughness;
layout(location = 6) out vec4 outSunLight;

// Where the sun ray leaves the box, so it doesn't start inside the crown.
const float kExitBias = 0.1;

void main() {
    SceneBuffer scene = pc.scene;
    LoadedInstance instance = loadInstance(scene, inInfo.x);
    ClusterInfo cluster = scene.clusters.clusters[inInfo.w];
    MeshInfo mesh = scene.meshes.meshes[cluster.mesh];

    mat3 linear = mat3(instance.transform);
    float scale = instance.scale;
    mat3 toLocal = transpose(linear) / (scale * scale);
    vec3 relative = inRelative;
    vec3 world = scene.cameraPosition + relative;
    uvec4 voxel = inVoxel;
    uint axisWord = inInfo.z;

    vec3 tint = instance.placed
        ? unpackPrototypeTint(scene.prototypes.prototypes[instance.meshOrPrototype].tint)
        : vec3(1.0);
    vec3 albedo = voxelSqrtRgb(voxel.y) * tint;
    vec3 transmitted = voxelSqrtRgb(voxel.z) * tint;
    // The aligned share of the lobe: (3 alignment - 1) / 2 of the leaves
    // face along its axis, the rest every way.
    float aligned = clamp((3.0 * voxelAlignment(voxel) - 1.0) * 0.5, 0.0, 1.0);
    // Sky visibility out of the model (up, and the four sides), as the
    // clusters' crownOcclusion (surface.glsl): only without GI, whose probes
    // already trace the canopy.
    float occlusion = 1.0;
    if (scene.giMode == 0u) {
        float up = exp(-voxelBoxDepth(axisWord, normalize(toLocal * vec3(0.0, 1.0, 0.0))));
        vec3 east = normalize(toLocal * vec3(1.0, 0.0, 0.0));
        vec3 south = normalize(toLocal * vec3(0.0, 0.0, 1.0));
        float sides = 0.25 * (exp(-voxelBoxDepth(axisWord, east)) +
                              exp(-voxelBoxDepth(axisWord, -east)) +
                              exp(-voxelBoxDepth(axisWord, south)) +
                              exp(-voxelBoxDepth(axisWord, -south)));
        occlusion = mix(1.0, 0.5 * (up + sides), scene.microShadowStrength);
    }

    Surface s;
    s.baseColor = albedo;
    s.alpha = 1.0;
    s.metallic = 0.0;
    // The leaves' normals spread around the lobe axis: their highlights
    // spread as much (Toksvig), or the flat axis glints where no leaf would.
    float alpha = voxelRoughness(voxel) * voxelRoughness(voxel);
    float spread = 2.0 * (1.0 - voxelAlignment(voxel));
    s.perceptualRoughness = sqrt(sqrt(min(alpha * alpha + spread, 1.0)));
    s.occlusion = occlusion;
    s.microOcclusion = 1.0;
    s.sunShadow = 1.0;
    s.emissive = vec3(0.0);
    // Two-sided, like the leaves the voxel averages.
    s.normal = normalize(linear * voxelLobeAxis(voxel));
    if (dot(s.normal, relative) > 0.0) {
        s.normal = -s.normal;
    }

    Shading sh = beginShading(s, relative);
    vec3 sunRadiance = vec3(0.0);
    float backNoL = clamp(-dot(s.normal, sh.L), 0.0, 1.0);
    vec3 rayDirection = sampleSunDirection(scene, gl_FragCoord.xy);
    if (rayDirection.y > -sin(scene.sunAngularRadius)) {
        vec3 sunLocal = normalize(toLocal * rayDirection);
        // Cascades 0-3 hold the voxels as casters (the coarsest one, far
        // cells): there the voxel takes the sun as a leaf does, from just
        // past its own caster. Beyond (and in the blend into the far cells,
        // which hold its model too) it shades itself through its depth out
        // of the model and traces from where the sun ray leaves the box.
        // Ray-traced shadows take the first path at every distance: the
        // ray meets the model's own triangles as a leaf's would.
        float casterRadius = scene.sunShadows.cascades[kShadowCascades - 2u].radius *
                             (1.0 - kCascadeBlendFraction);
        float visibility;
        if (scene.shadowMode != 0u || length(relative) < casterRadius) {
            vec3 origin = world + rayDirection * (cluster.voxelEdge * scale);
            visibility = traceSunVisibility(origin, origin, rayDirection, 0.0, false,
                                            origin - scene.cameraPosition, rayDirection);
        } else {
            vec3 halfExtent = vec3(mesh.halfExtentX, mesh.halfExtentY, mesh.halfExtentZ);
            vec3 lo = mesh.boundsCenter - halfExtent, hi = mesh.boundsCenter + halfExtent;
            vec3 exitPoint =
                world + rayDirection * (voxelBoxExit(lo, hi, inLocal, sunLocal) * scale + kExitBias);
            visibility = exp(-voxelBoxDepth(axisWord, sunLocal)) *
                         traceSunVisibility(exitPoint, exitPoint, rayDirection, 0.0, false,
                                            exitPoint - scene.cameraPosition, s.normal);
        }
        visibility *= float(inInfo.y);
        if (sh.NoL > 0.0) {
            addSunLight(sh, visibility * aligned, 1.0);
        }
        addTransmittedSun(sh, transmitted, backNoL, visibility * aligned);
        // Light travels -L, scattered toward the camera travels -V.
        sh.diffuseLight += sh.sunAtSurface * (visibility * (1.0 - aligned)) *
                           leafPhase(-dot(sh.L, sh.V), albedo, transmitted);
        sunRadiance = sh.diffuseLight + sh.specularLight;
    }
    addAmbientLight(sh, s.occlusion);
    addTransmittedAmbient(sh, transmitted, s.occlusion);
    float hazeTransmittance;
    vec3 luminance = applyAerialPerspective(sh.diffuseLight + sh.specularLight, relative, 1.0,
                                            hazeTransmittance);
    vec3 exposure = scene.sunIlluminance * scene.exposure.exposure;
    outColor = vec4(min(luminance * exposure, vec3(kMaxExposedLuminance)), 1.0);
    outDiffuseAlbedo = vec4(sh.diffuseColor, 1.0);
    outSpecularAlbedo = vec4(mix(sh.dfg.xxx, sh.dfg.yyy, sh.f0), 1.0);
    outNormalRoughness = vec4(sh.N, sh.perceptualRoughness);
    outSunLight =
        vec4(min(sunRadiance * hazeTransmittance * exposure, vec3(kMaxExposedLuminance)), 1.0);

    // Motion of the flat point, each frame displayed with its own camera and
    // drop (static, so a still camera gives zero).
    vec4 current = scene.currentViewProjection * displayedPosition(scene, relative);
    vec4 previous = scene.previousViewProjection *
                    displayedPosition(scene, relative + (scene.cameraPosition -
                                                         scene.previousCameraPosition));
    outMotion = previous.xy / previous.w * 0.5 - current.xy / current.w * 0.5;
}
