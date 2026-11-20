// meridian-cook: converts a glTF's PNG/JPEG textures to KTX2 (Basis
// Universal UASTC, Zstandard-supercompressed, full mip chains) and writes a
// standard glTF that references them through KHR_texture_basisu.
//
// UASTC is the highest-quality Basis mode and transcodes almost losslessly to
// BC7 at load time. Color textures are encoded and mip-filtered as sRGB;
// normal maps keep unit-length normals in every mip.

#include "ktx2.h"
#include "tree.h"

#include <basisu_comp.h>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <meridian/terrain.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

namespace {

using namespace meridian::cook;

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

// Calls fn on each element (3 or 4 floats) of a float accessor, in place.
template <typename Fn> void editFloatAccessor(fastgltf::Asset &asset, size_t index, Fn &&fn) {
  fastgltf::Accessor &accessor = asset.accessors[index];
  if (!accessor.bufferViewIndex || accessor.sparse ||
      accessor.componentType != fastgltf::ComponentType::Float) {
    throw std::runtime_error("cannot bake a transform into a sparse or non-float accessor");
  }
  const fastgltf::BufferView &view = asset.bufferViews[*accessor.bufferViewIndex];
  auto &source = asset.buffers[view.bufferIndex].data;
  std::byte *bytes = nullptr;
  if (auto *array = std::get_if<fastgltf::sources::Array>(&source)) {
    bytes = array->bytes.data();
  } else if (auto *vector = std::get_if<fastgltf::sources::Vector>(&source)) {
    bytes = vector->bytes.data();
  } else {
    throw std::runtime_error("buffer was not loaded");
  }
  const size_t components = fastgltf::getNumComponents(accessor.type);
  const size_t stride = view.byteStride.value_or(components * sizeof(float));
  bytes += view.byteOffset + accessor.byteOffset;
  for (size_t i = 0; i < accessor.count; ++i) {
    float v[4] = {};
    std::memcpy(v, bytes + i * stride, components * sizeof(float));
    fn(v);
    std::memcpy(bytes + i * stride, v, components * sizeof(float));
  }
}

// Scans arrive in centimeters and Z up through their node's rotation and
// scale. Baking those into a mesh that one childless node uses alone leaves
// the mesh upright in meters, so it can stand on its own (a placement
// prototype). Translations stay on the nodes; drawn models don't change.
void bakeNodeRotationScale(fastgltf::Asset &asset) {
  constexpr const char *kBaked[] = {"POSITION", "NORMAL", "TANGENT"};
  std::vector<size_t> users(asset.meshes.size(), 0);
  for (const fastgltf::Node &node : asset.nodes) {
    if (node.meshIndex) {
      ++users[*node.meshIndex];
    }
  }
  // Per mesh, the rotation and scale to bake (none: left as is), and per
  // accessor, the meshes whose baked attributes read it.
  std::vector<std::optional<std::array<float, 7>>> bakes(asset.meshes.size());
  for (fastgltf::Node &node : asset.nodes) {
    const auto *trs = std::get_if<fastgltf::TRS>(&node.transform);
    if (!trs || !node.meshIndex || !node.children.empty() || users[*node.meshIndex] != 1) {
      continue;
    }
    const std::array<float, 7> rotationScale = {trs->rotation[0], trs->rotation[1],
                                                trs->rotation[2], trs->rotation[3],
                                                trs->scale[0],    trs->scale[1],
                                                trs->scale[2]};
    if (rotationScale != std::array<float, 7>{0, 0, 0, 1, 1, 1, 1}) {
      bakes[*node.meshIndex] = rotationScale;
    }
  }
  std::map<size_t, std::set<size_t>> accessorMeshes;
  for (size_t m = 0; m < asset.meshes.size(); ++m) {
    for (const fastgltf::Primitive &primitive : asset.meshes[m].primitives) {
      for (const char *name : kBaked) {
        const auto attribute = primitive.findAttribute(name);
        if (attribute != primitive.attributes.cend()) {
          accessorMeshes[attribute->accessorIndex].insert(m);
        }
      }
    }
  }
  // A shared accessor is baked once, so every mesh reading it must bake the
  // same rotation and scale.
  for (const auto &[accessor, meshes] : accessorMeshes) {
    const auto &first = bakes[*meshes.begin()];
    for (size_t m : meshes) {
      if (bakes[m] != first) {
        throw std::runtime_error("cannot bake node transforms: accessor " +
                                 std::to_string(accessor) +
                                 " is shared by meshes with different node rotations or "
                                 "scales");
      }
    }
  }
  std::set<size_t> baked;
  for (fastgltf::Node &node : asset.nodes) {
    auto *trs = std::get_if<fastgltf::TRS>(&node.transform);
    if (!trs || !node.meshIndex || !bakes[*node.meshIndex]) {
      continue;
    }
    const float x = trs->rotation[0], y = trs->rotation[1], z = trs->rotation[2],
                w = trs->rotation[3];
    const float scale[3] = {trs->scale[0], trs->scale[1], trs->scale[2]};
    if (!(scale[0] > 0.0f && scale[1] > 0.0f && scale[2] > 0.0f)) {
      throw std::runtime_error("cannot bake a mirroring or zero node scale");
    }
    const float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    // Rotates v after scaling it by scale^power (1: points, -1: normals).
    const auto apply = [&](float *v, int power, bool unit) {
      float scaled[3], out[3];
      for (int i = 0; i < 3; ++i) {
        scaled[i] = power > 0 ? v[i] * scale[i] : v[i] / scale[i];
      }
      for (int i = 0; i < 3; ++i) {
        out[i] = r[i][0] * scaled[0] + r[i][1] * scaled[1] + r[i][2] * scaled[2];
      }
      const float length = unit ? std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2])
                                : 1.0f;
      for (int i = 0; i < 3; ++i) {
        v[i] = length > 0.0f ? out[i] / length : out[i];
      }
    };
    for (const fastgltf::Primitive &primitive : asset.meshes[*node.meshIndex].primitives) {
      for (const auto &[name, power, unit] :
           {std::tuple{"POSITION", 1, false}, {"NORMAL", -1, true}, {"TANGENT", 1, true}}) {
        const auto attribute = primitive.findAttribute(name);
        if (attribute == primitive.attributes.cend() ||
            !baked.insert(attribute->accessorIndex).second) {
          continue;
        }
        float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        editFloatAccessor(asset, attribute->accessorIndex, [&](float *v) {
          apply(v, power, unit);
          for (int i = 0; i < 3; ++i) {
            lo[i] = std::min(lo[i], v[i]);
            hi[i] = std::max(hi[i], v[i]);
          }
        });
        if (std::strcmp(name, "POSITION") == 0) {
          fastgltf::Accessor &accessor = asset.accessors[attribute->accessorIndex];
          accessor.min = fastgltf::AccessorBoundsArray::ForType<double>(3);
          accessor.max = fastgltf::AccessorBoundsArray::ForType<double>(3);
          for (int i = 0; i < 3; ++i) {
            accessor.min->data<double>()[i] = lo[i];
            accessor.max->data<double>()[i] = hi[i];
          }
        }
      }
    }
    trs->rotation = fastgltf::math::nquat(0.0f, 0.0f, 0.0f, 1.0f);
    trs->scale = fastgltf::math::nvec3(1.0f);
    std::printf("Baked node %s's rotation and scale into its mesh\n",
                std::string(node.name).c_str());
  }
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
  bakeNodeRotationScale(asset);

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
  if ((argc == 6 || argc == 8) && std::strcmp(argv[1], "--tree") == 0 &&
      std::strcmp(argv[4], "--triangles") == 0 &&
      (argc == 6 || std::strcmp(argv[6], "--bark-texture") == 0)) {
    try {
      const TreeCookResult result =
          cookTree(argv[2], argv[3], std::stoull(argv[5]), argc == 8 ? argv[7] : "");
      std::printf("source %zu tris -> %zu tris, leaf area %.2f -> %.2f m^2 (%+.2f%%)\n",
                  result.sourceTriangles, result.triangles, result.leafAreaBefore,
                  result.leafAreaAfter,
                  100.0 * (result.leafAreaAfter - result.leafAreaBefore) /
                      result.leafAreaBefore);
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
                 "<spacing> <out.height>\n"
                 "       meridian-cook --tree <in.gltf> <output-dir> --triangles <n> "
                 "[--bark-texture <dir>]\n");
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
