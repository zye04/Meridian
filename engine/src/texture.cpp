#include <meridian/texture.h>

#include <basisu_transcoder.h>

#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace meridian {

namespace {

void initTranscoder() {
  static std::once_flag once;
  std::call_once(once, [] { basist::basisu_transcoder_init(); });
}

} // namespace

TextureData loadKtx2(const std::vector<uint8_t> &file, ColorSpace colorSpace) {
  initTranscoder();

  basist::ktx2_transcoder transcoder;
  if (!transcoder.init(file.data(), static_cast<uint32_t>(file.size())) ||
      !transcoder.start_transcoding()) {
    throw std::runtime_error("not a valid Basis Universal KTX2 file");
  }
  if (transcoder.get_faces() != 1 || transcoder.get_layers() > 1) {
    throw std::runtime_error("only plain 2D KTX2 textures are supported");
  }

  TextureData texture;
  texture.width = transcoder.get_width();
  texture.height = transcoder.get_height();
  texture.colorSpace = colorSpace;
  texture.mips.resize(transcoder.get_levels());

  constexpr uint32_t kBc7BlockBytes = 16;
  for (uint32_t level = 0; level < transcoder.get_levels(); ++level) {
    basist::ktx2_image_level_info info;
    if (!transcoder.get_image_level_info(info, level, 0, 0)) {
      throw std::runtime_error("bad KTX2 mip level");
    }
    std::vector<uint8_t> &blocks = texture.mips[level];
    blocks.resize(static_cast<size_t>(info.m_total_blocks) * kBc7BlockBytes);
    if (!transcoder.transcode_image_level(
            level, 0, 0, blocks.data(), info.m_total_blocks,
            basist::transcoder_texture_format::cTFBC7_RGBA)) {
      throw std::runtime_error("KTX2 transcoding to BC7 failed");
    }
  }
  return texture;
}

TextureData loadKtx2(const std::filesystem::path &path, ColorSpace colorSpace) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw std::runtime_error("cannot open " + path.string());
  }
  std::vector<uint8_t> file(static_cast<size_t>(stream.tellg()));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(file.data()),
              static_cast<std::streamsize>(file.size()));
  return loadKtx2(file, colorSpace);
}

} // namespace meridian
