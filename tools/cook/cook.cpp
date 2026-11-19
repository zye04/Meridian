// meridian-cook: converts a glTF's PNG/JPEG textures to KTX2 (Basis
// Universal UASTC, Zstandard-supercompressed, full mip chains) and writes a
// standard glTF that references them through KHR_texture_basisu.
//
// UASTC is the highest-quality Basis mode and transcodes almost losslessly to
// BC7 at load time. Color textures are encoded and mip-filtered as sRGB;
// normal maps keep unit-length normals in every mip.

#include <basisu_comp.h>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <meridian/terrain.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

enum class Usage { Color, Data, Normal };

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

std::vector<uint8_t> sourceImageBytes(const fastgltf::Asset &asset,
                                      const fastgltf::Image &image,
                                      const std::filesystem::path &directory) {
  if (const auto *uri = std::get_if<fastgltf::sources::URI>(&image.data)) {
    return readFile(directory / uri->uri->fspath());
  }
  if (const auto *view = std::get_if<fastgltf::sources::BufferView>(&image.data)) {
    const fastgltf::BufferView &bufferView = asset.bufferViews[view->bufferViewIndex];
    const auto &data = asset.buffers[bufferView.bufferIndex].data;
    const std::byte *bytes = nullptr;
    if (const auto *array = std::get_if<fastgltf::sources::Array>(&data)) {
      bytes = array->bytes.data();
    } else if (const auto *vector = std::get_if<fastgltf::sources::Vector>(&data)) {
      bytes = vector->bytes.data();
    } else {
      throw std::runtime_error("image buffer was not loaded");
    }
    const auto *begin =
        reinterpret_cast<const uint8_t *>(bytes + bufferView.byteOffset);
    return {begin, begin + bufferView.byteLength};
  }
  throw std::runtime_error("unsupported image source");
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

// Which way each image is sampled decides how it's encoded.
std::map<size_t, Usage> imageUsages(const fastgltf::Asset &asset) {
  std::map<size_t, Usage> usages;
  auto mark = [&](const auto &info, Usage usage) {
    if (!info) {
      return;
    }
    const fastgltf::Texture &texture = asset.textures[info->textureIndex];
    if (!texture.imageIndex) {
      return;
    }
    auto [it, inserted] = usages.emplace(*texture.imageIndex, usage);
    if (!inserted && it->second != usage) {
      std::fprintf(stderr, "warning: image %zu is used both as color and data\n",
                   *texture.imageIndex);
    }
  };
  for (const fastgltf::Material &m : asset.materials) {
    mark(m.pbrData.baseColorTexture, Usage::Color);
    mark(m.emissiveTexture, Usage::Color);
    mark(m.pbrData.metallicRoughnessTexture, Usage::Data);
    mark(m.occlusionTexture, Usage::Data);
    mark(m.normalTexture, Usage::Normal);
  }
  return usages;
}

void cook(const std::filesystem::path &input, const std::filesystem::path &output) {
  fastgltf::Parser parser(fastgltf::Extensions::KHR_texture_basisu |
                          fastgltf::Extensions::KHR_materials_emissive_strength |
                          fastgltf::Extensions::KHR_texture_transform);
  auto data = fastgltf::GltfDataBuffer::FromPath(input);
  if (data.error() != fastgltf::Error::None) {
    throw std::runtime_error("cannot read " + input.string());
  }
  auto loaded = parser.loadGltf(data.get(), input.parent_path(),
                                fastgltf::Options::LoadExternalBuffers);
  if (loaded.error() != fastgltf::Error::None) {
    throw std::runtime_error(std::string("invalid glTF: ") +
                             std::string(fastgltf::getErrorMessage(loaded.error())));
  }
  fastgltf::Asset &asset = loaded.get();

  basisu::basisu_encoder_init();
  basisu::job_pool jobPool(std::max(1u, std::thread::hardware_concurrency()));

  const auto usages = imageUsages(asset);
  std::set<size_t> opaqueImages;
  for (const auto &[index, usage] : usages) {
    fastgltf::Image &image = asset.images[index];
    std::printf("Encoding image %zu (%s)\n", index,
                usage == Usage::Color ? "color" : usage == Usage::Normal ? "normal" : "data");
    const basisu::image decoded =
        decode(sourceImageBytes(asset, image, input.parent_path()));
    if (!decoded.has_alpha()) {
      opaqueImages.insert(index);
    }
    std::vector<uint8_t> ktx2 = encodeKtx2(decoded, usage, jobPool);

    fastgltf::sources::Vector encoded;
    encoded.bytes.resize(ktx2.size());
    std::memcpy(encoded.bytes.data(), ktx2.data(), ktx2.size());
    encoded.mimeType = fastgltf::MimeType::KTX2;
    image.data = std::move(encoded);
  }

  // Vertex colors with alpha multiply the base color alpha.
  std::set<size_t> vertexAlphaMaterials;
  for (const fastgltf::Mesh &mesh : asset.meshes) {
    for (const fastgltf::Primitive &primitive : mesh.primitives) {
      const auto color = primitive.findAttribute("COLOR_0");
      if (primitive.materialIndex && color != primitive.attributes.cend() &&
          asset.accessors[color->accessorIndex].type == fastgltf::AccessorType::Vec4) {
        vertexAlphaMaterials.insert(*primitive.materialIndex);
      }
    }
  }

  // Exporters often mark materials blended or masked when their alpha is 1
  // everywhere; cooking them opaque skips needless blending and alpha tests.
  for (size_t i = 0; i < asset.materials.size(); ++i) {
    fastgltf::Material &m = asset.materials[i];
    const auto &baseColor = m.pbrData.baseColorTexture;
    const bool opaqueTexture =
        !baseColor ||
        opaqueImages.contains(
            asset.textures[baseColor->textureIndex].imageIndex.value_or(SIZE_MAX));
    if (m.alphaMode != fastgltf::AlphaMode::Opaque && opaqueTexture &&
        !vertexAlphaMaterials.contains(i) && m.pbrData.baseColorFactor[3] == 1.0f &&
        m.alphaCutoff <= 1.0f) {
      std::printf("Material %s has no transparency; cooking it opaque\n",
                  std::string(m.name).c_str());
      m.alphaMode = fastgltf::AlphaMode::Opaque;
    }
  }

  // Point every texture at its KTX2 image through KHR_texture_basisu.
  for (fastgltf::Texture &texture : asset.textures) {
    if (texture.imageIndex && usages.contains(*texture.imageIndex)) {
      texture.basisuImageIndex = texture.imageIndex;
      texture.imageIndex.reset();
    }
  }
  for (auto *list : {&asset.extensionsUsed, &asset.extensionsRequired}) {
    if (std::find(list->begin(), list->end(), "KHR_texture_basisu") == list->end()) {
      list->emplace_back("KHR_texture_basisu");
    }
  }

  std::filesystem::create_directories(output);
  fastgltf::FileExporter exporter;
  exporter.setBufferPath("buffers");
  exporter.setImagePath("textures");
  const auto target = output / (input.stem().string() + ".gltf");
  if (exporter.writeGltfJson(asset, target) != fastgltf::Error::None) {
    throw std::runtime_error("cannot write " + target.string());
  }
  std::printf("Wrote %s\n", target.string().c_str());
}

void cookTexture(const std::filesystem::path &input,
                 const std::filesystem::path &output, Usage usage) {
  basisu::basisu_encoder_init();
  basisu::job_pool jobPool(std::max(1u, std::thread::hardware_concurrency()));
  const std::vector<uint8_t> ktx2 = encodeKtx2(decode(readFile(input)), usage, jobPool);
  if (output.has_parent_path()) {
    std::filesystem::create_directories(output.parent_path());
  }
  std::ofstream stream(output, std::ios::binary);
  stream.write(reinterpret_cast<const char *>(ktx2.data()),
               static_cast<std::streamsize>(ktx2.size()));
  if (!stream) {
    throw std::runtime_error("cannot write " + output.string());
  }
  std::printf("Wrote %s\n", output.string().c_str());
}

// Raw float32 meters (little-endian, row 0 north) -> 16-bit .height file.
void cookHeightfield(const std::filesystem::path &input, uint32_t width,
                     uint32_t height, float spacing,
                     const std::filesystem::path &output) {
  const std::vector<uint8_t> bytes = readFile(input);
  if (bytes.size() != static_cast<size_t>(width) * height * sizeof(float)) {
    throw std::runtime_error("input size does not match width x height float32");
  }
  std::vector<float> heights(static_cast<size_t>(width) * height);
  std::memcpy(heights.data(), bytes.data(), bytes.size());
  // Float32 no-data (swisstopo uses large negatives) must not reach the range.
  if (std::any_of(heights.begin(), heights.end(),
                  [](float h) { return !(h >= -100.0f); })) {
    throw std::runtime_error("input has no-data samples (< -100 m)");
  }
  const meridian::Heightfield field =
      meridian::quantizeHeightfield(heights, width, height, spacing);
  if (output.has_parent_path()) {
    std::filesystem::create_directories(output.parent_path());
  }
  meridian::saveHeightfield(field, output);
  std::printf("Wrote %s (%ux%u, %.1f..%.1f m)\n", output.string().c_str(), width,
              height, field.minHeight, field.minHeight + field.heightScale * 65535.0f);
}

} // namespace

