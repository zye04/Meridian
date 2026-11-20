#include "meridian/grade.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <numbers>
#include <utility>

namespace meridian {
namespace {

// Row-major 3x3; built in double, applied in float like the shader.
using Mat3 = std::array<double, 9>;

constexpr Mat3 multiply(const Mat3 &a, const Mat3 &b) {
  Mat3 r{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k)
        r[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
  return r;
}

constexpr Mat3 inverse(const Mat3 &m) {
  Mat3 c{m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8],
         m[1] * m[5] - m[2] * m[4], m[5] * m[6] - m[3] * m[8],
         m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
         m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7],
         m[0] * m[4] - m[1] * m[3]};
  double det = m[0] * c[0] + m[1] * c[3] + m[2] * c[6];
  for (double &x : c)
    x /= det;
  return c;
}

Vec3 apply(const Mat3 &m, Vec3 v) {
  auto row = [&](int i) {
    return static_cast<float>(m[i * 3]) * v.x +
           static_cast<float>(m[i * 3 + 1]) * v.y +
           static_cast<float>(m[i * 3 + 2]) * v.z;
  };
  return {row(0), row(1), row(2)};
}

// grade.glsl's and grade_lut.comp's matrices, transposed from GLSL's
// column-major mat3.
constexpr Mat3 kRec709ToAgxInset{
    0.544814816, 0.373786466, 0.081398195, 0.140416759, 0.754137100,
    0.105444695, 0.088810540, 0.178871522, 0.732317593};
constexpr Mat3 kAgxOutset{
    1.127100582,  -0.110606643, -0.016493939, -0.141329763, 1.157823702,
    -0.016493939, -0.141329763, -0.110606643, 1.251936407};
constexpr Mat3 kRec2020ToRec709{
    1.660490296,  -0.587639106, -0.072851598, -0.124549970, 1.132899922,
    -0.008347964, -0.018151119, -0.100578724, 1.118729878};
constexpr Mat3 kRec709ToRec2020 = inverse(kRec2020ToRec709);
constexpr Mat3 kAgxInsetToRec709 = inverse(kRec709ToAgxInset);

constexpr float kAgxMinEv = -12.47393f, kAgxMaxEv = 4.026069f;

constexpr Mat3 kRec2020ToXyz{0.636958048, 0.144616904, 0.168880975,
                             0.262700212, 0.677998072, 0.059301716,
                             0.0,         0.028072693, 1.060985058};
constexpr Mat3 kXyzToRec2020 = inverse(kRec2020ToXyz);
constexpr Mat3 kCat02{0.7328, 0.4296,  -0.1624, -0.7036, 1.6975,
                      0.0061, 0.0030, 0.0136,  0.9834};

// OKLab (Ottosson 2020): XYZ -> LMS, then cube root -> Lab.
constexpr Mat3 kRec2020ToOklabLms =
    multiply(Mat3{0.8189330101, 0.3618667424, -0.1288597137, 0.0329845436,
                  0.9293118715, 0.0361456387, 0.0482003018, 0.2643662691,
                  0.6338517070},
             kRec2020ToXyz);
constexpr Mat3 kOklabLmsToRec2020 = inverse(kRec2020ToOklabLms);
constexpr Mat3 kOklabLmsToLab{0.2104542553, 0.7936177850,  -0.0040720468,
                              1.9779984951, -2.4285922050, 0.4505937099,
                              0.0259040371, 0.7827717662,  -0.8086757660};
constexpr Mat3 kOklabLabToLms = inverse(kOklabLmsToLab);

struct Uv {
  double u, v;
};

// CIE 1960 uv of the Planckian locus (Krystek 1985, 1000..15000 K).
Uv planckianUv(double t) {
  return {(0.860117757 + 1.54118254e-4 * t + 1.28641212e-7 * t * t) /
              (1.0 + 8.42420235e-4 * t + 7.08145163e-7 * t * t),
          (0.317398726 + 4.22806245e-5 * t + 4.20481691e-8 * t * t) /
              (1.0 - 2.89741816e-5 * t + 1.61456053e-7 * t * t)};
}

// CIE daylight locus (CIE 15:2004), 4000..25000 K.
Uv daylightUv(double t) {
  double x = t <= 7000.0 ? -4.6070e9 / (t * t * t) + 2.9678e6 / (t * t) +
                               0.09911e3 / t + 0.244063
                         : -2.0064e9 / (t * t * t) + 1.9018e6 / (t * t) +
                               0.24748e3 / t + 0.237040;
  double y = -3.0 * x * x + 2.870 * x - 0.275;
  double d = -2.0 * x + 12.0 * y + 3.0;
  return {4.0 * x / d, 6.0 * y / d};
}

// Daylight where it is defined, Planckian below; blended over 4000..5000 K
// so the temperature has no jump.
Uv whiteUv(double kelvin, double tint) {
  double t = std::clamp(kelvin, 1000.0, 25000.0);
  Uv p = planckianUv(t);
  double s = std::clamp((t - 4000.0) / 1000.0, 0.0, 1.0);
  Uv d = daylightUv(std::max(t, 4000.0));
  Uv uv{p.u + (d.u - p.u) * s, p.v + (d.v - p.v) * s};
  Uv a = planckianUv(t - 1.0), b = planckianUv(t + 1.0);
  double du = b.u - a.u, dv = b.v - a.v, len = std::hypot(du, dv);
  // u falls as T rises, so (dv, -du) has +v: positive tint is greener.
  return {uv.u + tint * dv / len, uv.v - tint * du / len};
}

Mat3 diagonal(double a, double b, double c) {
  return {a, 0, 0, 0, b, 0, 0, 0, c};
}

std::array<double, 3> catLms(Uv uv) {
  double d = 2.0 * uv.u - 8.0 * uv.v + 4.0;
  double x = 3.0 * uv.u / d, y = 2.0 * uv.v / d;
  double xyz[3] = {x / y, 1.0, (1.0 - x - y) / y};
  std::array<double, 3> lms{};
  for (int i = 0; i < 3; ++i)
    for (int k = 0; k < 3; ++k)
      lms[i] += kCat02[i * 3 + k] * xyz[k];
  return lms;
}

Vec3 componentwise(Vec3 v, auto f) { return {f(v.x), f(v.y), f(v.z)}; }

Vec3 multiply(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

void applyBands(const Grade &grade, Vec3 &lab) {
  constexpr float kDegrees = 180.0f / std::numbers::pi_v<float>;
  float hue = std::atan2(lab.z, lab.y) * kDegrees;
  float rotation = 0.0f, chromaScale = 1.0f;
  for (auto [band, centre] :
       {std::pair{&grade.greens, kGreensHue}, std::pair{&grade.blues, kBluesHue},
        std::pair{&grade.browns, kBrownsHue}}) {
    float distance = std::abs(std::remainder(hue - centre, 360.0f));
    if (distance >= kHueBandHalfWidth)
      continue;
    float weight =
        0.5f + 0.5f * std::cos(std::numbers::pi_v<float> * distance /
                               kHueBandHalfWidth);
    rotation += weight * band->hueShiftDegrees;
    chromaScale *= 1.0f + weight * (band->saturation - 1.0f);
  }
  // Scaling and rotating (a, b) moves a colour by an amount proportional to
  // its chroma, so greys stay grey.
  float c = std::cos(rotation / kDegrees) * chromaScale;
  float s = std::sin(rotation / kDegrees) * chromaScale;
  lab = {lab.x, c * lab.y - s * lab.z, s * lab.y + c * lab.z};
}

} // namespace

std::array<float, 9> whiteBalanceMatrix(float kelvin, float tint) {
  std::array<double, 3> source = catLms(whiteUv(kelvin, tint));
  std::array<double, 3> target = catLms(whiteUv(6504.0, 0.0));
  Mat3 m = multiply(
      kXyzToRec2020,
      multiply(inverse(kCat02),
               multiply(diagonal(target[0] / source[0], target[1] / source[1],
                                 target[2] / source[2]),
                        multiply(kCat02, kRec2020ToXyz))));
  std::array<float, 9> result;
  for (int i = 0; i < 9; ++i)
    result[i] = static_cast<float>(m[i]);
  return result;
}

Vec3 gradeColor(const Grade &grade, Vec3 rec2020) {
  std::array<float, 9> wb =
      whiteBalanceMatrix(grade.temperatureKelvin, grade.tint);
  Vec3 c{wb[0] * rec2020.x + wb[1] * rec2020.y + wb[2] * rec2020.z,
         wb[3] * rec2020.x + wb[4] * rec2020.y + wb[5] * rec2020.z,
         wb[6] * rec2020.x + wb[7] * rec2020.y + wb[8] * rec2020.z};
  c = c * std::exp2(grade.exposureEv);
  c = multiply(c, grade.slope) + grade.offset;
  c = {std::pow(std::max(c.x, 0.0f), grade.power.x),
       std::pow(std::max(c.y, 0.0f), grade.power.y),
       std::pow(std::max(c.z, 0.0f), grade.power.z)};
  c = componentwise(
      c, [&](float x) { return 0.18f * std::pow(x / 0.18f, grade.contrast); });
  float luminance = dot(c, {0.2627f, 0.6780f, 0.0593f});
  c = Vec3{luminance, luminance, luminance} +
      (c - Vec3{luminance, luminance, luminance}) * grade.saturation;

  Vec3 lab = apply(kOklabLmsToLab,
                   componentwise(apply(kRec2020ToOklabLms, c),
                                 [](float x) { return std::cbrt(x); }));
  applyBands(grade, lab);
  return apply(kOklabLmsToRec2020,
               componentwise(apply(kOklabLabToLms, lab),
                             [](float x) { return x * x * x; }));
}

Vec3 agxDisplay(Vec3 rec709) {
  Vec3 v = apply(kRec709ToAgxInset,
                 componentwise(rec709, [](float x) { return std::max(x, 0.0f); }));
  v = componentwise(v, [](float x) {
    float t = (std::log2(std::max(x, 1e-10f)) - kAgxMinEv) /
              (kAgxMaxEv - kAgxMinEv);
    t = std::clamp(t, 0.0f, 1.0f);
    float x2 = t * t, x4 = x2 * x2, x6 = x4 * x2;
    return -17.86f * x6 * t + 78.01f * x6 - 126.7f * x4 * t + 92.06f * x4 -
           28.72f * x2 * t + 4.361f * x2 - 0.1718f * t + 0.002857f;
  });
  v = apply(kAgxOutset, v);
  v = componentwise(v, [](float x) { return std::pow(std::max(x, 0.0f), 2.2f); });
  return apply(kRec2020ToRec709, v);
}

float shaperEncode(float x) {
  return std::clamp((std::log2(std::max(x, 1e-10f)) - kLutMinEv) /
                        (kLutMaxEv - kLutMinEv),
                    0.0f, 1.0f);
}

float shaperDecode(float t) {
  return std::exp2(kLutMinEv + t * (kLutMaxEv - kLutMinEv));
}

std::vector<std::array<float, 4>> bakeGradeLut(const Grade &grade) {
  constexpr uint32_t n = kGradeLutSize;
  std::vector<std::array<float, 4>> lut(n * n * n);
  auto decode = [](uint32_t i) {
    return shaperDecode(static_cast<float>(i) / (n - 1));
  };
  for (uint32_t k = 0; k < n; ++k)
    for (uint32_t j = 0; j < n; ++j)
      for (uint32_t i = 0; i < n; ++i) {
        Vec3 rec709 = apply(kAgxInsetToRec709, {decode(i), decode(j), decode(k)});
        Vec3 graded = gradeColor(grade, apply(kRec709ToRec2020, rec709));
        Vec3 display = agxDisplay(apply(kRec2020ToRec709, graded));
        lut[i + n * (j + n * k)] = {display.x, display.y, display.z, 1.0f};
      }
  return lut;
}

Vec3 sampleGradeLut(std::span<const std::array<float, 4>> lut, Vec3 rec709) {
  constexpr uint32_t n = kGradeLutSize;
  Vec3 v = apply(kRec709ToAgxInset,
                 componentwise(rec709, [](float x) { return std::max(x, 0.0f); }));
  uint32_t base[3];
  float frac[3];
  float coords[3] = {v.x, v.y, v.z};
  for (int c = 0; c < 3; ++c) {
    float p = shaperEncode(coords[c]) * (n - 1);
    base[c] = std::min(static_cast<uint32_t>(p), n - 2);
    frac[c] = p - static_cast<float>(base[c]);
  }
  Vec3 result{};
  for (uint32_t corner = 0; corner < 8; ++corner) {
    float weight = 1.0f;
    uint32_t index[3];
    for (int c = 0; c < 3; ++c) {
      uint32_t bit = (corner >> c) & 1;
      index[c] = base[c] + bit;
      weight *= bit ? frac[c] : 1.0f - frac[c];
    }
    const auto &texel = lut[index[0] + n * (index[1] + n * index[2])];
    result = result + Vec3{texel[0], texel[1], texel[2]} * weight;
  }
  return result;
}

namespace {

// The text after `"key":` at the top level (nested objects blanked out), or
// empty.
std::string_view valueOf(std::string_view json, std::string_view key) {
  const std::string quoted = "\"" + std::string(key) + "\"";
  size_t at = 0;
  int depth = 0;
  for (; at < json.size(); ++at) {
    if (json[at] == '{' || json[at] == '[') {
      ++depth;
    } else if (json[at] == '}' || json[at] == ']') {
      --depth;
    } else if (depth == 1 && json.compare(at, quoted.size(), quoted) == 0) {
      size_t colon = json.find_first_not_of(" \t\r\n", at + quoted.size());
      if (colon != std::string_view::npos && json[colon] == ':') {
        return json.substr(colon + 1);
      }
    }
  }
  return {};
}

std::optional<float> leadingNumber(std::string_view text, size_t *length = nullptr) {
  const size_t start = text.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) {
    return std::nullopt;
  }
  const char *first = text.data() + start;
  float value = 0.0f;
  const auto [end, error] = std::from_chars(first, text.data() + text.size(), value);
  if (error != std::errc{} || !std::isfinite(value)) {
    return std::nullopt;
  }
  if (length) {
    *length = static_cast<size_t>(end - text.data());
  }
  return value;
}

void readNumber(std::string_view json, std::string_view key, float &out) {
  if (auto value = jsonNumber(json, key)) {
    out = *value;
  }
}

void readVec3(std::string_view json, std::string_view key, Vec3 &out) {
  std::string_view text = valueOf(json, key);
  const size_t open = text.find_first_not_of(" \t\r\n");
  if (open == std::string_view::npos || text[open] != '[') {
    return;
  }
  text.remove_prefix(open + 1);
  float values[3];
  for (float &value : values) {
    size_t length = 0;
    auto number = leadingNumber(text, &length);
    if (!number) {
      return;
    }
    value = *number;
    text.remove_prefix(length);
    const size_t next = text.find_first_not_of(" \t\r\n,");
    text.remove_prefix(next == std::string_view::npos ? text.size() : next);
  }
  out = {values[0], values[1], values[2]};
}

void readBand(std::string_view json, std::string_view key, HueBand &out) {
  const std::string_view text = valueOf(json, key);
  const size_t open = text.find_first_not_of(" \t\r\n");
  if (open == std::string_view::npos || text[open] != '{') {
    return;
  }
  const std::string_view body = text.substr(open, text.find('}', open) - open + 1);
  readNumber(body, "hueShiftDegrees", out.hueShiftDegrees);
  readNumber(body, "saturation", out.saturation);
}

} // namespace

