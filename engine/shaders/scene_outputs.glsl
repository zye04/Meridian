// What a scene pass's opaque, masked or blended surface writes to the scene
// attachments (architecture.md "Scene attachments"), returned by the shading
// functions the forward shaders and the visibility resolve share.
#ifndef SCENE_OUTPUTS_GLSL
#define SCENE_OUTPUTS_GLSL

struct SceneOutputs {
    vec4 color;
    vec2 motion;
    vec4 diffuseAlbedo;
    vec4 specularAlbedo;
    vec4 normalRoughness;
    vec4 sunLight;
    // Blended surfaces only: the transparency mask's coverage.
    float transparency;
#ifdef DEBUG_ID
    // The light split for writeDebugLight, exposed (haze included).
    vec3 debugSunDiffuse;
    vec3 debugSunSpecular;
    vec3 debugAmbientDiffuse;
    vec3 debugAmbientSpecular;
#endif
};

#endif