int main(int argc, char **argv) {
  const std::map<std::string, Usage> usages = {
      {"color", Usage::Color}, {"data", Usage::Data}, {"normal", Usage::Normal}};
  const bool texture = argc == 5 && std::strcmp(argv[1], "--texture") == 0 &&
                       usages.contains(argv[4]);
  if (argc == 7 && std::strcmp(argv[1], "--heightfield") == 0) {
    try {
      cookHeightfield(argv[2], static_cast<uint32_t>(std::stoul(argv[3])),
                      static_cast<uint32_t>(std::stoul(argv[4])), std::stof(argv[5]),
                      argv[6]);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "error: %s\n", error.what());
      return 1;
    }
    return 0;
  }
  if (argc != 3 && !texture) {
    std::fprintf(stderr,
                 "usage: meridian-cook <input.gltf|.glb> <output-dir>\n"
                 "       meridian-cook --texture <input.png|.jpg> <output.ktx2> "
                 "<color|data|normal>\n"
                 "       meridian-cook --heightfield <in.f32> <width> <height> "
                 "<spacing> <out.height>\n");
    return 2;
  }
  try {
    if (texture) {
      cookTexture(argv[2], argv[3], usages.at(argv[4]));
    } else {
      cook(argv[1], argv[2]);
    }
  } catch (const std::exception &error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
  return 0;
}
