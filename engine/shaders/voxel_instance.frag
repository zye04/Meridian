#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#ifdef DEBUG_ID
#extension GL_ARB_fragment_shader_interlock : require
layout(pixel_interlock_unordered) in;
// The debug stores would otherwise move the depth test after shading.
layout(early_fragment_tests) in;
#endif
#include "common.glsl"
#include "atmosphere.glsl"
#include "material.glsl"
#include "terrain.glsl"
#include "lighting.glsl"
#include "voxel_lod.glsl"
#include "debug_id.glsl"

// A voxel of a voxel cluster (voxel_instance.mesh draws it as a
// camera-facing quad), shaded flat at its centre as the leaves it stands in
// for: their mean reflected and transmitted colour (with the placed
// instance's tint, as mesh.frag), roughness and normal lobe. The aligned
// part of the lobe shades as one two-sided leaf through the same lighting
// as the triangles; the rest as leaves facing every way (the spherical-leaf
// phase, gi.glsl). The sun reaches it as it reaches a leaf, from one voxel
// toward the sun, its own model among the casters; the model shades itself
// toward the sky through the voxel's optical depth out of it; the terrain's
// sun visibility comes per brick from voxel_bricks.comp.

layout(location = 0) flat in vec3 inRelative;
layout(location = 1) flat in uvec4 inVoxel;
layout(location = 2) flat in uvec4 inInfo;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;
// The guides and the sun's exposed radiance, as in mesh.frag; the
// transparency mask (location 2) is not written (opaque).
layout(location = 3) out vec4 outDiffuseAlbedo;
layout(location = 4) out vec4 outSpecularAlbedo;
layout(location = 5) out vec4 outNormalRoughness;
layout(location = 6) out vec4 outSunLight;

// |N1 - N2|^2 between neighbouring voxels' lobe axes, for specular AA: a
// crown's leaves face every way, so neighbours' axes are about 60 degrees
// apart (|N1 - N2|^2 = 2 - 2 cos). The value stands for the squared normal
// change over both screen axes together (the sum the function expects).
const float kVoxelNeighbourNormalChange = 1.0;

