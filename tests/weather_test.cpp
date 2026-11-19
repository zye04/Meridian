#include <doctest/doctest.h>
#include <meridian/weather.h>

#include <cmath>
#include <stdexcept>
#include <utility>

using meridian::Vec3;
using meridian::Weather;
using meridian::WeatherState;

namespace {

WeatherState steady(float coverage, Vec3 direction, float speed) {
  WeatherState state;
  state.coverage = coverage;
  state.wind.direction = direction;
  state.wind.speed10m = speed;
  return state;
}

bool finite(const WeatherState &s) {
  return std::isfinite(s.coverage) && std::isfinite(s.type) &&
         std::isfinite(s.precipitation) && std::isfinite(s.wind.direction.x) &&
         std::isfinite(s.wind.direction.z) && std::isfinite(s.wind.speed10m) &&
         std::isfinite(s.wind.gustiness);
}

} // namespace

TEST_CASE("weather presets") {
  CHECK(meridian::weatherPreset("clear").coverage == 0.0f);
  CHECK(meridian::weatherPreset("overcast").type == 0.0f);
  for (const char *name : {"clear", "fair", "broken", "overcast"}) {
    const WeatherState s = meridian::weatherPreset(name);
    CHECK(meridian::length(s.wind.direction) == doctest::Approx(1.0f));
    CHECK(s.wind.direction.y == 0.0f);
  }
  CHECK_THROWS_AS(meridian::weatherPreset("stormy"), std::invalid_argument);
}

TEST_CASE("wind profile: power law from 10 m, veering clockwise with altitude") {
  meridian::WindState wind;
  wind.direction = {1.0f, 0.0f, 0.0f};
  wind.speed10m = 5.0f;
  const Vec3 at10 = meridian::windVelocityAt(wind, 10.0f);
  // WindState is the 10 m wind: no scale, no veer there.
  CHECK(at10 == Vec3{5.0f, 0.0f, 0.0f});
  const Vec3 high = meridian::windVelocityAt(wind, 2500.0f);
  const float speed = 5.0f * std::pow(250.0f, meridian::kWindShearExponent);
  CHECK(meridian::length(high) == doctest::Approx(speed));
  // Clockwise seen from above (north = -z): east turns toward south (+z).
  CHECK(high.z / high.x == doctest::Approx(std::tan(meridian::kWindVeer)));
  CHECK(meridian::length(meridian::windVelocityAt(wind, -50.0f)) > 0.0f);
}

TEST_CASE("state blend is C1-continuous at both ends") {
  const WeatherState fair = meridian::weatherPreset("fair");
  const WeatherState overcast = meridian::weatherPreset("overcast");
  Weather weather(1, fair);
  constexpr double kStart = 10.0;
  constexpr double kSeconds = 100.0;
  constexpr double kH = 0.01;
  weather.update(kStart - kH);
  const WeatherState before = weather.current();
  weather.update(kStart);
  const WeatherState atStart = weather.current();
  weather.setTarget(overcast, static_cast<float>(kSeconds));
  weather.update(kStart + kH);
  const WeatherState afterStart = weather.current();
  weather.update(kStart + kSeconds - kH);
  const WeatherState beforeEnd = weather.current();
  weather.update(kStart + kSeconds);
  const WeatherState atEnd = weather.current();
  weather.update(kStart + kSeconds + kH);
  const WeatherState afterEnd = weather.current();

  auto checkC1 = [&](auto field) {
    CHECK(std::abs(field(afterStart) - field(atStart)) < 1e-4);
    CHECK(std::abs((field(afterStart) - field(atStart)) / kH -
                   (field(atStart) - field(before)) / kH) < 1e-4);
    CHECK(std::abs(field(atEnd) - field(beforeEnd)) < 1e-4);
    CHECK(std::abs((field(afterEnd) - field(atEnd)) / kH -
                   (field(atEnd) - field(beforeEnd)) / kH) < 1e-4);
  };
  checkC1([](const WeatherState &s) { return s.coverage; });
  checkC1([](const WeatherState &s) { return s.type; });
  checkC1([](const WeatherState &s) { return s.wind.speed10m; });
  CHECK(atStart.coverage == doctest::Approx(fair.coverage));
  CHECK(atEnd.coverage == doctest::Approx(overcast.coverage));
  CHECK(atEnd.wind.speed10m == doctest::Approx(overcast.wind.speed10m));
}

