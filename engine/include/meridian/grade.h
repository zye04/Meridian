#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "meridian/math/vec3.h"

namespace meridian {

// Scene-referred colour grade, applied in linear Rec.2020 before AgX. The
// default is identity. This is the CPU reference of the GPU LUT bake.

// Band centres and half-width in OKLCh hue degrees (raised-cosine windows).
constexpr float kGreensHue = 140.0f, kBluesHue = 250.0f, kBrownsHue = 60.0f;
constexpr float kHueBandHalfWidth = 45.0f;

// OKLCh hue band: rotation (positive = increasing hue) and chroma scale.
struct HueBand {
  float hueShiftDegrees = 0.0f;
  float saturation = 1.0f;
};

struct Grade {
  // Source white: CIE daylight locus at kelvin (Planckian below 4000 K,
  // blended over 4000..5000 K), moved by tint (Duv, positive = greener
  // source, so the image turns magenta) along the Planckian isotherm.
  float temperatureKelvin = 6504.0f;
  float tint = 0.0f;
  float exposureEv = 0.0f;
  Vec3 slope{1, 1, 1}, offset{0, 0, 0}, power{1, 1, 1};
  float contrast = 1.0f;
  float saturation = 1.0f;
  HueBand greens{}, blues{}, browns{};
};

// Row-major Rec.2020 -> Rec.2020 CAT02 adaptation from the (kelvin, tint)
// white to D65 (the daylight locus at 6504 K, so 6504/0 is exactly identity).
std::array<float, 9> whiteBalanceMatrix(float kelvin, float tint);

Vec3 gradeColor(const Grade &grade, Vec3 rec2020);

// grade_lut.comp's agxDisplay(): scene-linear Rec.709 in, display-linear
// Rec.709 out (not clamped).
Vec3 agxDisplay(Vec3 rec709);

// The LUT's log2 shaper, over a range past AgX's +4.03 EV clamp so a
// negative grade exposure can bring highlights back.
constexpr float kLutMinEv = -12.47393f, kLutMaxEv = 8.0f;
float shaperEncode(float x);
float shaperDecode(float t);

// Texel (i, j, k) at index i + N * (j + N * k) holds the graded AgX output
// for shaper coordinates (i, j, k) / (N - 1) of the AgX-inset colour.
constexpr uint32_t kGradeLutSize = 65;
std::vector<std::array<float, 4>> bakeGradeLut(const Grade &grade);

// The tonemap's lookup: inset, shaper, trilinear.
Vec3 sampleGradeLut(std::span<const std::array<float, 4>> lut, Vec3 rec709);

} // namespace meridian
