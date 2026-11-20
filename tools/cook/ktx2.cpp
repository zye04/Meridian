#include "ktx2.h"

#include <algorithm>
#include <cmath>

#include <fstream>
#include <stdexcept>

namespace meridian::cook {

std::vector<uint8_t> readFile(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw std::runtime_error("cannot open " + path.string());
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(stream.tellg()));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  return bytes;
}

basisu::image decode(const std::vector<uint8_t> &bytes) {
  basisu::image image;
  const bool isPng = bytes.size() > 4 && bytes[0] == 0x89 && bytes[1] == 'P';
  const bool ok = isPng ? basisu::load_png(bytes.data(), bytes.size(), image)
                        : basisu::load_jpg(bytes.data(), bytes.size(), image);
  if (!ok) {
    throw std::runtime_error("cannot decode PNG/JPEG image");
  }
  return image;
}

namespace {

double coverage(const basisu::image &image, int channel, double scale) {
  size_t above = 0;
  const size_t count = static_cast<size_t>(image.get_width()) * image.get_height();
  for (size_t i = 0; i < count; ++i) {
    above += image.get_ptr()[i][channel] * scale >= 127.5;
  }
  return static_cast<double>(above) / count;
}

float toLinear(uint8_t v) {
  const float c = v / 255.0f;
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

uint8_t toSrgb(float v) {
  const float c = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
  return static_cast<uint8_t>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Box-filters to half size (colour channels averaged weighted by the mask,
// in linear light for color), then rescales the mask channel to keep
// `target` coverage.
basisu::image coverageMip(const basisu::image &src, Usage usage, int channel, double target) {
  const uint32_t w = std::max(1u, src.get_width() / 2), h = std::max(1u, src.get_height() / 2);
  basisu::image dst(w, h);
  const bool srgb = usage == Usage::Color;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      float sum[4] = {}, weight = 0.0f;
      for (uint32_t k = 0; k < 4; ++k) {
        const basisu::color_rgba &c =
            src(std::min(2 * x + (k & 1), src.get_width() - 1),
                std::min(2 * y + (k >> 1), src.get_height() - 1));
        const float wgt = c[channel] / 255.0f + 1e-4f;
        weight += wgt;
        for (int i = 0; i < 4; ++i) {
          sum[i] += (srgb && i < 3 ? toLinear(c[i]) : c[i] / 255.0f) * (i == channel ? 1.0f : wgt);
        }
      }
      basisu::color_rgba &out = dst(x, y);
      for (int i = 0; i < 4; ++i) {
        const float v = i == channel ? sum[i] / 4.0f : sum[i] / weight;
        out[i] = srgb && i < 3 ? toSrgb(v)
                               : static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
      }
    }
  }
  double lo = 0.0, hi = 8.0;
  for (int i = 0; i < 24; ++i) {
    const double mid = (lo + hi) / 2;
    (coverage(dst, channel, mid) < target ? lo : hi) = mid;
  }
  const double scale = (lo + hi) / 2;
  for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
    uint8_t &v = dst.get_ptr()[i][channel];
    v = static_cast<uint8_t>(std::min(255.0, v * scale + 0.5));
  }
  return dst;
}

} // namespace

std::vector<uint8_t> encodeKtx2(const basisu::image &image, Usage usage,
                                basisu::job_pool &jobPool, int coverageChannel) {
  basisu::basis_compressor_params params;
  params.m_source_images.push_back(image);
  params.set_format_mode(basist::basis_tex_format::cUASTC_LDR_4x4);
  params.m_pack_uastc_ldr_4x4_flags = basisu::cPackUASTCLevelSlower;
  params.m_create_ktx2_file = true;
  params.m_ktx2_uastc_supercompression = basist::KTX2_SS_ZSTANDARD;
  params.m_mip_gen = coverageChannel < 0;
  if (coverageChannel >= 0) {
    const double target = coverage(image, coverageChannel, 1.0);
    basisu::vector<basisu::image> mips;
    basisu::image previous = image;
    while (previous.get_width() > 1 || previous.get_height() > 1) {
      previous = coverageMip(previous, usage, coverageChannel, target);
      mips.push_back(previous);
    }
    params.m_source_mipmap_images.push_back(std::move(mips));
  }
  // Perceptual error metric, sRGB mip filtering and the KTX2 sRGB transfer
  // function, all on for color and off for data.
  params.set_srgb_options(usage == Usage::Color);
  params.m_mip_renormalize = usage == Usage::Normal;
  params.m_status_output = false;
  params.m_multithreading = true;
  params.m_pJob_pool = &jobPool;

  basisu::basis_compressor compressor;
  if (!compressor.init(params) ||
      compressor.process() != basisu::basis_compressor::cECSuccess) {
    throw std::runtime_error("KTX2 encoding failed");
  }
  const auto &ktx2 = compressor.get_output_ktx2_file();
  return {ktx2.begin(), ktx2.end()};
}

} // namespace meridian::cook