TEST_CASE("weather is independent of the update rate") {
  // Game seconds per update: 1, 4 and 60 updates per game second, and a
  // 600x time-lapse at 60 fps (10 game seconds per update).
  struct Rate {
    double num, den;
  };
  auto run = [](Rate rate) {
    Weather weather(7, meridian::weatherPreset("fair"));
    WeatherState turned = meridian::weatherPreset("overcast");
    turned.wind.direction = meridian::normalize({0.3f, 0.0f, 1.0f});
    turned.wind.speed10m = 12.0f;
    bool first = false;
    bool second = false;
    for (long k = 1;; ++k) {
      const double t = static_cast<double>(k) * rate.num / rate.den;
      weather.update(t);
      if (!first && t >= 600.0) {
        weather.setTarget(turned, 900.0f);
        first = true;
      }
      if (!second && t >= 2000.0) {
        weather.setTarget(meridian::weatherPreset("broken"), 300.0f);
        second = true;
      }
      if (t >= 3600.0) {
        break;
      }
    }
    return weather;
  };
  const Weather reference = run({1.0, 1.0});
  for (Rate rate : {Rate{1.0, 4.0}, Rate{1.0, 60.0}, Rate{10.0, 1.0}}) {
    const Weather other = run(rate);
    const WeatherState &a = reference.current();
    const WeatherState &b = other.current();
    CHECK(std::abs(a.coverage - b.coverage) < 1e-5);
    CHECK(std::abs(a.type - b.type) < 1e-5);
    CHECK(std::abs(a.wind.speed10m - b.wind.speed10m) < 1e-5);
    CHECK(std::abs(a.wind.direction.x - b.wind.direction.x) < 1e-5);
    CHECK(std::abs(a.wind.direction.z - b.wind.direction.z) < 1e-5);
    CHECK(std::abs(reference.travel() - other.travel()) < 1e-5);
    CHECK(reference.windOffset(10.0f) == other.windOffset(10.0f));
    CHECK(reference.history().size() == other.history().size());
  }
  CHECK(reference.travel() > 20000.0);
}

TEST_CASE("wind offset is the integral of the velocity across a speed change") {
  Weather weather(1, steady(0.35f, {1.0f, 0.0f, 0.0f}, 2.0f));
  weather.update(100.0);
  CHECK(weather.windOffset(10.0f).x == doctest::Approx(200.0f));
  weather.setTarget(steady(0.35f, {1.0f, 0.0f, 0.0f}, 12.0f), 50.0f);
  // Frames of 1/60 s straddle the substeps: each frame moves by speed * dt,
  // never more (no jump).
  float previous = weather.windOffset(10.0f).x;
  for (int frame = 1; frame <= 6000; ++frame) {
    weather.update(100.0 + frame / 60.0);
    const float offset = weather.windOffset(10.0f).x;
    CHECK((offset - previous) * 60.0f >= 2.0f - 1e-2f);
    CHECK((offset - previous) * 60.0f <= 12.0f + 1e-2f);
    previous = offset;
  }
  // 2 m/s for 100 s, the smoothstep blend averages 7 m/s over 50 s, then
  // 12 m/s for 50 s.
  CHECK(weather.windOffset(10.0f).x == doctest::Approx(200.0f + 350.0f + 600.0f).epsilon(1e-5));
  CHECK(weather.windOffset(10.0f).z == 0.0f);
  CHECK(weather.travel() == doctest::Approx(1150.0).epsilon(1e-5));
  // Aloft the same integral, scaled and veered like the velocity.
  meridian::WindState unit;
  unit.speed10m = 1.0f;
  const Vec3 aloft = weather.windOffset(1500.0f);
  const Vec3 expected = meridian::windVelocityAt(unit, 1500.0f) * 1150.0f;
  CHECK(aloft.x == doctest::Approx(expected.x).epsilon(1e-5));
  CHECK(aloft.z == doctest::Approx(expected.z).epsilon(1e-5));
}

TEST_CASE("the wind offset modulo a period stays precise after long travel") {
  const Vec3 direction = meridian::normalize({-0.6f, 0.0f, 0.8f});
  Weather weather(1, steady(0.35f, direction, 10.0f));
  // ~2300 km of air travel at 3 km: a float offset there resolves 25 cm.
  weather.update(1.0e5);
  const double period = 262144.0;
  const Vec3 wrapped = weather.windOffsetModulo(3000.0f, period);
  const Vec3 offset = weather.windOffset(3000.0f);
  for (auto [w, o] : {std::pair{wrapped.x, offset.x}, std::pair{wrapped.z, offset.z}}) {
    CHECK(w >= 0.0f);
    CHECK(w < period);
    CHECK(std::abs(std::remainder(static_cast<double>(o) - w, period)) < 0.2);
  }
  // One 1/60 s frame later the wrapped offset has moved by velocity * dt,
  // within the float resolution below the period (1.6 cm), whatever the
  // total travel.
  weather.update(1.0e5 + 1.0 / 60.0);
  const Vec3 next = weather.windOffsetModulo(3000.0f, period);
  meridian::WindState wind;
  wind.direction = direction;
  wind.speed10m = 10.0f;
  const Vec3 step = meridian::windVelocityAt(wind, 3000.0f) * (1.0f / 60.0f);
  CHECK(std::abs(std::remainder(static_cast<double>(next.x) - wrapped.x, period) - step.x) < 0.02);
  CHECK(std::abs(std::remainder(static_cast<double>(next.z) - wrapped.z, period) - step.z) < 0.02);
}

