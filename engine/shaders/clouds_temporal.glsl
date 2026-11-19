// Shared by the cloud resolve and composite: their push constants and the
// reprojection of a cloud by its depth, the camera and the wind. Define
// MERIDIAN_CUSTOM_PUSH_CONSTANTS and include common.glsl first.
#ifndef CLOUDS_TEMPORAL_GLSL
#define CLOUDS_TEMPORAL_GLSL

// Must match CloudConstants in C++.
layout(push_constant) uniform CloudConstants {
    SceneBuffer scene;
    // 0 on the first frame at this resolution: no usable history.
    uint historyValid;
    // The camera is in or near the layer (half-resolution march).
    uint inLayer;
} pc;

// The unjittered view ray through uv, as the march builds it.
vec3 cloudRay(vec2 uv) {
    vec4 nearPoint = inverse(pc.scene.currentViewProjection) * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    return normalize(nearPoint.xyz / nearPoint.w);
}

// Last frame's clip position of the cloud seen at distance d (m) along this
// frame's ray: the same air, moved back by the wind displacement since then,
// seen from the previous camera, each through its own curvature warp (the
// march's rays are straight in the displayed frame). d = 0 (no cloud):
// infinitely far, so only rotation moves it, as the sky.
vec4 cloudPreviousClip(vec3 direction, float d) {
    if (d <= 0.0) {
        return pc.scene.previousViewProjection * vec4(direction, 0.0);
    }
    vec3 displayed = direction * d;
    vec3 previous = vec3(displayed.x, displayed.y + curvatureDrop(pc.scene, displayed), displayed.z) +
                    (pc.scene.cameraPosition - pc.scene.previousCameraPosition);
    previous.xz -= pc.scene.clouds.windDisplacement;
    return pc.scene.previousViewProjection * displayedPosition(pc.scene, previous);
}

#endif
