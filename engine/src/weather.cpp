#include <meridian/weather.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

namespace meridian {
namespace {

float lerp(float a, float b, float t) { return a + (b - a) * t; }

float profileScale(float altitude) {
  return std::pow(std::max(altitude, 1.0f) / 10.0f, kWindShearExponent);
}

// Relative to the 10 m wind, which WindState describes.
float veerAngle(float altitude) {
  return kWindVeer * std::clamp((altitude - 10.0f) / (kWindVeerAltitude - 10.0f), 0.0f, 1.0f);
}

// The 10 m offset carried to `altitude`: scaled and veered like the
// velocity (both are time-invariant), in double.
std::pair<double, double> offsetAloft(double x, double z, float altitude) {
  const double angle = veerAngle(altitude);
  const double scale = profileScale(altitude);
  const double c = std::cos(angle);
  const double s = std::sin(angle);
  return {(x * c - z * s) * scale, (x * s + z * c) * scale};
}

} // namespace

WeatherState weatherPreset(std::string_view name) {
  WeatherState state;
  if (name == "clear") {
    state.coverage = 0.0f;
    state.wind.gustiness = 0.2f;
  } else if (name == "fair") {
    // The defaults: fair-weather cumulus.
  } else if (name == "broken") {
    state.coverage = 0.65f;
    state.type = 0.6f;
    state.wind.speed10m = 6.0f;
    state.wind.gustiness = 0.45f;
  } else if (name == "overcast") {
    state.coverage = 1.0f;
    state.type = 0.0f;
    state.wind.speed10m = 8.0f;
    state.wind.gustiness = 0.5f;
  } else {
    throw std::invalid_argument("unknown weather preset " + std::string(name) +
                                " (use clear, fair, broken or overcast)");
  }
  return state;
}

Vec3 windVelocityAt(const WindState &wind, float altitude) {
  const float angle = veerAngle(altitude);
  const float speed = wind.speed10m * profileScale(altitude);
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  return {(wind.direction.x * c - wind.direction.z * s) * speed, 0.0f,
          (wind.direction.x * s + wind.direction.z * c) * speed};
}

Weather::Weather(uint32_t seed, const WeatherState &start)
    : seed_(seed), from_(start), target_(start) {
  current_ = stateAtTime(0.0);
  history_.reserve(kWeatherHistoryCapacity);
  history_.push_back({0.0f, current_.coverage, current_.type, current_.precipitation});
}

void Weather::setTarget(const WeatherState &target, float seconds) {
  from_ = current_;
  target_ = target;
  blendStart_ = now_;
  blendSeconds_ = std::max(seconds, 0.0f);
}

WeatherState Weather::stateAtTime(double t) const {
  const double u = blendSeconds_ > 0.0
                       ? std::clamp((t - blendStart_) / blendSeconds_, 0.0, 1.0)
                       : (t >= blendStart_ ? 1.0 : 0.0);
  // Smoothstep: zero slope at both ends, so the blend is C1 with the steady
  // states around it.
  const float s = static_cast<float>(u * u * (3.0 - 2.0 * u));
  WeatherState state;
  state.coverage = lerp(from_.coverage, target_.coverage, s);
  state.type = lerp(from_.type, target_.type, s);
  state.precipitation = lerp(from_.precipitation, target_.precipitation, s);
  state.wind.speed10m = lerp(from_.wind.speed10m, target_.wind.speed10m, s);
  state.wind.gustiness = lerp(from_.wind.gustiness, target_.wind.gustiness, s);
  // Turn along the shorter arc; a lerp of the vectors would pass through
  // zero on a reversal.
  const float a0 = std::atan2(from_.wind.direction.z, from_.wind.direction.x);
  const float a1 = std::atan2(target_.wind.direction.z, target_.wind.direction.x);
  const float delta = std::remainder(a1 - a0, 2.0f * std::numbers::pi_v<float>);
  const float angle = a0 + delta * s;
  state.wind.direction = {std::cos(angle), 0.0f, std::sin(angle)};
  return state;
}

void Weather::update(double gameSeconds) {
  if (gameSeconds < now_) {
    return;
  }
  while (substepTime_ + kWeatherSubstep <= gameSeconds) {
    // Midpoint rule: exact for linear changes, and the same value the
    // extrapolation below uses, so offsets stay continuous.
    const WindState wind = stateAtTime(substepTime_ + 0.5 * kWeatherSubstep).wind;
    const double step = wind.speed10m * kWeatherSubstep;
    substepOffsetX_ += wind.direction.x * step;
    substepOffsetZ_ += wind.direction.z * step;
    substepTravel_ += step;
    substepTime_ += kWeatherSubstep;
    if (substepTravel_ >= nextSampleTravel_) {
      if (history_.size() == kWeatherHistoryCapacity) {
        history_.erase(history_.begin());
      }
      const WeatherState state = stateAtTime(substepTime_);
      history_.push_back({static_cast<float>(substepTravel_), state.coverage, state.type,
                          state.precipitation});
      nextSampleTravel_ =
          (std::floor(substepTravel_ / kWeatherHistoryInterval) + 1.0) * kWeatherHistoryInterval;
    }
  }
  now_ = gameSeconds;
  current_ = stateAtTime(now_);
  const double partial = now_ - substepTime_;
  const WindState wind = stateAtTime(substepTime_ + 0.5 * partial).wind;
  offsetX_ = substepOffsetX_ + wind.direction.x * wind.speed10m * partial;
  offsetZ_ = substepOffsetZ_ + wind.direction.z * wind.speed10m * partial;
  travel_ = substepTravel_ + wind.speed10m * partial;
}

const WeatherState &Weather::current() const { return current_; }

WeatherState Weather::stateAt(Vec3 worldPosition) const {
  const Vec3 direction = current_.wind.direction;
  const double downwind = static_cast<double>(worldPosition.x) * direction.x +
                          static_cast<double>(worldPosition.z) * direction.z +
                          kFrontLineUpwind;
  WeatherState state = current_;
  if (downwind <= 0.0) {
    return state;
  }
  const double travel = travel_ - downwind;
  const WeatherHistorySample newest{static_cast<float>(travel_), current_.coverage,
                                    current_.type, current_.precipitation};
  const WeatherHistorySample *older = &history_.front();
  const WeatherHistorySample *newer = &newest;
  if (travel <= older->travel) {
    newer = older;
  } else if (travel < history_.back().travel) {
    const auto next = std::upper_bound(
        history_.begin(), history_.end(), travel,
        [](double t, const WeatherHistorySample &sample) { return t < sample.travel; });
    newer = &*next;
    older = &*(next - 1);
  } else {
    older = &history_.back();
  }
  // Clamped at the ends (no span), and float rounding can leave the newest
  // sample at the current travel.
  const double span = static_cast<double>(newer->travel) - older->travel;
  const float t =
      span > 0.0 ? static_cast<float>(std::clamp((travel - older->travel) / span, 0.0, 1.0))
                 : 0.0f;
  state.coverage = lerp(older->coverage, newer->coverage, t);
  state.type = lerp(older->type, newer->type, t);
  state.precipitation = lerp(older->precipitation, newer->precipitation, t);
  return state;
}

Vec3 Weather::windOffset(float altitude) const {
  const auto [x, z] = offsetAloft(offsetX_, offsetZ_, altitude);
  return {static_cast<float>(x), 0.0f, static_cast<float>(z)};
}

Vec3 Weather::windOffsetModulo(float altitude, double period) const {
  const auto [x, z] = offsetAloft(offsetX_, offsetZ_, altitude);
  auto wrap = [&](double v) {
    const double r = std::fmod(v, period);
    return static_cast<float>(r < 0.0 ? r + period : r);
  };
  return {wrap(x), 0.0f, wrap(z)};
}

double Weather::travel() const { return travel_; }

double Weather::time() const { return now_; }

uint32_t Weather::seed() const { return seed_; }

std::span<const WeatherHistorySample> Weather::history() const { return history_; }

} // namespace meridian
