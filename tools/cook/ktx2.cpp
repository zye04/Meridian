#include "ktx2.h"

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

std::vector<uint8_t> encodeKtx2(const basisu::image &image, Usage usage,
                                basisu::job_pool &jobPool) {
  basisu::basis_compressor_params params;
  params.m_source_images.push_back(image);
  params.set_format_mode(basist::basis_tex_format::cUASTC_LDR_4x4);
  params.m_pack_uastc_ldr_4x4_flags = basisu::cPackUASTCLevelSlower;
  params.m_create_ktx2_file = true;
  params.m_ktx2_uastc_supercompression = basist::KTX2_SS_ZSTANDARD;
  params.m_mip_gen = true;
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
