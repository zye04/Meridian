#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace meridian {

struct Heightfield;

// Per-texel 8-bit channels cooked from a heightfield (meridian-cook --geo).
// The engine treats them as generic data: what each channel means is the
// cook's and the game's business. A texel covers ratio x ratio heightfield
// samples, so the heightfield is exactly ratio times the grid on each side;
// row 0 = north, like the heightfield.
constexpr uint32_t kGeoChannelCount = 8;

struct GeoChannels {
  uint32_t width = 0, height = 0;
  uint32_t ratio = 0;
  // The heightfield it was cooked from.
  uint32_t fieldWidth = 0, fieldHeight = 0;
  float fieldSpacing = 0.0f;
  std::vector<uint8_t> texels; // kGeoChannelCount per texel

  uint8_t at(uint32_t i, uint32_t j, uint32_t channel) const {
    return texels[(static_cast<size_t>(j) * width + i) * kGeoChannelCount + channel];
  }
};

void saveGeoChannels(const GeoChannels &geo, const std::filesystem::path &path);
GeoChannels loadGeoChannels(const std::filesystem::path &path);
bool geoMatches(const GeoChannels &geo, const Heightfield &field);
// x.height -> x.geo
std::filesystem::path geoPathFor(const std::filesystem::path &heightPath);

} // namespace meridian
