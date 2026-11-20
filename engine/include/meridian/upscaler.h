// Anti-aliasing and upscaling settings, and the rules that tie the render
// resolution to the display resolution (DLSS programming guide).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace meridian {

struct Extent {
  uint32_t width = 0, height = 0;
  bool operator==(const Extent &) const = default;
};

enum class AaMethod { Taa, Dlss };
enum class DlssMode { Dlaa, Quality, Balanced, Performance };

struct AaSettings {
  AaMethod method = AaMethod::Taa;
  DlssMode mode = DlssMode::Dlaa;
  char preset = 0; // 0: the DLSS module's per-mode default
  bool rayReconstruction = false;
  float renderScale = 1.0f;
};

inline Extent scaledExtent(Extent display, float scale) {
  auto side = [scale](uint32_t size) {
    return std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(size * double{scale})));
  };
  return {side(display.width), side(display.height)};
}

// Halton phases per jitter cycle: enough that every display pixel gets about
// 8 samples (Ray Reconstruction wants at least 32).
inline uint32_t jitterPhaseCount(Extent render, Extent display, bool rayReconstruction) {
  const double ratio = static_cast<double>(display.width) / render.width;
  const auto phases = static_cast<uint32_t>(std::ceil(8.0 * ratio * ratio));
  return std::max(rayReconstruction ? 32u : 8u, phases);
}

// Textures sample at the display's detail: one level sharper per halving of
// the render width, plus one more when upscaling.
inline float textureMipBias(Extent render, Extent display) {
  const float bias = std::log2(static_cast<float>(render.width) / display.width);
  return render.width < display.width ? bias - 1.0f : bias;
}

} // namespace meridian