TEST_CASE("a change reaches a point downwind of the front line at distance / speed") {
  const Vec3 east{1.0f, 0.0f, 0.0f};
  for (float downwind : {80000.0f, 150000.0f}) {
    Weather weather(1, steady(0.35f, east, 10.0f));
    constexpr double kChange = 1000.0;
    weather.update(kChange);
    weather.setTarget(steady(0.95f, east, 10.0f), 0.0f);
    const Vec3 point = east * (downwind - meridian::kFrontLineUpwind);
    const double arrival = kChange + downwind / 10.0;
    const double interval = meridian::kWeatherHistoryInterval / 10.0;
    weather.update(arrival - interval);
    CHECK(weather.stateAt(point).coverage == doctest::Approx(0.35f));
    weather.update(arrival + interval);
    CHECK(weather.stateAt(point).coverage == doctest::Approx(0.95f));
    // Upwind of the line: the current state.
    CHECK(weather.stateAt(east * -70000.0f).coverage == doctest::Approx(0.95f));
  }
}

TEST_CASE("calm wind and a reversing wind keep the weather defined") {
  const Vec3 east{1.0f, 0.0f, 0.0f};
  Weather weather(1, steady(0.35f, east, 10.0f));
  weather.update(3000.0);
  // The wind drops instantly while the state turns overcast: nothing more
  // arrives, every point keeps what had arrived.
  weather.setTarget(steady(0.95f, east, 0.0f), 0.0f);
  const double travel = weather.travel();
  weather.update(3600.0);
  CHECK(weather.travel() == travel);
  for (float x : {-59999.0f, -59000.0f, -40000.0f, 0.0f, 100000.0f}) {
    const WeatherState s = weather.stateAt({x, 0.0f, 0.0f});
    CHECK(finite(s));
    CHECK(s.coverage == doctest::Approx(0.35f));
  }
  CHECK(finite(weather.stateAt({-60000.5f, 0.0f, 0.0f})));
  CHECK(std::isfinite(weather.windOffset(2000.0f).x));

  // The wind returns from the opposite side over a minute.
  weather.setTarget(steady(0.95f, east * -1.0f, 10.0f), 60.0f);
  for (int second = 1; second <= 120; ++second) {
    weather.update(3600.0 + second);
    const Vec3 d = weather.current().wind.direction;
    CHECK(meridian::length(d) == doctest::Approx(1.0f));
    for (float x : {-80000.0f, -60000.0f, 0.0f, 60000.0f}) {
      const WeatherState s = weather.stateAt({x, 0.0f, 2000.0f});
      CHECK(finite(s));
      CHECK(s.coverage >= 0.35f - 1e-5f);
      CHECK(s.coverage <= 0.95f + 1e-5f);
    }
  }
}

TEST_CASE("history lookup clamps at both ends") {
  const Vec3 east{1.0f, 0.0f, 0.0f};
  Weather weather(1, steady(0.0f, east, 10.0f));
  weather.update(1000.0);
  weather.setTarget(steady(0.95f, east, 10.0f), 0.0f);
  weather.update(30000.0);
  weather.setTarget(steady(0.65f, east, 10.0f), 0.0f);
  weather.update(31000.0);
  const auto history = weather.history();
  REQUIRE(history.size() == meridian::kWeatherHistoryCapacity);
  CHECK(history.front().coverage == doctest::Approx(0.95f));
  CHECK(history.back().travel <= weather.travel());
  CHECK(history.front().travel <
        weather.travel() - 250.0 * meridian::kWeatherHistoryInterval);
  // Upwind of the front line: current.
  CHECK(weather.stateAt(east * -70000.0f).coverage == doctest::Approx(0.65f));
  // Farther downwind than the history reaches: the oldest sample.
  CHECK(weather.stateAt(east * 400000.0f).coverage ==
        doctest::Approx(history.front().coverage));
}
