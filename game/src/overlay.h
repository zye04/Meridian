#pragma once

#include <meridian/grade.h>
#include <meridian/math/vec3.h>
#include <meridian/renderer.h>
#include <meridian/window.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

enum class Haze { Clean, Summer, SummerHazy };

// Everything the grading panel edits: the colour grade, the sun (azimuth
// clockwise from north, which is -z; elevation above the horizon), the
// aerosol and the bloom.
struct Look {
  meridian::Grade grade;
  float sunAzimuthDegrees = 0.0f;
  float sunElevationDegrees = 0.0f;
  Haze haze = Haze::Summer;
  float fovDegrees = 70.0f; // horizontal
  meridian::Bloom bloom;
  meridian::Effects effects;
};

meridian::Vec3 sunDirection(float azimuthDegrees, float elevationDegrees);
// A Look with this sun direction and the default grade.
Look lookWithSun(meridian::Vec3 direction, Haze haze);

// $XDG_CONFIG_HOME/meridian/grade.json, else ~/.config/meridian/grade.json.
std::filesystem::path defaultLookPath();
// The grade file's text for a look; keys missing from `json` keep `base`'s.
std::string lookToJson(const Look &look);
Look lookFromJson(std::string_view json, const Look &base);
std::optional<Look> loadLook(const std::filesystem::path &path, const Look &base);

// G opens the panel: every grade field, the sun, the haze and the bloom, with Save,
// Load, Reset and an A/B toggle against the built-in grade. While open the
// cursor is free and the game should not move the camera.
class GradePanel {
public:
  // builtIn is what Reset restores (and what A/B shows); initial is the look
  // at startup, saved to and loaded from path.
  GradePanel(const Look &builtIn, const Look &initial, std::filesystem::path path);

  bool visible() const { return visible_; }
  void setVisible(meridian::Window &window, bool visible);
  // G toggles, unless a text field is being edited. Once per frame.
  void handleToggle(meridian::Window &window);

  struct Changes {
    bool grade = false;    // the grade() to render changed
    bool lighting = false; // the sun or the haze changed
    bool bloom = false;    // look().bloom changed
    bool effects = false;  // effects() changed
  };
  // Issues the widgets (after Renderer::beginOverlay).
  Changes draw();

  const Look &look() const { return look_; }
  // The effects to render: the edited ones, or the defaults with A/B on.
  const meridian::Effects &effects() const {
    return showBuiltIn_ ? builtIn_.effects : look_.effects;
  }
  // The grade to render: the edited one, or the built-in with A/B on.
  const meridian::Grade &grade() const {
    return showBuiltIn_ ? builtIn_.grade : look_.grade;
  }

private:
  Look builtIn_, look_;
  std::filesystem::path path_;
  bool visible_ = false;
  bool gWasDown_ = false;
  bool showBuiltIn_ = false;
  std::string status_;
};