std::optional<float> jsonNumber(std::string_view json, std::string_view key) {
  return leadingNumber(valueOf(json, key));
}

std::optional<std::string> jsonString(std::string_view json, std::string_view key) {
  std::string_view text = valueOf(json, key);
  const size_t open = text.find_first_not_of(" \t\r\n");
  if (open == std::string_view::npos || text[open] != '"') {
    return std::nullopt;
  }
  text.remove_prefix(open + 1);
  const size_t close = text.find('"');
  if (close == std::string_view::npos) {
    return std::nullopt;
  }
  return std::string(text.substr(0, close));
}

namespace {

void appendNumber(std::string &out, float value) {
  char text[32];
  out.append(text, std::to_chars(text, text + sizeof(text), value).ptr);
}

void appendVec3(std::string &out, const char *key, Vec3 v) {
  out += "  \"";
  out += key;
  out += "\": [";
  appendNumber(out, v.x);
  out += ", ";
  appendNumber(out, v.y);
  out += ", ";
  appendNumber(out, v.z);
  out += "],\n";
}

void appendBand(std::string &out, const char *key, const HueBand &band, bool last) {
  out += "  \"";
  out += key;
  out += "\": {\"hueShiftDegrees\": ";
  appendNumber(out, band.hueShiftDegrees);
  out += ", \"saturation\": ";
  appendNumber(out, band.saturation);
  out += last ? "}\n" : "},\n";
}

void appendField(std::string &out, const char *key, float value) {
  out += "  \"";
  out += key;
  out += "\": ";
  appendNumber(out, value);
  out += ",\n";
}

} // namespace

