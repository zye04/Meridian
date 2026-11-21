#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#include "common.glsl"
#include "atmosphere.glsl"
#include "material.glsl"
#include "terrain.glsl"
#include "lighting.glsl"
#include "protovoxel.glsl"

// A voxel of a tree's prototype voxels (voxel_instance.mesh draws it as a
// camera-facing quad), shaded flat at its center: its normal, albedo and
// baked sky occlusion. The crown shades itself through the voxel's baked
// optical depth toward the sun (sunlit tops, dark undersides); the sun ray
// traces the scene's objects from where it leaves the prototype's box; the
// terrain's sun visibility comes per brick from voxel_bricks.comp. Over the switch band the voxels draw
// where the dither is below their weight, the clusters where it is above.

layout(location = 0) flat in vec3 inRelative;
layout(location = 1) flat in uvec2 inVoxel;
layout(location = 2) flat in uvec2 inBrick;
layout(location = 3) flat in vec3 inLocal;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outMotion;
// The guides and the sun's exposed radiance, as in mesh.frag; the
// transparency mask (location 2) is not written (opaque).
layout(location = 3) out vec4 outDiffuseAlbedo;
layout(location = 4) out vec4 outSpecularAlbedo;
layout(location = 5) out vec4 outNormalRoughness;
layout(location = 6) out vec4 outSunLight;

const float kVoxelPerceptualRoughness = 0.8;
// Where the sun ray leaves the box, so it doesn't start inside the crown.
const float kExitBias = 0.1;

void main() {
    SceneBuffer scene = pc.scene;
    LoadedInstance instance = loadInstance(scene, inBrick.x);
    PrototypeInfo prototype = scene.prototypes.prototypes[instance.meshOrPrototype];
#ifdef VOXEL_BAND
    if (voxelDither(gl_FragCoord.xy, scene.frameIndex) >=
        instanceVoxelBlend(scene, instance, prototype)) {
        discard;
    }
#endif

    mat3 linear = mat3(instance.transform);
    float scale = instance.scale;
    mat3 toLocal = transpose(linear) / (scale * scale);
    vec3 relative = inRelative;
    vec3 world = scene.cameraPosition + relative;
    uvec2 voxel = inVoxel;

    Surface s;
    s.baseColor = scene.farField.field.palette.albedo[voxelAlbedo(voxel.x)].rgb;
    s.alpha = 1.0;
    s.metallic = 0.0;
    s.perceptualRoughness = kVoxelPerceptualRoughness;
    s.occlusion = float(voxel.y & 0xFFu) * (1.0 / 255.0);
    s.microOcclusion = 1.0;
    s.sunShadow = 1.0;
    s.emissive = vec3(0.0);
    // Foliage is two-sided, like the leaf cards the voxels average.
    s.normal = normalize(linear * voxelNormal(voxel.x));
    if (dot(s.normal, relative) > 0.0) {
        s.normal = -s.normal;
    }

    Shading sh = beginShading(s, relative);
    vec3 sunRadiance = vec3(0.0);
    if (sh.NoL > 0.0) {
        vec3 rayDirection = sampleSunDirection(scene, gl_FragCoord.xy);
        vec3 sunLocal = normalize(toLocal * rayDirection);
        PrototypeVoxels box = scene.protoVoxels.prototypes.prototypes[instance.meshOrPrototype];
        float selfDepth = protoVoxelDepth(voxel.y, sunLocal);
        vec3 exitPoint =
            world + rayDirection * (protoBoxExit(box, inLocal, sunLocal) * scale + kExitBias);
        float visibility = float(inBrick.y >> 31) * exp(-selfDepth) *
                           traceSunVisibility(exitPoint, exitPoint, rayDirection, 0.0, false,
                                              exitPoint - scene.cameraPosition, s.normal);
        addSunLight(sh, visibility, 1.0);
        sunRadiance = sh.diffuseLight + sh.specularLight;
    }
    addAmbientLight(sh, s.occlusion);
    float hazeTransmittance;
    vec3 luminance = applyAerialPerspective(sh.diffuseLight + sh.specularLight, relative, 1.0,
                                            hazeTransmittance);
    vec3 exposure = scene.sunIlluminance * scene.exposure.exposure;
    outColor = vec4(luminance * exposure, 1.0);
    outDiffuseAlbedo = vec4(sh.diffuseColor, 1.0);
    outSpecularAlbedo = vec4(mix(sh.dfg.xxx, sh.dfg.yyy, sh.f0), 1.0);
    outNormalRoughness = vec4(sh.N, sh.perceptualRoughness);
    outSunLight = vec4(sunRadiance * hazeTransmittance * exposure, 1.0);

    // Motion of the flat hit, each frame displayed with its own camera and
    // drop (static, so a still camera gives zero).
    vec4 current = scene.currentViewProjection * displayedPosition(scene, relative);
    vec4 previous = scene.previousViewProjection *
                    displayedPosition(scene, relative + (scene.cameraPosition -
                                                         scene.previousCameraPosition));
    outMotion = previous.xy / previous.w * 0.5 - current.xy / current.w * 0.5;
}
