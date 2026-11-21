#include "overlay.h"

#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <sstream>

using meridian::Vec3;

namespace {

constexpr float kDegrees = std::numbers::pi_v<float> / 180.0f;
constexpr const char *kHazeNames[] = {"clean", "summer", "summer-hazy"};

} // namespace

Vec3 sunDirection(float azimuthDegrees, float elevationDegrees) {
  const float azimuth = azimuthDegrees * kDegrees;
  const float elevation = elevationDegrees * kDegrees;
  return {std::sin(azimuth) * std::cos(elevation), std::sin(elevation),
          -std::cos(azimuth) * std::cos(elevation)};
}

Look lookWithSun(Vec3 direction, Haze haze) {
  const Vec3 d = meridian::normalize(direction);
  Look look;
  look.sunAzimuthDegrees = std::atan2(d.x, -d.z) / kDegrees;
  look.sunElevationDegrees = std::asin(std::clamp(d.y, -1.0f, 1.0f)) / kDegrees;
  look.haze = haze;
  return look;
}

std::filesystem::path defaultLookPath() {
  if (const char *config = std::getenv("XDG_CONFIG_HOME"); config && *config) {
    return std::filesystem::path(config) / "meridian" / "grade.json";
  }
  const char *home = std::getenv("HOME");
  return std::filesystem::path(home ? home : ".") / ".config" / "meridian" / "grade.json";
}

std::string lookToJson(const Look &look) {
  std::string json = meridian::gradeToJson(look.grade);
  json.erase(json.rfind("\n}"));
  auto number = [](float value) {
    char text[32];
    return std::string(text, std::to_chars(text, text + sizeof(text), value).ptr);
  };
  const meridian::Effects &e = look.effects;
  const std::string tail = ",\n  \"sunAzimuthDegrees\": " + number(look.sunAzimuthDegrees) +
                           ",\n  \"sunElevationDegrees\": " + number(look.sunElevationDegrees) +
                           ",\n  \"fovDegrees\": " + number(look.fovDegrees) +
                           ",\n  \"haze\": \"" + kHazeNames[static_cast<int>(look.haze)] +
                           "\",\n  \"bloomIntensity\": " + number(look.bloom.intensity) +
                           ",\n  \"bloomScatter\": " + number(look.bloom.scatter) +
                           ",\n  \"effectParallax\": " + number(e.parallax) +
                           ",\n  \"effectParallaxDepth\": " + number(e.parallaxDepth) +
                           ",\n  \"effectParallaxSelfShadow\": " + number(e.parallaxSelfShadow) +
                           ",\n  \"effectMicroShadows\": " + number(e.microShadows) +
                           ",\n  \"effectMicroShadowStrength\": " + number(e.microShadowStrength) +
                           ",\n  \"effectContactShadows\": " + number(e.contactShadows) +
                           ",\n  \"effectContactLength\": " + number(e.contactLength) +
                           ",\n  \"effectBloom\": " + number(e.bloom) +
                           ",\n  \"effectCloudOutline\": " + number(e.cloudOutline) + "\n}\n";
  return json + tail;
}

Look lookFromJson(std::string_view json, const Look &base) {
  Look look = base;
  look.grade = meridian::gradeFromJson(json, base.grade);
  look.sunAzimuthDegrees =
      meridian::jsonNumber(json, "sunAzimuthDegrees").value_or(base.sunAzimuthDegrees);
  look.sunElevationDegrees =
      meridian::jsonNumber(json, "sunElevationDegrees").value_or(base.sunElevationDegrees);
  look.fovDegrees = meridian::jsonNumber(json, "fovDegrees").value_or(base.fovDegrees);
  look.bloom.intensity =
      meridian::jsonNumber(json, "bloomIntensity").value_or(base.bloom.intensity);
  look.bloom.scatter = meridian::jsonNumber(json, "bloomScatter").value_or(base.bloom.scatter);
  auto flag = [&](const char *key, bool fallback) {
    return meridian::jsonNumber(json, key).value_or(fallback ? 1.0f : 0.0f) != 0.0f;
  };
  meridian::Effects &e = look.effects;
  const meridian::Effects &b = base.effects;
  e.parallax = flag("effectParallax", b.parallax);
  e.parallaxDepth = meridian::jsonNumber(json, "effectParallaxDepth").value_or(b.parallaxDepth);
  e.parallaxSelfShadow = flag("effectParallaxSelfShadow", b.parallaxSelfShadow);
  e.microShadows = flag("effectMicroShadows", b.microShadows);
  e.microShadowStrength =
      meridian::jsonNumber(json, "effectMicroShadowStrength").value_or(b.microShadowStrength);
  e.contactShadows = flag("effectContactShadows", b.contactShadows);
  e.contactLength = meridian::jsonNumber(json, "effectContactLength").value_or(b.contactLength);
  e.bloom = flag("effectBloom", b.bloom);
  e.cloudOutline = meridian::jsonNumber(json, "effectCloudOutline").value_or(b.cloudOutline);
  if (auto haze = meridian::jsonString(json, "haze")) {
    for (int i = 0; i < 3; ++i) {
      if (*haze == kHazeNames[i]) {
        look.haze = static_cast<Haze>(i);
      }
    }
  }
  return look;
}

std::optional<Look> loadLook(const std::filesystem::path &path, const Look &base) {
  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }
  std::stringstream text;
  text << file.rdbuf();
  return lookFromJson(text.str(), base);
}

GradePanel::GradePanel(const Look &builtIn, const Look &initial,
                       std::filesystem::path path)
    : builtIn_(builtIn), look_(initial), path_(std::move(path)) {}

