// The shading of a cluster triangle's pixel (opaque, masked and blended
// meshes), shared by mesh.frag and the visibility resolve. Include after
// surface.glsl. The caller fills ClusterPixel: from varyings and screen
// derivatives in mesh.frag, from the triangle's three vertices and analytic
// derivatives in the resolve. Only the specular AA's normal derivative needs
// the caller's pixel quad (CLUSTER_SHADING_SCREEN_DERIVATIVES): it is taken
// of the normal-mapped shading normal, which the resolve has no gradient of.
#ifndef CLUSTER_SHADING_GLSL
#define CLUSTER_SHADING_GLSL

#include "scene_outputs.glsl"

struct ClusterPixel {
    vec3 relative;
    // Screen derivatives of relative (dpdx, dpdy) and texture LOD gradients
    // (SurfaceInput).
    vec3 relativeDx;
    vec3 relativeDy;
    vec2 uvDx;
    vec2 uvDy;
    vec2 terrainUvDx;
    vec2 terrainUvDy;
    // Interpolated vertex normal (not normalized, not flipped).
    vec3 normal;
    // The triangle's normal, any length, any sign.
    vec3 geometricNormal;
    bool frontFacing;
    vec2 uv;
    vec4 tangent;
    uint materialIndex;
    // How far (m) the LOD cluster may deviate from the full-detail surface
    // the shadow rays are traced against; 0 at full detail.
    float lodError;
    // Height within the mesh, 0 at its base to 1 at its top (1 where
    // unknown).
    float height;
    // Placed instances' prototype tint, 1 otherwise.
    vec3 tint;
    // A streamed plant's shrink factor (1 = full size): it shades like the
    // terrain it turns into as this goes to 0.
    float streamFade;
    // Sky visibility inside a tree crown (cluster_vertex.glsl), 1 elsewhere.
    float crownVisibility;
    // The cluster's terrain sun visibility (cluster_cull.comp), or -1 where
    // the terrain is marched here per pixel.
    float terrainSun;
    vec4 currentClip;
    vec4 previousClip;
    // Without CLUSTER_SHADING_SCREEN_DERIVATIVES: the vertex normal's screen
    // derivatives, for the specular AA.
    vec3 normalDx;
    vec3 normalDy;
};

// The sun's visibility at a point on the triangle (traceSunVisibility),
// offset along the triangle's normal.
float clusterSunVisibility(ClusterPixel p, vec3 position, vec3 normal) {
    vec3 origin = position + normal * max(kShadowRayBias, p.lodError);
    if (p.terrainSun == 0.0) {
        return 0.0;
    }
    if (p.terrainSun > 0.0) {
        return traceSunVisibility(origin, origin, sampleSunDirection(pc.scene, SHADING_PIXEL),
                                  0.0, false, p.relative, normal);
    }
    // An object's base can sit below the bilinear heightfield (detail
    // displacement, coarse ridges), where the march would hit at once.
    vec3 marchOrigin = origin;
    marchOrigin.y = max(origin.y, terrainHeight(pc.scene.terrain, origin.xz) + kShadowRayBias);
    return traceSunVisibility(origin, marchOrigin, sampleSunDirection(pc.scene, SHADING_PIXEL),
                              0.0, true, p.relative, normal);
}

