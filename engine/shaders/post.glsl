#extension GL_EXT_buffer_reference : require
#include "exposure.glsl"

// Descriptor set shared by the passes after the upscaler (histogram,
// exposure, tonemap, grade LUT bake). Must match createDescriptors.
// The upscaler's display-sized HDR output.
layout(set = 0, binding = 0) uniform sampler2D resolved;
// Grade + AgX, baked by grade_lut.comp (which writes it at binding 2).
layout(set = 0, binding = 1) uniform sampler3D gradeLut;

layout(buffer_reference, std430) buffer HistogramBuffer {
    uint bins[kHistogramBins];
};

// Must match PostConstants in C++.
layout(push_constant) uniform PostConstants {
    float deltaSeconds;
    HistogramBuffer histogram;
    ExposureBuffer exposure;
    float exposureCompensation;
} post;
