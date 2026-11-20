#include <meridian/frame_stats.h>

#include <algorithm>
#include <functional>
#include <vector>

namespace meridian {

FrameStats summarizeFrameTimes(std::span<const float> frameMs) {
  if (frameMs.empty()) {
    return {};
  }
  std::vector<float> slowestFirst(frameMs.begin(), frameMs.end());
  std::sort(slowestFirst.begin(), slowestFirst.end(), std::greater<>());
  const size_t n = slowestFirst.size();

  // The mean of the slowest ceil(permille / 1000 * n) frames, as FPS.
  auto low = [&](size_t permille) {
    const size_t count = std::max<size_t>(1, (permille * n + 999) / 1000);
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
      sum += slowestFirst[i];
    }
    return static_cast<float>(1000.0 * count / sum);
  };

  double total = 0.0;
  for (float ms : slowestFirst) {
    total += ms;
  }
  return {.averageFps = static_cast<float>(1000.0 * n / total),
          .low0_1Fps = low(1),
          .low1Fps = low(10),
          .low5Fps = low(50),
          .maxFps = 1000.0f / slowestFirst.back()};
}

} // namespace meridian
