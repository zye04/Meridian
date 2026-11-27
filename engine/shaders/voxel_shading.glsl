// A voxel of a voxel cluster: the quad voxel_instance.mesh draws for it and
// its shading (voxel_instance.frag), shared with the visibility resolve,
// which rebuilds the voxel from its index in the phase's voxel list.
// Include after voxel_lod.glsl (and lighting.glsl for the shading).
#ifndef VOXEL_SHADING_GLSL
#define VOXEL_SHADING_GLSL

#include "scene_outputs.glsl"

struct VoxelQuad {
    uvec2 clusterEntry;
    LoadedInstance instance;
    ClusterInfo cluster;
    VoxelBrick brick;
    // The voxel's index in its brick, and its centre relative to the camera
    // (flat world).
    uint i;
    vec3 relative;
    uvec4 packed;
    // (instance, terrain sun, axis word, cluster).
    uvec4 info;
};

// Entry `index` of the voxel list `list` (phase * 2).
VoxelQuad loadVoxelQuad(SceneBuffer scene, uint list, uint index) {
    VoxelQuad q;
    uvec2 entry = scene.voxelInstances.entries[voxelQuadListStart(list) + index];
    q.i = entry.y & 63u;
    q.clusterEntry = scene.voxelInstances.entries[entry.x & 0x7FFFFFFFu];
    q.instance = loadInstance(scene, q.clusterEntry.x);
    q.cluster = scene.clusters.clusters[q.clusterEntry.y];
    VoxelPoolBuffer pools = scene.voxelPools;
    float v = q.cluster.voxelEdge;
    q.brick = pools.bricks.bricks[entry.y >> 6];
    uint voxel = voxelSlot(q.brick, q.i);
    vec3 local = (vec3(voxelCoords(q.brick, q.i)) + 0.5) * v;
    q.relative = (q.instance.transform * vec4(local, 1.0)).xyz - scene.cameraPosition;
    q.packed = pools.voxels.voxels[voxel];
    q.info = uvec4(q.clusterEntry.x, entry.x >> 31, pools.axis.words[voxel], q.clusterEntry.y);
    return q;
}

// The shading needs lighting.glsl; the mesh shader includes only the quad.
#ifdef LIGHTING_GLSL
// |N1 - N2|^2 between neighbouring voxels' lobe axes, for specular AA: a
// crown's leaves face every way, so neighbours' axes are about 60 degrees
// apart (|N1 - N2|^2 = 2 - 2 cos). The value stands for the squared normal
// change over both screen axes together (the sum the function expects).
const float kVoxelNeighbourNormalChange = 1.0;

SceneOutputs shadeVoxelPixel(vec3 relative, uvec4 voxel, uvec4 info) {
    SceneBuffer scene = pc.scene;
    LoadedInstance instance = loadInstance(scene, info.x);
    ClusterInfo cluster = scene.clusters.clusters[info.w];

    mat3 linear = mat3(instance.transform);
    float scale = instance.scale;
    mat3 toLocal = transpose(linear) / (scale * scale);
    vec3 world = scene.cameraPosition + relative;
    uint axisWord = info.z;

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
    vec3 sunRadiance = vec3(0.0);
    float backNoL = clamp(-dot(s.normal, sh.L), 0.0, 1.0);
    vec3 rayDirection = sampleSunDirection(scene, gl_FragCoord.xy);
    if (rayDirection.y > -sin(scene.sunAngularRadius)) {
        // The cascades hold the voxels as casters up to cascade 3 and the far
        // cells (the model's bricks) in cascade 4, ray-traced shadows the
        // model's triangles: either way the model shades itself.
        vec3 origin = world + rayDirection * (cluster.voxelEdge * scale);
        float visibility = traceSunVisibility(origin, origin, rayDirection, 0.0, false,
                                              origin - scene.cameraPosition, rayDirection);
        visibility *= float(info.y);
        if (sh.NoL > 0.0) {
            addSunLight(sh, visibility * aligned, 1.0);
        }
        addTransmittedSun(sh, transmitted, backNoL, visibility * aligned);
        // Light travels -L, scattered toward the camera travels -V.
        sh.diffuseLight += sh.sunAtSurface * (visibility * (1.0 - aligned)) *
                           leafPhase(-dot(sh.L, sh.V), albedo, transmitted);
        sunRadiance = sh.diffuseLight + sh.specularLight;
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
    if (scene.giMode != 0u) {
        vec3 R = reflect(-sh.V, sh.N);
        float crownR = exp(-voxelBoxDepth(axisWord, normalize(toLocal * R)));
        sh.specularLight = specularBeforeAmbient + (sh.specularLight - specularBeforeAmbient) *
                                                       mix(1.0, crownR, scene.microShadowStrength);
    }
    addTransmittedAmbient(sh, transmitted, s.occlusion);
    if (scene.giMode != 0u) {
        sh.diffuseLight = diffuseBeforeAmbient +
                          (sh.diffuseLight - diffuseBeforeAmbient) *
                              mix(vec3(1.0), multiBounceOcclusion(occlusion, sh.diffuseColor), sh.gi.remaining);
    }
    float hazeTransmittance;
    vec3 luminance = applyAerialPerspective(sh.diffuseLight + sh.specularLight, relative, 1.0,
                                            hazeTransmittance);
    vec3 exposure = scene.sunIlluminance * scene.exposure.exposure;
    SceneOutputs o;
    o.color = vec4(min(luminance * exposure, vec3(kMaxExposedLuminance)), 1.0);
    o.diffuseAlbedo = vec4(sh.diffuseColor, 1.0);
    o.specularAlbedo = vec4(mix(sh.dfg.xxx, sh.dfg.yyy, sh.f0), 1.0);
    o.normalRoughness = vec4(sh.N, sh.perceptualRoughness);
    o.sunLight =
        vec4(min(sunRadiance * hazeTransmittance * exposure, vec3(kMaxExposedLuminance)), 1.0);
    o.transparency = 0.0;
#ifdef DEBUG_ID
    vec3 debugScale = exposure * hazeTransmittance;
    o.debugSunDiffuse = debugSunDiffuse * debugScale;
    o.debugSunSpecular = debugSunSpecular * debugScale;
    o.debugAmbientDiffuse = (sh.diffuseLight - debugSunDiffuse) * debugScale;
    o.debugAmbientSpecular = (sh.specularLight - debugSunSpecular) * debugScale;
#endif

    // Motion of the flat point, each frame displayed with its own camera and
    // drop (static, so a still camera gives zero).
    vec4 current = scene.currentViewProjection * displayedPosition(scene, relative);
    vec4 previous = scene.previousViewProjection *
                    displayedPosition(scene, relative + (scene.cameraPosition -
                                                         scene.previousCameraPosition));
    o.motion = previous.xy / previous.w * 0.5 - current.xy / current.w * 0.5;
    return o;
}

#endif

#endif
