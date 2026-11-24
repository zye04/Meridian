#include <doctest/doctest.h>
#include <meridian/texture.h>

#include <basisu_comp.h>

namespace {

// Encodes a gradient as a UASTC KTX2 with mipmaps, the way the asset cooker
// will.
std::vector<uint8_t> encodeTestKtx2(uint32_t size) {
  basisu::basisu_encoder_init();

  basisu::image source(size, size);
  for (uint32_t y = 0; y < size; ++y) {
    for (uint32_t x = 0; x < size; ++x) {
      source(x, y).set(static_cast<uint8_t>(x * 255 / size),
                       static_cast<uint8_t>(y * 255 / size), 128, 255);
    }
  }

  basisu::basis_compressor_params params;
  params.m_source_images.push_back(source);
  params.set_format_mode(basist::basis_tex_format::cUASTC_LDR_4x4);
  params.m_create_ktx2_file = true;
  params.m_ktx2_uastc_supercompression = basist::KTX2_SS_ZSTANDARD;
  params.m_mip_gen = true;
  params.m_status_output = false;
  params.m_multithreading = false;
  basisu::job_pool jobPool(1);
  params.m_pJob_pool = &jobPool;

  basisu::basis_compressor compressor;
  REQUIRE(compressor.init(params));
  REQUIRE(compressor.process() == basisu::basis_compressor::cECSuccess);
  const auto &ktx2 = compressor.get_output_ktx2_file();
  return {ktx2.begin(), ktx2.end()};
}

} // namespace

TEST_CASE("KTX2 UASTC loads as BC7 with a full mip chain") {
  std::vector<uint8_t> file = encodeTestKtx2(64);
  meridian::TextureData texture =
      meridian::loadKtx2(file, meridian::ColorSpace::Srgb);

  CHECK(texture.width == 64);
  CHECK(texture.height == 64);
  CHECK(texture.colorSpace == meridian::ColorSpace::Srgb);
  REQUIRE(texture.mips.size() == 7); // 64, 32, 16, 8, 4, 2, 1
  // BC7: 16 bytes per 4x4 block; mips below 4 pixels still use one block.
  CHECK(texture.mips[0].size() == 16 * 16 * 16);
  CHECK(texture.mips[1].size() == 8 * 8 * 16);
  CHECK(texture.mips[6].size() == 16);
}

TEST_CASE("loading garbage as KTX2 throws") {
  std::vector<uint8_t> garbage(100, 0xAB);
  CHECK_THROWS(meridian::loadKtx2(garbage, meridian::ColorSpace::Linear));
}

TEST_CASE("KTX2 decodes to RGBA8 at the largest mip within a size") {
  std::vector<uint8_t> file = encodeTestKtx2(64);
  const meridian::CpuImage image = meridian::loadKtx2Rgba8(file, 32);
  CHECK(image.width == 32);
  CHECK(image.height == 32);
  REQUIRE(image.rgba.size() == 32 * 32 * 4);
  // The gradient: red grows with x, green with y, blue 128, alpha 255.
  const auto px = [&](uint32_t x, uint32_t y, int c) { return int(image.rgba[(y * 32 + x) * 4 + c]); };
  CHECK(px(24, 8, 0) > px(8, 8, 0) + 100); // interior: the mip filter wraps at the edges
  CHECK(px(8, 24, 1) > px(8, 8, 1) + 100);
  CHECK(std::abs(px(10, 10, 2) - 128) <= 8);
  CHECK(px(10, 10, 3) == 255);
}