std::string gradeToJson(const Grade &g) {
  std::string out = "{\n";
  appendField(out, "temperatureKelvin", g.temperatureKelvin);
  appendField(out, "tint", g.tint);
  appendField(out, "exposureEv", g.exposureEv);
  appendVec3(out, "slope", g.slope);
  appendVec3(out, "offset", g.offset);
  appendVec3(out, "power", g.power);
  appendField(out, "contrast", g.contrast);
  appendField(out, "saturation", g.saturation);
  appendBand(out, "greens", g.greens, false);
  appendBand(out, "blues", g.blues, false);
  appendBand(out, "browns", g.browns, true);
  out += "}\n";
  return out;
}

Grade gradeFromJson(std::string_view json, const Grade &base) {
  Grade g = base;
  readNumber(json, "temperatureKelvin", g.temperatureKelvin);
  readNumber(json, "tint", g.tint);
  readNumber(json, "exposureEv", g.exposureEv);
  readVec3(json, "slope", g.slope);
  readVec3(json, "offset", g.offset);
  readVec3(json, "power", g.power);
  readNumber(json, "contrast", g.contrast);
  readNumber(json, "saturation", g.saturation);
  readBand(json, "greens", g.greens);
  readBand(json, "blues", g.blues);
  readBand(json, "browns", g.browns);
  return g;
}

} // namespace meridian
