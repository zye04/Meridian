#pragma once

#include <span>

namespace meridian {

// Frame rates summarizing a run of frame times. "x% low" is 1000 / the mean
// of the slowest ceil(x% * n) frame times (at least one frame), so a few
// spikes show up where the average hides them; the average is 1000 / the mean
// frame time (not the mean of the per-frame FPS); max is 1000 / the fastest
// frame. All zero for an empty run.
struct FrameStats {
  float averageFps = 0.0f;
  float low0_1Fps = 0.0f;
  float low1Fps = 0.0f;
  float low5Fps = 0.0f;
  float maxFps = 0.0f;
};

FrameStats summarizeFrameTimes(std::span<const float> frameMs);

} // namespace meridian
