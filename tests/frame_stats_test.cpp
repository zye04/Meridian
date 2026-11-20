#include <doctest/doctest.h>
#include <meridian/frame_stats.h>

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

using meridian::FrameStats;
using meridian::summarizeFrameTimes;

TEST_CASE("frame stats: 1000 frames with 10 spikes") {
  std::vector<float> ms(1000, 10.0f);
  for (int i = 0; i < 10; ++i) {
    ms[i * 100] = 50.0f;
  }
  const FrameStats stats = summarizeFrameTimes(ms);
  CHECK(stats.averageFps == doctest::Approx(1000.0 / 10.4)); // mean frame 10.4 ms
  CHECK(stats.low0_1Fps == doctest::Approx(20.0));           // the 1 slowest: 50 ms
  CHECK(stats.low1Fps == doctest::Approx(20.0));             // the 10 slowest: all 50 ms
  CHECK(stats.low5Fps == doctest::Approx(1000.0 / 18.0));    // 10 x 50 + 40 x 10 ms, / 50
  CHECK(stats.maxFps == doctest::Approx(100.0));
}

TEST_CASE("frame stats: fewer frames than 1/x use the slowest frame") {
  const std::vector<float> ms = {20.0f, 10.0f, 40.0f};
  const FrameStats stats = summarizeFrameTimes(ms);
  CHECK(stats.averageFps == doctest::Approx(3000.0 / 70.0));
  CHECK(stats.low0_1Fps == doctest::Approx(25.0));
  CHECK(stats.low1Fps == doctest::Approx(25.0));
  CHECK(stats.low5Fps == doctest::Approx(25.0));
  CHECK(stats.maxFps == doctest::Approx(100.0));
}

TEST_CASE("frame stats: empty is zero") {
  const FrameStats stats = summarizeFrameTimes({});
  CHECK(stats.averageFps == 0.0f);
  CHECK(stats.low0_1Fps == 0.0f);
  CHECK(stats.low1Fps == 0.0f);
  CHECK(stats.low5Fps == 0.0f);
  CHECK(stats.maxFps == 0.0f);
}

TEST_CASE("frame stats: non-round n, unsorted input") {
  std::vector<float> ms(1001, 10.0f);
  ms[0] = 50.0f;
  ms[1] = 30.0f;
  std::shuffle(ms.begin(), ms.end(), std::mt19937(1));
  const FrameStats stats = summarizeFrameTimes(ms);
  // ceil(0.1% * 1001) = 2 slowest: (50 + 30) / 2 = 40 ms
  CHECK(stats.low0_1Fps == doctest::Approx(25.0));
  // ceil(1%) = 11 slowest: 50 + 30 + 9 x 10 = 170 ms / 11
  CHECK(stats.low1Fps == doctest::Approx(1000.0 * 11.0 / 170.0));
  CHECK(stats.averageFps == doctest::Approx(1000.0 * 1001.0 / 10070.0));
  CHECK(stats.maxFps == doctest::Approx(100.0));
}

TEST_CASE("frame stats: a single frame") {
  const std::vector<float> ms = {16.0f};
  const FrameStats stats = summarizeFrameTimes(ms);
  CHECK(stats.averageFps == doctest::Approx(62.5));
  CHECK(stats.low0_1Fps == doctest::Approx(62.5));
  CHECK(stats.low1Fps == doctest::Approx(62.5));
  CHECK(stats.low5Fps == doctest::Approx(62.5));
  CHECK(stats.maxFps == doctest::Approx(62.5));
}
