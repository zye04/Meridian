#pragma once

// What the geo channels (meridian/geo.h) mean and how each is encoded in
// its byte: shared by the cook, which writes them, and the game, whose
// placement rules read them. The engine knows none of this.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace meridian::geo {

enum Channel : uint32_t {
  kWetness = 0,          // topographic wetness index ln(a / tan b)
  kFlow = 1,             // log2 specific catchment area (m^2/m)
  kLocalPosition = 2,    // height minus the mean within ~6 m (m)
  kLandformPosition = 3, // height minus the mean within ~40 m (m)
  kSunExposure = 4,      // annual direct sun relative to a flat open surface
  kTalus = 5,            // rockfall reach below steep ground, 1..255 = closeness to the source
};

inline uint8_t toByte(float v) {
  return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
}

inline uint8_t encodeWetness(float twi) { return toByte((twi - 2.0f) / 18.0f * 255.0f); }
inline float decodeWetness(float b) { return 2.0f + b / 255.0f * 18.0f; }
inline uint8_t encodeFlow(float sca) { return toByte(std::log2(std::max(sca, 1.0f)) * 12.75f); }
inline float decodeFlow(float b) { return std::exp2(b / 12.75f); }
inline uint8_t encodeLocalPosition(float m) { return toByte(128.0f + m * 63.5f); }
inline float decodeLocalPosition(float b) { return (b - 128.0f) / 63.5f; }
inline uint8_t encodeLandformPosition(float m) { return toByte(128.0f + m * 7.94f); }
inline float decodeLandformPosition(float b) { return (b - 128.0f) / 7.94f; }
inline uint8_t encodeSunExposure(float relative) { return toByte(255.0f * relative / 1.5f); }
inline float decodeSunExposure(float b) { return b / 255.0f * 1.5f; }
// Closeness 0..1 (0 = out of reach).
inline uint8_t encodeTalus(float closeness) {
  return closeness > 0.0f ? std::max<uint8_t>(1, toByte(closeness * 255.0f)) : 0;
}

} // namespace meridian::geo
