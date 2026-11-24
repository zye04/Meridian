#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace meridian {

// Color textures (base color, emissive) are sRGB-encoded; data textures
// (normals, roughness, occlusion) are linear.
enum class ColorSpace { Srgb, Linear };

// A GPU-ready texture: BC7-compressed blocks for every mip level, largest
// first.
struct TextureData {
  uint32_t width = 0;
  uint32_t height = 0;
  ColorSpace colorSpace = ColorSpace::Srgb;
  std::vector<std::vector<uint8_t>> mips;
};

// Loads a KTX2 file with Basis Universal (UASTC or ETC1S) content and
// transcodes it to BC7. UASTC is a subset of BC7, so that step is nearly
// lossless.
TextureData loadKtx2(const std::filesystem::path &path, ColorSpace colorSpace);
TextureData loadKtx2(const std::vector<uint8_t> &file, ColorSpace colorSpace);

// A decoded RGBA8 image (row-major from the top-left), for CPU sampling.
struct CpuImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};

// Decodes a Basis Universal KTX2 to RGBA8 at its largest mip no larger than
// maxSize on either side (the smallest mip when none fits).
CpuImage loadKtx2Rgba8(const std::vector<uint8_t> &file, uint32_t maxSize);
CpuImage loadKtx2Rgba8(const std::filesystem::path &path, uint32_t maxSize);

} // namespace meridian