void GradePanel::setVisible(meridian::Window &window, bool visible) {
  visible_ = visible;
  window.setCursorCaptured(!visible);
}

void GradePanel::handleToggle(meridian::Window &window) {
  const bool gDown = window.isKeyDown(meridian::Key::G);
  if (gDown && !gWasDown_ && !ImGui::GetIO().WantTextInput) {
    setVisible(window, !visible_);
  }
  gWasDown_ = gDown;
}

GradePanel::Changes GradePanel::draw() {
  Changes changes;
  auto slider = [&](const char *label, float &value, float min, float max, bool &flag) {
    flag |= ImGui::SliderFloat(label, &value, min, max, "%.3f");
  };
  auto slider3 = [&](const char *label, Vec3 &value, float min, float max) {
    float v[3] = {value.x, value.y, value.z};
    if (ImGui::SliderFloat3(label, v, min, max, "%.3f")) {
      value = {v[0], v[1], v[2]};
      changes.grade = true;
    }
  };
  auto band = [&](const char *name, meridian::HueBand &band) {
    ImGui::PushID(name);
    ImGui::Text("%s", name);
    slider("hue shift (deg)", band.hueShiftDegrees, -45.0f, 45.0f, changes.grade);
    slider("saturation", band.saturation, 0.0f, 2.0f, changes.grade);
    ImGui::PopID();
  };

  ImGui::SetNextWindowPos({16, 16}, ImGuiCond_FirstUseEver);
  ImGui::Begin("Grade", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
  meridian::Grade &g = look_.grade;
  ImGui::SeparatorText("White balance and exposure");
  slider("temperature (K)", g.temperatureKelvin, 2000.0f, 12000.0f, changes.grade);
  slider("tint", g.tint, -0.05f, 0.05f, changes.grade);
  slider("exposure compensation (EV)", g.exposureEv, -4.0f, 4.0f, changes.grade);
  ImGui::SeparatorText("Slope, offset, power (RGB)");
  slider3("slope", g.slope, 0.0f, 2.0f);
  slider3("offset", g.offset, -0.1f, 0.1f);
  slider3("power", g.power, 0.2f, 3.0f);
  ImGui::SeparatorText("Contrast and saturation");
  slider("contrast", g.contrast, 0.5f, 2.5f, changes.grade);
  slider("saturation", g.saturation, 0.0f, 2.5f, changes.grade);
  ImGui::SeparatorText("Hue bands");
  band("greens", g.greens);
  band("blues", g.blues);
  band("browns", g.browns);

  ImGui::SeparatorText("Sun and haze");
  slider("sun azimuth (deg from north)", look_.sunAzimuthDegrees, -180.0f, 180.0f,
         changes.lighting);
  slider("sun elevation (deg)", look_.sunElevationDegrees, -10.0f, 90.0f, changes.lighting);
  int haze = static_cast<int>(look_.haze);
  if (ImGui::Combo("haze", &haze, "clean\0summer\0summer-hazy\0")) {
    look_.haze = static_cast<Haze>(haze);
    changes.lighting = true;
  }

  ImGui::SeparatorText("Field of view");
  bool fovChanged = false; // read every frame by main.cpp
  slider("field of view (deg, horizontal)", look_.fovDegrees, 50.0f, 110.0f, fovChanged);

  ImGui::SeparatorText("Bloom");
  slider("bloom intensity", look_.bloom.intensity, 0.0f, 0.1f, changes.bloom);
  slider("bloom scatter", look_.bloom.scatter, 0.0f, 1.0f, changes.bloom);
  changes.effects |= ImGui::Checkbox("bloom on", &look_.effects.bloom);

  ImGui::SeparatorText("Effects");
  meridian::Effects &fx = look_.effects;
  auto effect = [&](const char *name, bool &on, float *value, float max, const char *label) {
    ImGui::PushID(name);
    changes.effects |= ImGui::Checkbox(name, &on);
    if (value) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(160.0f);
      slider(label, *value, 0.0f, max, changes.effects);
    }
    ImGui::PopID();
  };
  effect("parallax", fx.parallax, &fx.parallaxDepth, 4.0f, "depth x");
  effect("parallax self-shadow", fx.parallaxSelfShadow, nullptr, 0.0f, "");
  effect("micro shadows", fx.microShadows, &fx.microShadowStrength, 1.0f, "strength");
  effect("contact shadows", fx.contactShadows, &fx.contactLength, 3.0f, "length x");
  slider("cloud outline", fx.cloudOutline, 0.0f, 3.0f, changes.effects);

  ImGui::Separator();
  if (ImGui::Checkbox("show built-in grade", &showBuiltIn_)) {
    changes.grade = changes.effects = true;
  }
  if (ImGui::Button("Save")) {
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    std::ofstream file(path_);
    file << lookToJson(look_);
    status_ = file ? "saved" : "save failed";
  }
  ImGui::SameLine();
  const bool load = ImGui::Button("Load");
  ImGui::SameLine();
  const bool reset = ImGui::Button("Reset");
  if (load || reset) {
    std::optional<Look> next = reset ? builtIn_ : loadLook(path_, look_);
    if (next) {
      look_ = *next;
      if (reset) {
        showBuiltIn_ = false;
      }
      changes.grade = changes.lighting = changes.bloom = changes.effects = true;
    }
    status_ = !next ? "load failed" : reset ? "reset" : "loaded";
  }
  ImGui::TextDisabled("%s", path_.c_str());
  if (!status_.empty()) {
    ImGui::SameLine();
    ImGui::Text("(%s)", status_.c_str());
  }
  ImGui::End();
  return changes;
}