SceneOutputs shadeClusterPixel(ClusterPixel p) {
    vec3 worldPosition = p.relative + pc.scene.cameraPosition;
    surfaceIn.relativePosition = p.relative;
    surfaceIn.worldPosition = worldPosition;
    surfaceIn.dpdx = p.relativeDx;
    surfaceIn.dpdy = p.relativeDy;
    surfaceIn.uv = p.uv;
    surfaceIn.uvDx = p.uvDx;
    surfaceIn.uvDy = p.uvDy;
    surfaceIn.terrainUvDx = p.terrainUvDx;
    surfaceIn.terrainUvDy = p.terrainUvDy;
    surfaceIn.tangent = p.tangent;
    surfaceIn.tint = p.tint;
    surfaceIn.height = p.height;
    surfaceIn.streamFade = p.streamFade;
    surfaceIn.crownVisibility = p.crownVisibility;

    Material material = pc.scene.materials.materials[p.materialIndex];
    float facing = p.frontFacing ? 1.0 : -1.0;
    vec3 normal = normalize(p.normal) * facing;
    Surface s = materialSurface(material, normal);
    vec3 fadeNormal = vec3(0.0);
    if (p.streamFade < 1.0) {
        fadeNormal = terrainNormal();
        s.normal = normalize(mix(fadeNormal, s.normal, p.streamFade));
    }
#ifdef CLUSTER_SHADING_SCREEN_DERIVATIVES
    vec3 normalDx = dFdx(s.normal);
    vec3 normalDy = dFdy(s.normal);
#endif
    if (pc.scene.foliageSpecularAa != 0u && material.diffuseTransmission > 0.0) {
#ifndef CLUSTER_SHADING_SCREEN_DERIVATIVES
        // The shading normal one pixel over, normal map included, as the
        // forward path's dFdx of it sees.
        vec3 nx = mappedNormal(material, normalize(p.normal + p.normalDx) * facing, p.uv + p.uvDx);
        vec3 ny = mappedNormal(material, normalize(p.normal + p.normalDy) * facing, p.uv + p.uvDy);
        if (p.streamFade < 1.0) {
            nx = normalize(mix(fadeNormal, nx, p.streamFade));
            ny = normalize(mix(fadeNormal, ny, p.streamFade));
        }
        vec3 normalDx = nx - s.normal;
        vec3 normalDy = ny - s.normal;
#endif
        s.perceptualRoughness = specularAaRoughness(
            s.perceptualRoughness, dot(normalDx, normalDx) + dot(normalDy, normalDy));
    }
    Shading sh = beginShading(s, p.relative);
    bool leaf = material.diffuseTransmission > 0.0;
    vec3 transmitted = vec3(0.0);
    if (leaf) {
        vec3 transmissionColor = material.diffuseTransmissionColor * p.tint;
        if (material.diffuseTransmissionColorTexture != kNoTexture) {
            transmissionColor *= sampleTexture(material.diffuseTransmissionColorTexture, p.uv).rgb;
        }
        transmitted = transmissionColor * (material.diffuseTransmission * (1.0 - s.metallic));
        sh.diffuseColor *= 1.0 - material.diffuseTransmission;
        // As the voxels' model occlusion: scaled by the micro shadow strength.
        float crown = mix(1.0, p.crownVisibility, pc.scene.microShadowStrength);
        setLeafLight(sh, sh.diffuseColor + transmitted, crown, crown);
    }

    // Offset shadow rays along the true triangle normal: the smooth shading
    // normal can point below the surface and make the ray hit its own
    // triangle.
    vec3 geometricNormal = normalize(p.geometricNormal);
    if (dot(geometricNormal, normal) < 0.0) {
        geometricNormal = -geometricNormal;
    }

    vec3 sunRadiance = vec3(0.0);
    // A fading plant's normal is blended toward the terrain's (up), which
    // turns off its back faces' transmission; let them take the sun.
    bool facesSun = dot(geometricNormal, sh.L) > 0.0 || p.streamFade < 1.0;
    if (sh.NoL > 0.0 && facesSun) {
        float visibility = clusterSunVisibility(p, worldPosition, geometricNormal);
        float plantSun = mix(1.0, plantOcclusion(material), 0.5);
        addSunLight(sh, visibility,
                    plantSun *
                        mix(1.0, microShadow(sh.NoL, s.microOcclusion), pc.scene.microShadowStrength) *
                        s.sunShadow);
        sunRadiance = sh.diffuseLight + sh.specularLight;
    }
    // A thin surface's lit side is the geometric one; where the shading
    // normal disagrees, its Lambert lobes take |N.L| instead of nothing
    // (pbrt: lobe by the geometric normal, BSDF in the shading frame).
    // A fading plant's normal is blended up, toward the terrain's; it keeps
    // transmission only where both normals face away.
    if (leaf) {
        float NgoL = dot(geometricNormal, sh.L);
        float NoL = dot(sh.N, sh.L);
        if (NgoL < 0.0 && (NoL < 0.0 || p.streamFade >= 1.0)) {
            addTransmittedSun(sh, transmitted, min(abs(NoL), 1.0),
                              clusterSunVisibility(p, worldPosition, -geometricNormal));
        } else if (NoL <= 0.0) {
            addTransmittedSun(sh, sh.diffuseColor, min(-NoL, 1.0),
                              clusterSunVisibility(p, worldPosition, geometricNormal));
        }
    }

    // The leaf light's canopy ambient holds the crown itself.
    if (sh.leafVisibility < 0.0) {
        s.occlusion *= crownOcclusion();
    }
#ifdef DEBUG_ID
    vec3 debugSunDiffuse = sh.diffuseLight;
    vec3 debugSunSpecular = sh.specularLight;
#endif
    addAmbientLight(sh, s.occlusion);
    if (leaf) {
        addTransmittedAmbient(sh, transmitted, s.occlusion);
    }

#ifdef ALPHA_BLEND
    // Premultiplied alpha (blend: one, one-minus-source-alpha).
    float coverage = s.alpha;
    vec3 luminance = sh.diffuseLight * coverage + sh.specularLight;
#else
    float coverage = 1.0;
    vec3 luminance = sh.diffuseLight + sh.specularLight;
#endif

    float hazeTransmittance;
    luminance = applyAerialPerspective(luminance, p.relative, coverage, hazeTransmittance);
    sunRadiance *= hazeTransmittance;

    SceneOutputs o;
    // Emissive is already in physical units (cd/m^2), not relative to the sun.
    vec3 exposed = luminance * pc.scene.sunIlluminance + s.emissive;
    o.color = vec4(exposed * pc.scene.exposure.exposure, coverage);

    vec2 currentUv = p.currentClip.xy / p.currentClip.w * 0.5;
    vec2 previousUv = p.previousClip.xy / p.previousClip.w * 0.5;
    o.motion = previousUv - currentUv;
#ifdef DEBUG_ID
    vec3 debugScale = pc.scene.sunIlluminance * pc.scene.exposure.exposure * hazeTransmittance;
    o.debugSunDiffuse = debugSunDiffuse * debugScale;
    o.debugSunSpecular = debugSunSpecular * debugScale;
    o.debugAmbientDiffuse = (sh.diffuseLight - debugSunDiffuse) * debugScale;
    o.debugAmbientSpecular = (sh.specularLight - debugSunSpecular) * debugScale;
#endif
#ifdef ALPHA_BLEND
    o.transparency = coverage;
    o.sunLight = vec4(0.0, 0.0, 0.0, coverage);
#else
    o.transparency = 0.0;
    o.diffuseAlbedo = vec4(sh.diffuseColor, 1.0);
    o.specularAlbedo = vec4(mix(sh.dfg.xxx, sh.dfg.yyy, sh.f0), 1.0);
    o.normalRoughness = vec4(sh.N, sh.perceptualRoughness);
    o.sunLight = vec4(sunRadiance * pc.scene.sunIlluminance * pc.scene.exposure.exposure, 1.0);
#endif
    return o;
}

#endif
