#pragma once

#include <meridian/math/vec3.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace meridian {

// The direction the wind blows toward (unit, y = 0); speed at 10 m above the
// ground; gustiness [0, 1].
struct WindState {
  Vec3 direction{1.0f, 0.0f, 0.0f};
  float speed10m = 3.0f;
  float gustiness = 0.3f;
};

// coverage [0, 1]; type 0 = stratus/overcast .. 1 = cumulus (above 1 is
// reserved for cumulonimbus); precipitation [0, 1] (carried, unused so far).
struct WeatherState {
  float coverage = 0.35f;
  float type = 1.0f;
  float precipitation = 0.0f;
  WindState wind;
};

// clear, fair, broken, overcast; throws on any other name.
WeatherState weatherPreset(std::string_view name);

// A weather change is born on this line (perpendicular to the wind, this far
// upwind of the world origin, m) and travels downwind with the air.
constexpr float kFrontLineUpwind = 60000.0f;
// Fixed integration step (game seconds), so results don't depend on the
// frame rate.
constexpr double kWeatherSubstep = 0.25;
// One history sample per this much air travel (m); the capacity covers
// ~255 km, the farthest upwind the clouds can show.
constexpr float kWeatherHistoryInterval = 1000.0f;
constexpr uint32_t kWeatherHistoryCapacity = 256;
// Wind profile: power-law speed (neutral stability) and a clockwise veer
// (Ekman spiral, northern hemisphere) reaching its full angle at the top of
// the boundary layer.
constexpr float kWindShearExponent = 1.0f / 7.0f;
constexpr float kWindVeer = 0.35f;
constexpr float kWindVeerAltitude = 1000.0f;

struct WeatherHistorySample {
  float travel;
  float coverage, type, precipitation;
};

// Wind velocity (m/s, xz, y = 0) at `altitude` meters above the ground.
Vec3 windVelocityAt(const WindState &wind, float altitude);

// The cloud shadow map's orthographic frame: forward is the light's travel
// (-sunDirection), right and up span the map, right x up = sunDirection.
// Continuous in the sun direction everywhere except the nadir (Frisvad's
// basis with its one singular direction there), so it doesn't flip or spin
// as the sun passes overhead or along the horizon.
struct SunBasis {
  Vec3 right, up, forward;
};
SunBasis cloudShadowBasis(Vec3 sunDirection);

// One Beer shadow map texel (cloud_shadow.comp mirrors this): depth along
// the light (m) where the cloud starts, its mean extinction (1/m) up to
// where it ends, and its total optical depth.
struct BeerShadowTexel {
  float front = 0.0f;
  float meanExtinction = 0.0f;
  float maxOpticalDepth = 0.0f;
};
// From extinction samples (1/m) at the centers of steps of `step` m along the
// light, the first starting at depth `start`.
BeerShadowTexel beerShadowTexel(std::span<const float> extinction, float start, float step);
// exp(-min(maxOpticalDepth, meanExtinction * max(0, depth - front))).
float beerShadowTransmittance(const BeerShadowTexel &texel, float depth);

// The global weather: a current state blending toward a target, the
// integrated wind, and a history of past states that fronts are read from.
class Weather {
public:
  explicit Weather(uint32_t seed, const WeatherState &start);

  // Blends from the current state to `target` over `seconds` of game time
  // (smoothstep: value and slope continuous), starting now.
  void setTarget(const WeatherState &target, float seconds);
  // Advances to the absolute game time `gameSeconds` (never backwards).
  void update(double gameSeconds);

  const WeatherState &current() const;
  // The state the air at `worldPosition` carries: what was current when that
  // air crossed the front line. The wind is the global current one.
  WeatherState stateAt(Vec3 worldPosition) const;
  // Integral of windVelocityAt over game time (xz, y = 0).
  Vec3 windOffset(float altitude) const;
  // windOffset modulo `period` (x and z in [0, period)), reduced in double
  // precision: for scrolling noise that tiles with that period, which stays
  // precise however far the air has moved.
  Vec3 windOffsetModulo(float altitude, double period) const;
  // Integral of the 10 m wind speed over game time (m).
  double travel() const;
  double time() const;
  uint32_t seed() const;
  // Oldest first.
  std::span<const WeatherHistorySample> history() const;

private:
  WeatherState stateAtTime(double t) const;

  uint32_t seed_;
  WeatherState from_;
  WeatherState target_;
  double blendStart_ = 0.0;
  double blendSeconds_ = 0.0;
  // Integrated up to substepTime_ (a multiple of kWeatherSubstep), then
  // extrapolated to now_.
  double substepTime_ = 0.0;
  double substepOffsetX_ = 0.0;
  double substepOffsetZ_ = 0.0;
  double substepTravel_ = 0.0;
  double nextSampleTravel_ = kWeatherHistoryInterval;
  double now_ = 0.0;
  WeatherState current_;
  double offsetX_ = 0.0;
  double offsetZ_ = 0.0;
  double travel_ = 0.0;
  std::vector<WeatherHistorySample> history_;
};

} // namespace meridian