void main() {
    SceneBuffer scene = pc.scene;
    LoadedInstance instance = loadInstance(scene, inInfo.x);
    ClusterInfo cluster = scene.clusters.clusters[inInfo.w];

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
    // clusters' crownOcclusion (surface.glsl). With GI it applies only to the
    // share outside the probes' clipmap: inside, the probes trace the canopy.
    float up = exp(-voxelBoxDepth(axisWord, normalize(toLocal * vec3(0.0, 1.0, 0.0))));
    vec3 east = normalize(toLocal * vec3(1.0, 0.0, 0.0));
    vec3 south = normalize(toLocal * vec3(0.0, 0.0, 1.0));
    float sides = 0.25 * (exp(-voxelBoxDepth(axisWord, east)) + exp(-voxelBoxDepth(axisWord, -east)) +
                          exp(-voxelBoxDepth(axisWord, south)) + exp(-voxelBoxDepth(axisWord, -south)));
    float occlusion = mix(1.0, 0.5 * (up + sides), scene.microShadowStrength);

    Surface s;
    s.baseColor = albedo;
    s.alpha = 1.0;
    s.metallic = 0.0;
    // The leaves' normals spread around the lobe axis: their highlights
    // spread as much (Toksvig), or the flat axis glints where no leaf would.
    float alpha = voxelRoughness(voxel) * voxelRoughness(voxel);
    float spread = 2.0 * (1.0 - voxelAlignment(voxel));
    s.perceptualRoughness = sqrt(sqrt(min(alpha * alpha + spread, 1.0)));
    // Specular AA: a pixel's normals change by the change between
    // neighbouring voxels' lobe axes over the voxel's size on screen.
    if (scene.foliageSpecularAa != 0u) {
        float voxelPixels = cluster.voxelEdge * scale * abs(scene.projectionScale.y) * 0.5 /
                            (scene.inverseResolution.y * length(relative));
        s.perceptualRoughness =
            specularAaRoughness(s.perceptualRoughness,
                                kVoxelNeighbourNormalChange / max(voxelPixels * voxelPixels, 1.0));
    }
    s.occlusion = scene.giMode == 0u ? occlusion : 1.0;
    s.microOcclusion = 1.0;
    s.sunShadow = 1.0;
    s.emissive = vec3(0.0);
    // Two-sided, like the leaves the voxel averages.
    s.normal = normalize(linear * voxelLobeAxis(voxel));
    if (dot(s.normal, relative) > 0.0) {
        s.normal = -s.normal;
    }

    Shading sh = beginShading(s, relative);
    if (scene.leafLight != 0u && isLeafMesh(scene, scene.meshes.meshes[cluster.mesh])) {
        float leafCrownR = mix(1.0, exp(-voxelBoxDepth(axisWord, normalize(toLocal * reflect(-sh.V, sh.N)))),
                               scene.microShadowStrength);
        setLeafLight(sh, albedo + transmitted, occlusion, leafCrownR);
    }
    // The leaf light's canopy ambient holds the crown itself, GI on or off.
    if (sh.leafVisibility >= 0.0) {
        s.occlusion = 1.0;
    }
    vec3 sunRadiance = vec3(0.0);
    float backNoL = clamp(-dot(s.normal, sh.L), 0.0, 1.0);
    vec3 rayDirection = sampleSunDirection(scene, gl_FragCoord.xy);
    if (rayDirection.y > -sin(scene.sunAngularRadius)) {
        // The cascades hold the voxels as casters up to cascade 3 and the far
        // cells (the model's bricks) in cascade 4, ray-traced shadows the
        // model's triangles: either way the model shades itself.
        vec3 origin = world + rayDirection * (cluster.voxelEdge * scale);
        // The cascades take the voxel at its centre with no offset: one along
        // the sun moves the receiver nearer the light by a voxel edge and the
        // normal offset (~4 m in cascade 3), past its own crown's leaves in
        // front of it, and lit the leaves inside a sunlit crown (decisions.md
        // "Voxel sun lookup"); the depth bias holds the acne. The ray-traced
        // and far-shadow paths keep the offset start.
        bool centre = scene.voxelShadowOffset == 0u && scene.shadowMode == 0u && !farShadowReceiver(origin);
        float visibility = traceSunVisibility(origin, origin, rayDirection, 0.0, false,
                                              (centre ? world : origin) - scene.cameraPosition,
                                              centre ? vec3(0.0) : rayDirection);
        visibility *= float(inInfo.y);
        if (sh.NoL > 0.0) {
            addSunLight(sh, visibility * aligned, 1.0);
        }
        // Light travels -L, scattered toward the camera travels -V.
        vec2 phase = leafPhaseWeights(-dot(sh.L, sh.V));
        vec3 scatteredSun = sh.sunAtSurface * (visibility * (1.0 - aligned));
        sh.diffuseLight += scatteredSun * albedo * phase.x;
        // Contact shadows remove only the reflected sun, as on triangle leaves
        // (mesh.frag), so the tiers agree at the voxel switch.
        sunRadiance = sh.diffuseLight + sh.specularLight;
        addTransmittedSun(sh, transmitted, backNoL, visibility * aligned);
        sh.diffuseLight += scatteredSun * transmitted * phase.y;
    }
#ifdef DEBUG_ID
    vec3 debugSunDiffuse = sh.diffuseLight;
    vec3 debugSunSpecular = sh.specularLight;
#endif
    vec3 specularBeforeAmbient = sh.specularLight;
    vec3 diffuseBeforeAmbient = sh.diffuseLight;
    addAmbientLight(sh, s.occlusion);
    // With GI the probes stand for the crown in the diffuse term, but the sky
    // reflected along R still passes through the model: occlude it by the
    // model's optical depth along R.
    if (scene.giMode != 0u && sh.leafVisibility < 0.0) {
        vec3 R = reflect(-sh.V, sh.N);
        float crownR = exp(-voxelBoxDepth(axisWord, normalize(toLocal * R)));
        sh.specularLight = specularBeforeAmbient + (sh.specularLight - specularBeforeAmbient) *
                                                       mix(1.0, crownR, scene.microShadowStrength);
    }
    addTransmittedAmbient(sh, transmitted, s.occlusion);
    if (scene.giMode != 0u && sh.leafVisibility < 0.0) {
        sh.diffuseLight = diffuseBeforeAmbient +
                          (sh.diffuseLight - diffuseBeforeAmbient) *
                              mix(vec3(1.0), multiBounceOcclusion(occlusion, sh.diffuseColor), sh.gi.remaining);
    }
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
    writeDebugId(kDebugVoxels, inInfo.x, inInfo.w, relative, ~0u);
#ifdef DEBUG_ID
    vec3 debugScale = exposure * hazeTransmittance;
    beginInvocationInterlockARB();
    writeDebugLight(gl_FragCoord.z, debugSunDiffuse * debugScale, debugSunSpecular * debugScale,
                    (sh.diffuseLight - debugSunDiffuse) * debugScale,
                    (sh.specularLight - debugSunSpecular) * debugScale, vec2(0.0));
    endInvocationInterlockARB();
#endif

    // Motion of the flat point, each frame displayed with its own camera and
    // drop (static, so a still camera gives zero).
    vec4 current = scene.currentViewProjection * displayedPosition(scene, relative);
    vec4 previous = scene.previousViewProjection *
                    displayedPosition(scene, relative + (scene.cameraPosition -
                                                         scene.previousCameraPosition));
    outMotion = previous.xy / previous.w * 0.5 - current.xy / current.w * 0.5;
}
