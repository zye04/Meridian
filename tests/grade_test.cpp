#include <doctest/doctest.h>
#include <meridian/grade.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>

using meridian::Vec3;

namespace {

bool near(Vec3 a, Vec3 b, float tolerance) {
  return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
         std::abs(a.z - b.z) <= tolerance;
}

// Literal reference matrices (BT.2087 / Ottosson), independent of grade.cpp.
Vec3 rec709To2020(Vec3 c) {
  return {0.627403896f * c.x + 0.329283039f * c.y + 0.043313065f * c.z,
          0.069097289f * c.x + 0.919540395f * c.y + 0.011362316f * c.z,
          0.016391439f * c.x + 0.088013308f * c.y + 0.895595253f * c.z};
}

Vec3 rec2020To709(Vec3 c) {
  return {1.660490296f * c.x - 0.587639106f * c.y - 0.072851598f * c.z,
          -0.124549970f * c.x + 1.132899922f * c.y - 0.008347964f * c.z,
          -0.018151119f * c.x - 0.100578724f * c.y + 1.118729878f * c.z};
}

// OKLab hue in degrees of a linear Rec.709 (sRGB primaries) colour.
float oklabHue(Vec3 c) {
  float l = std::cbrt(0.4122214708f * c.x + 0.5363325363f * c.y +
                      0.0514459929f * c.z);
  float m = std::cbrt(0.2119034982f * c.x + 0.6806995451f * c.y +
                      0.1073969566f * c.z);
  float s = std::cbrt(0.0883024619f * c.x + 0.2817188376f * c.y +
                      0.6299787005f * c.z);
  float a = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
  float b = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
  return std::atan2(b, a) * 180.0f / std::numbers::pi_v<float>;
}

float hueDifference(float a, float b) {
  return std::remainder(a - b, 360.0f);
}

float srgb(float c) {
  c = std::clamp(c, 0.0f, 1.0f);
  return c <= 0.0031308f ? 12.92f * c
                         : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

float srgbError(Vec3 a, Vec3 b) {
  return (std::abs(srgb(a.x) - srgb(b.x)) + std::abs(srgb(a.y) - srgb(b.y)) +
          std::abs(srgb(a.z) - srgb(b.z))) *
         255.0f / 3.0f;
}

// Random hue and saturation, scaled so the largest channel is log-uniform
// in [2^minEv, 2^maxEv].
std::vector<Vec3> randomColors(float minEv, float maxEv) {
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  std::vector<Vec3> colors;
  for (int n = 0; n < 10000; ++n) {
    float hue = unit(rng) * 6.0f;
    float saturation = unit(rng);
    float f = hue - std::floor(hue);
    float rgb[3];
    switch (static_cast<int>(hue) % 6) {
    case 0: rgb[0] = 1; rgb[1] = f; rgb[2] = 0; break;
    case 1: rgb[0] = 1 - f; rgb[1] = 1; rgb[2] = 0; break;
    case 2: rgb[0] = 0; rgb[1] = 1; rgb[2] = f; break;
    case 3: rgb[0] = 0; rgb[1] = 1 - f; rgb[2] = 1; break;
    case 4: rgb[0] = f; rgb[1] = 0; rgb[2] = 1; break;
    default: rgb[0] = 1; rgb[1] = 0; rgb[2] = 1 - f; break;
    }
    float scale = std::exp2(minEv + unit(rng) * (maxEv - minEv));
    colors.push_back(Vec3{1 - saturation * (1 - rgb[0]),
                          1 - saturation * (1 - rgb[1]),
                          1 - saturation * (1 - rgb[2])} *
                     scale);
  }
  return colors;
}

float meanLutError(const std::vector<Vec3> &colors) {
  auto lut = meridian::bakeGradeLut(meridian::Grade{});
  double sum = 0.0;
  float worst = 0.0f;
  for (Vec3 c : colors) {
    float error = srgbError(meridian::sampleGradeLut(lut, c),
                            meridian::agxDisplay(c));
    sum += error;
    worst = std::max(worst, error);
  }
  float mean = static_cast<float>(sum / colors.size());
  std::printf("lut vs agx (%u^3): mean %.4f/255, max %.4f/255\n",
              meridian::kGradeLutSize, mean, worst);
  return mean;
}

} // namespace

TEST_CASE("identity grade is identity") {
  for (Vec3 c : {Vec3{0.18f, 0.18f, 0.18f}, Vec3{1, 0.2f, 0.05f}, Vec3{0, 0, 0},
                 Vec3{8, 4, 1}}) {
    Vec3 graded = meridian::gradeColor(meridian::Grade{}, c);
    CHECK(near(graded, c, 1e-5f));
  }
}

TEST_CASE("white balance") {
  auto identity = meridian::whiteBalanceMatrix(6504.0f, 0.0f);
  for (int i = 0; i < 9; ++i)
    CHECK(std::abs(identity[i] - (i % 4 == 0 ? 1.0f : 0.0f)) <= 1e-3f);

  // Planckian 3200 K white, CIE 1931 xy (Wyszecki & Stiles), to Rec.2020.
  const float x = 0.4234f, y = 0.3990f;
  const float X = x / y, Y = 1.0f, Z = (1.0f - x - y) / y;
  Vec3 white{1.716651188f * X - 0.355670784f * Y - 0.253366281f * Z,
             -0.666684352f * X + 1.616481237f * Y + 0.015768546f * Z,
             0.017639857f * X - 0.042770613f * Y + 0.942103121f * Z};
  auto m = meridian::whiteBalanceMatrix(3200.0f, 0.0f);
  Vec3 balanced{m[0] * white.x + m[1] * white.y + m[2] * white.z,
                m[3] * white.x + m[4] * white.y + m[5] * white.z,
                m[6] * white.x + m[7] * white.y + m[8] * white.z};
  CHECK(balanced.x == doctest::Approx(balanced.y).epsilon(1e-3));
  CHECK(balanced.z == doctest::Approx(balanced.y).epsilon(1e-3));

  // Positive tint is a greener source white, so a neutral turns magenta.
  auto tinted = meridian::whiteBalanceMatrix(6504.0f, 0.01f);
  float r = tinted[0] + tinted[1] + tinted[2];
  float g = tinted[3] + tinted[4] + tinted[5];
  float b = tinted[6] + tinted[7] + tinted[8];
  CHECK(g < r);
  CHECK(g < b);
}

TEST_CASE("cdl and contrast") {
  meridian::Grade slope{.slope = {2, 2, 2}};
  CHECK(near(meridian::gradeColor(slope, {0.18f, 0.18f, 0.18f}),
             {0.36f, 0.36f, 0.36f}, 1e-5f));
  meridian::Grade power{.power = {2, 2, 2}};
  CHECK(near(meridian::gradeColor(power, {0.5f, 0.5f, 0.5f}),
             {0.25f, 0.25f, 0.25f}, 1e-5f));
  meridian::Grade contrast{.contrast = 2};
  CHECK(near(meridian::gradeColor(contrast, {0.18f, 0.18f, 0.18f}),
             {0.18f, 0.18f, 0.18f}, 1e-5f));
  CHECK(near(meridian::gradeColor(contrast, {0.36f, 0.36f, 0.36f}),
             {0.72f, 0.72f, 0.72f}, 1e-5f));
}

TEST_CASE("saturation 0 gives luminance") {
  meridian::Grade grade{.saturation = 0};
  Vec3 c{1, 0.2f, 0.05f};
  float luminance = 0.2627f * c.x + 0.6780f * c.y + 0.0593f * c.z;
  CHECK(near(meridian::gradeColor(grade, c),
             {luminance, luminance, luminance}, 1e-5f));
}

TEST_CASE("hue band") {
  meridian::Grade grade{.greens = {.hueShiftDegrees = 10}};
  auto shift = [&](Vec3 rec709) {
    Vec3 graded = rec2020To709(
        meridian::gradeColor(grade, rec709To2020(rec709)));
    return hueDifference(oklabHue(graded), oklabHue(rec709));
  };
  Vec3 green{0.12f, 0.35f, 0.04f};
  Vec3 blue{0.05f, 0.12f, 0.6f};
  INFO("green hue ", oklabHue(green), ", blue hue ", oklabHue(blue));
  CHECK(std::abs(shift(green) - 10.0f) <= 0.5f);
  CHECK(std::abs(shift(blue)) < 0.5f);
  Vec3 grey{0.3f, 0.3f, 0.3f};
  CHECK(near(meridian::gradeColor(grade, grey), grey, 1e-5f));
}

TEST_CASE("shaper") {
  CHECK(meridian::shaperEncode(0.18f) == doctest::Approx(0.488426f));
  for (float x : {1e-3f, 0.18f, 2.0f, 200.0f})
    CHECK(meridian::shaperDecode(meridian::shaperEncode(x)) ==
          doctest::Approx(x));
}

TEST_CASE("lut matches agx") {
  CHECK(meanLutError(randomColors(-12.0f, 4.0f)) <= 0.3f);
}

TEST_CASE("lut matches agx for highlights") {
  CHECK(meanLutError(randomColors(0.0f, 6.0f)) <= 0.3f);
}

TEST_CASE("negative exposure recovers highlights") {
  auto lut = meridian::bakeGradeLut(meridian::Grade{.exposureEv = -2});
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> ev(4.0f, 6.0f);
  double toExposed = 0.0, toUnexposed = 0.0;
  const int count = 10000;
  for (int n = 0; n < count; ++n) {
    Vec3 c{std::exp2(ev(rng)), std::exp2(ev(rng)), std::exp2(ev(rng))};
    Vec3 sampled = meridian::sampleGradeLut(lut, c);
    toExposed += srgbError(sampled, meridian::agxDisplay(c * 0.25f));
    toUnexposed += srgbError(sampled, meridian::agxDisplay(c));
  }
  std::printf("exposure -2 lut: vs agx(c/4) %.4f/255, vs agx(c) %.4f/255\n",
              toExposed / count, toUnexposed / count);
  CHECK(toExposed / count <= 0.3);
  CHECK(toUnexposed / count > 1.0);
}

TEST_CASE("the grade file round-trips every field") {
  meridian::Grade grade;
  grade.temperatureKelvin = 5200.5f;
  grade.tint = -0.0123f;
  grade.exposureEv = 0.75f;
  grade.slope = {1.1f, 0.9f, 1.05f};
  grade.offset = {0.01f, -0.02f, 0.003f};
  grade.power = {0.95f, 1.0f, 1.2f};
  grade.contrast = 1.4f;
  grade.saturation = 0.8f;
  grade.greens = {-12.5f, 1.15f};
  grade.blues = {7.0f, 0.6f};
  grade.browns = {30.0f, 1.9f};
  const meridian::Grade back = meridian::gradeFromJson(meridian::gradeToJson(grade));
  CHECK(back.temperatureKelvin == grade.temperatureKelvin);
  CHECK(back.tint == grade.tint);
  CHECK(back.exposureEv == grade.exposureEv);
  CHECK(near(back.slope, grade.slope, 0.0f));
  CHECK(near(back.offset, grade.offset, 0.0f));
  CHECK(near(back.power, grade.power, 0.0f));
  CHECK(back.contrast == grade.contrast);
  CHECK(back.saturation == grade.saturation);
  CHECK(back.greens.hueShiftDegrees == grade.greens.hueShiftDegrees);
  CHECK(back.greens.saturation == grade.greens.saturation);
  CHECK(back.blues.hueShiftDegrees == grade.blues.hueShiftDegrees);
  CHECK(back.blues.saturation == grade.blues.saturation);
  CHECK(back.browns.hueShiftDegrees == grade.browns.hueShiftDegrees);
  CHECK(back.browns.saturation == grade.browns.saturation);
}

TEST_CASE("a partial grade file keeps the defaults and ignores other keys") {
  const meridian::Grade grade = meridian::gradeFromJson(
      R"({"haze": "summer", "contrast": 1.5, "greens": {"saturation": 1.3}})");
  CHECK(grade.contrast == 1.5f);
  CHECK(grade.greens.saturation == 1.3f);
  CHECK(grade.greens.hueShiftDegrees == 0.0f);
  CHECK(grade.saturation == 1.0f);
  CHECK(grade.temperatureKelvin == 6504.0f);
  CHECK(near(grade.slope, {1, 1, 1}, 0.0f));
  CHECK(meridian::jsonString(R"({"haze": "summer"})", "haze") == "summer");
  CHECK(!meridian::jsonNumber(R"({"a": 1})", "b"));
}

TEST_CASE("missing grade keys keep the base and non-finite numbers are rejected") {
  meridian::Grade base;
  base.contrast = 1.7f;
  base.greens = {5.0f, 1.4f};
  const meridian::Grade grade = meridian::gradeFromJson(
      R"({"saturation": 0.5, "tint": nan, "exposureEv": inf, "slope": [1, nan, 1]})", base);
  CHECK(grade.saturation == 0.5f);
  CHECK(grade.contrast == 1.7f);
  CHECK(grade.greens.hueShiftDegrees == 5.0f);
  CHECK(grade.tint == base.tint);
  CHECK(grade.exposureEv == base.exposureEv);
  CHECK(near(grade.slope, base.slope, 0.0f));
}
