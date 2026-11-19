// Descriptor set shared by the post-processing passes. Two copies exist and
// swap every frame, so the history images ping-pong without updates.
layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D motion;
layout(set = 0, binding = 2) uniform sampler2D historyPrevious;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D historyNext;
// The same image as historyNext, read by the passes after the resolve.
layout(set = 0, binding = 4) uniform sampler2D resolved;

layout(push_constant) uniform PostConstants {
    uint historyValid;
} post;
