// Auto-exposure state and histogram mapping, shared by the scene shaders
// (which read the exposure) and the exposure passes (which update it).
#ifndef EXPOSURE_GLSL
#define EXPOSURE_GLSL

#extension GL_EXT_buffer_reference : require

// Must match ExposureState in C++.
layout(buffer_reference, std430) buffer ExposureBuffer {
    // Multiplier from scene luminance (cd/m^2) to display range.
    float exposure;
    float ev100;
    // The exposure the previous frame shaded with: the TAA history is
    // pre-exposed by it. Whatever writes `exposure` must set this too.
    float previousExposure;
};

// Sky irradiance on an upward-facing surface (sun illuminance 1), integrated
// each frame from the sky-view LUT with the sky capture's clouds over it,
// and from the sky-view LUT alone (the clear sky that lights the clouds).
// Must match SkyIrradiance in C++.
layout(buffer_reference, std430) buffer SkyIrradianceBuffer {
    vec3 irradiance;
    // Illuminance (lux) on open, upward-facing ground at the camera: the sun
    // through the atmosphere and the cloud shadow, plus the sky. The
    // exposure's lower bound.
    float openIlluminance;
    vec3 clearIrradiance;
};

// The histogram covers log2 luminance from 2^-10 (deep night) to 2^20
// (brighter than sunlit snow) in 256 bins.
const float kMinLogLuminance = -10.0;
const float kLogLuminanceRange = 30.0;
const uint kHistogramBins = 256u;

#endif
