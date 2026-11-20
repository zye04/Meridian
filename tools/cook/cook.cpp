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

// glTF's ORM packing puts occlusion in R, so an _arm pack bound only as
// metallicRoughness serves as the occlusion map too (Poly Haven omits the binding).
void bindArmOcclusion(fastgltf::Asset &asset) {
  for (fastgltf::Material &m : asset.materials) {
    const auto &mr = m.pbrData.metallicRoughnessTexture;
    if (!mr || m.occlusionTexture) {
      continue;
    }
    const auto image = asset.textures[mr->textureIndex].imageIndex;
    const auto *uri =
        image ? std::get_if<fastgltf::sources::URI>(&asset.images[*image].data) : nullptr;
    if (uri && uri->uri->fspath().filename().string().find("_arm") != std::string::npos) {
      // The same texture info, transform included: the loader requires one
      // texture transform per material.
      fastgltf::OcclusionTextureInfo occlusion;
      static_cast<fastgltf::TextureInfo &>(occlusion) = *mr;
      m.occlusionTexture = std::move(occlusion);
    }
  }
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
// scale, and rocks of a set sit on the ground through their node's vertical
// offset. Baking those into a mesh that one childless node uses alone
// leaves the mesh upright in meters with its base where the set's ground
// is, so it can stand on its own (a placement prototype). Horizontal
// translations stay on the nodes; drawn models don't change.
void bakeNodeTransform(fastgltf::Asset &asset) {
  constexpr const char *kBaked[] = {"POSITION", "NORMAL", "TANGENT"};
  std::vector<size_t> users(asset.meshes.size(), 0);
  for (const fastgltf::Node &node : asset.nodes) {
    if (node.meshIndex) {
      ++users[*node.meshIndex];
    }
  }
  // Per mesh, the rotation, scale and vertical offset to bake (none: left as
  // is), and per accessor, the meshes whose baked attributes read it.
  std::vector<std::optional<std::array<float, 8>>> bakes(asset.meshes.size());
  for (fastgltf::Node &node : asset.nodes) {
    const auto *trs = std::get_if<fastgltf::TRS>(&node.transform);
    if (!trs || !node.meshIndex || !node.children.empty() || users[*node.meshIndex] != 1) {
      continue;
    }
    const std::array<float, 8> bake = {trs->rotation[0], trs->rotation[1], trs->rotation[2],
                                       trs->rotation[3], trs->scale[0],    trs->scale[1],
                                       trs->scale[2],    trs->translation[1]};
    if (bake != std::array<float, 8>{0, 0, 0, 1, 1, 1, 1, 0}) {
      bakes[*node.meshIndex] = bake;
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
  // same rotation, scale and vertical offset.
  for (const auto &[accessor, meshes] : accessorMeshes) {
    const auto &first = bakes[*meshes.begin()];
    for (size_t m : meshes) {
      if (bakes[m] != first) {
        throw std::runtime_error("cannot bake node transforms: accessor " +
                                 std::to_string(accessor) +
                                 " is shared by meshes with different node rotations, "
                                 "scales or vertical offsets");
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
    const float offsetY = trs->translation[1];
    // Rotates v after scaling it by scale^power (1: points, -1: normals);
    // points also move up by the node's vertical offset.
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
      if (power > 0 && !unit) {
        v[1] += offsetY;
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
    trs->translation[1] = 0.0f;
    std::printf("Baked node %s's rotation, scale and vertical offset into its mesh\n",
                std::string(node.name).c_str());
  }
}

// Megascans 3D plants pack ORT: opacity (R), roughness (G), translucency
// (B); the nonUE glTF binds it as metallicRoughness, the UE one also as
// occlusion, and the fern has no alpha in its base color. Rebuild the
// textures into glTF's layout (base color alpha = opacity, roughness in G,
// no metal), make the material alpha-masked and diffuse-transmissive by the
// mean translucency over the opaque texels, leave the billboard material
// (the LOD 2/3 quads) empty (its textures are cooked on their own), and keep one
// node per variant's LOD 0, side by side (the engine builds its own LODs).
struct PlantImages {
  std::map<size_t, basisu::image> replaced;
  std::set<size_t> masks; // base color images whose alpha is the alpha-test mask
};

PlantImages preparePlants(fastgltf::Asset &asset, const std::filesystem::path &directory) {
  PlantImages result;
  std::set<size_t> usedMaterials;
  for (const fastgltf::Node &node : asset.nodes) {
    if (!node.meshIndex || node.name.find("_LOD") != std::string::npos) {
      continue;
    }
    for (const fastgltf::Primitive &primitive : asset.meshes[*node.meshIndex].primitives) {
      if (primitive.materialIndex) {
        usedMaterials.insert(*primitive.materialIndex);
      }
    }
  }
  const auto imageOf = [&](const auto &info) -> std::optional<size_t> {
    return info ? asset.textures[info->textureIndex].imageIndex : std::nullopt;
  };
  bool plant = false;
  for (size_t i = 0; i < asset.materials.size(); ++i) {
    fastgltf::Material &m = asset.materials[i];
    const auto orImage = imageOf(m.pbrData.metallicRoughnessTexture);
    const auto *uri = orImage ? std::get_if<fastgltf::sources::URI>(&asset.images[*orImage].data)
                              : nullptr;
    if (!usedMaterials.contains(i) || !uri ||
        uri->uri->fspath().filename().string().find("_ORT") == std::string::npos) {
      continue;
    }
    plant = true;
    const auto colorImage = imageOf(m.pbrData.baseColorTexture);
    if (!colorImage) {
      throw std::runtime_error("plant material without a base color texture");
    }
    basisu::image ort = decode(sourceImageBytes(asset, asset.images[*orImage], directory));
    basisu::image color =
        decode(sourceImageBytes(asset, asset.images[*colorImage], directory));
    if (ort.get_width() != color.get_width() || ort.get_height() != color.get_height()) {
      throw std::runtime_error("ORT and base color sizes differ");
    }
    double translucency = 0.0;
    size_t opaque = 0;
    for (uint32_t k = 0; k < ort.get_width() * ort.get_height(); ++k) {
      basisu::color_rgba &o = ort.get_ptr()[k];
      basisu::color_rgba &c = color.get_ptr()[k];
      c.a = o.r;
      if (o.r >= 128) {
        translucency += o.b / 255.0;
        ++opaque;
      }
      o.r = 255;
      o.b = 0;
      o.a = 255;
    }
    result.replaced[*orImage] = std::move(ort);
    result.replaced[*colorImage] = std::move(color);
    result.masks.insert(*colorImage);
    const float factor = opaque ? static_cast<float>(translucency / opaque) : 0.0f;
    m.occlusionTexture.reset();
    m.pbrData.metallicFactor = 0.0f;
    m.alphaMode = fastgltf::AlphaMode::Mask;
    m.alphaCutoff = 0.5f;
    m.doubleSided = true;
    m.diffuseTransmission = fastgltf::MaterialDiffuseTransmission{};
    m.diffuseTransmission->diffuseTransmissionFactor = factor;
    std::printf("Plant material %s: mean translucency %.3f over %zu opaque texels\n",
                std::string(m.name).c_str(), factor, opaque);
  }
  if (!plant) {
    return result;
  }
  for (size_t i = 0; i < asset.materials.size(); ++i) {
    if (!usedMaterials.contains(i)) {
      fastgltf::Material &m = asset.materials[i];
      m.pbrData.baseColorTexture.reset();
      m.pbrData.metallicRoughnessTexture.reset();
      m.normalTexture.reset();
      m.occlusionTexture.reset();
    }
  }
  // The billboard's textures were the last ones: drop them so no reference
  // is left to files that aren't cooked with the model.
  const auto textureUsed = [&](size_t t) {
    for (const fastgltf::Material &m : asset.materials) {
      for (const auto *info :
           {&m.pbrData.baseColorTexture, &m.pbrData.metallicRoughnessTexture}) {
        if (*info && (*info)->textureIndex == t) {
          return true;
        }
      }
      if ((m.normalTexture && m.normalTexture->textureIndex == t) ||
          (m.occlusionTexture && m.occlusionTexture->textureIndex == t) ||
          (m.emissiveTexture && m.emissiveTexture->textureIndex == t)) {
        return true;
      }
    }
    return false;
  };
  while (!asset.textures.empty() && !textureUsed(asset.textures.size() - 1)) {
    asset.textures.pop_back();
  }
  const auto imageUsed = [&](size_t i) {
    return std::any_of(asset.textures.begin(), asset.textures.end(),
                       [&](const fastgltf::Texture &t) { return t.imageIndex == i; });
  };
  while (!asset.images.empty() && !imageUsed(asset.images.size() - 1) &&
         !result.replaced.contains(asset.images.size() - 1)) {
    asset.images.pop_back();
  }
  return result;
}

// The nodes of a Megascans plant scene: LOD 0 of each variant, in a row.
// Runs after the node transforms are baked into the meshes.
void layoutPlantVariants(fastgltf::Asset &asset) {
  if (asset.scenes.empty()) {
    return;
  }
  auto &roots = asset.scenes[asset.defaultScene.value_or(0)].nodeIndices;
  std::erase_if(roots, [&](size_t n) {
    return asset.nodes[n].name.find("_LOD") != std::string::npos;
  });
  constexpr float kGap = 0.1f;
  float cursor = 0.0f;
  for (size_t n : roots) {
    fastgltf::Node &node = asset.nodes[n];
    auto *trs = std::get_if<fastgltf::TRS>(&node.transform);
    if (!trs || !node.meshIndex) {
      continue;
    }
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (const fastgltf::Primitive &primitive : asset.meshes[*node.meshIndex].primitives) {
      const fastgltf::Accessor &accessor =
          asset.accessors[primitive.findAttribute("POSITION")->accessorIndex];
      for (int i = 0; i < 3; ++i) {
        lo[i] = std::min(lo[i], static_cast<float>(accessor.min->data<double>()[i]));
        hi[i] = std::max(hi[i], static_cast<float>(accessor.max->data<double>()[i]));
      }
    }
    trs->translation[0] = cursor - lo[0];
    trs->translation[2] = -(lo[2] + hi[2]) / 2;
    cursor += hi[0] - lo[0] + kGap;
  }
}

// The plants' glTFs carry no normals. Smooth ones from the faces, so a
// blade or frond bends across its width; the shader's double-sided
// handling flips them for back faces.
void addPlantNormals(fastgltf::Asset &asset) {
  auto &data = asset.buffers[0].data;
  if (const auto *array = std::get_if<fastgltf::sources::Array>(&data)) {
    fastgltf::sources::Vector vector;
    vector.bytes.assign(array->bytes.begin(), array->bytes.end());
    data = std::move(vector);
  }
  auto *buffer = std::get_if<fastgltf::sources::Vector>(&data);
  if (!buffer) {
    throw std::runtime_error("buffer was not loaded");
  }
  std::set<size_t> done;
  for (fastgltf::Mesh &mesh : asset.meshes) {
    for (fastgltf::Primitive &primitive : mesh.primitives) {
      const auto position = primitive.findAttribute("POSITION");
      if (position == primitive.attributes.cend() || !primitive.indicesAccessor ||
          primitive.findAttribute("NORMAL") != primitive.attributes.cend() ||
          !done.insert(position->accessorIndex).second) {
        continue;
      }
      const size_t positionIndex = position->accessorIndex;
      const fastgltf::Accessor &positions = asset.accessors[positionIndex];
      std::vector<std::array<float, 3>> p(positions.count);
      fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
          asset, positions,
          [&](fastgltf::math::fvec3 v, size_t i) { p[i] = {v.x(), v.y(), v.z()}; });
      const fastgltf::Accessor &indexAccessor = asset.accessors[*primitive.indicesAccessor];
      std::vector<uint32_t> indices(indexAccessor.count);
      fastgltf::copyFromAccessor<uint32_t>(asset, indexAccessor, indices.data());
      std::vector<std::array<float, 3>> normals(p.size(), {0.0f, 0.0f, 0.0f});
      for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const auto &a = p[indices[t]], &b = p[indices[t + 1]], &c = p[indices[t + 2]];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                            e1[0] * e2[1] - e1[1] * e2[0]};
        for (size_t k = 0; k < 3; ++k) {
          for (int i = 0; i < 3; ++i) {
            normals[indices[t + k]][i] += n[i];
          }
        }
      }
      for (auto &n : normals) {
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        n = length > 0.0f ? std::array<float, 3>{n[0] / length, n[1] / length, n[2] / length}
                          : std::array<float, 3>{0.0f, 0.0f, 1.0f};
      }
      const size_t offset = buffer->bytes.size();
      buffer->bytes.resize(offset + normals.size() * sizeof(float) * 3);
      std::memcpy(buffer->bytes.data() + offset, normals.data(),
                  normals.size() * sizeof(float) * 3);
      asset.buffers[0].byteLength = buffer->bytes.size();
      fastgltf::BufferView view;
      view.bufferIndex = 0;
      view.byteOffset = offset;
      view.byteLength = normals.size() * sizeof(float) * 3;
      asset.bufferViews.push_back(view);
      fastgltf::Accessor accessor;
      accessor.bufferViewIndex = asset.bufferViews.size() - 1;
      accessor.count = normals.size();
      accessor.type = fastgltf::AccessorType::Vec3;
      accessor.componentType = fastgltf::ComponentType::Float;
      asset.accessors.push_back(std::move(accessor));
      const size_t normalAccessor = asset.accessors.size() - 1;
      for (fastgltf::Mesh &other : asset.meshes) {
        for (fastgltf::Primitive &user : other.primitives) {
          const auto shared = user.findAttribute("POSITION");
          if (shared != user.attributes.cend() && shared->accessorIndex == positionIndex) {
            user.attributes.push_back({"NORMAL", normalAccessor});
          }
        }
      }
    }
  }
}

void cook(const std::filesystem::path &input, const std::filesystem::path &output) {
  fastgltf::Parser parser(fastgltf::Extensions::KHR_texture_basisu |
                          fastgltf::Extensions::KHR_materials_emissive_strength |
                          fastgltf::Extensions::KHR_texture_transform |
                          fastgltf::Extensions::KHR_materials_diffuse_transmission);
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
  bakeNodeTransform(asset);
  bindArmOcclusion(asset);
  PlantImages plantImages = preparePlants(asset, input.parent_path());
  if (!plantImages.replaced.empty()) {
    layoutPlantVariants(asset);
    addPlantNormals(asset);
  }

  basisu::basisu_encoder_init();
  basisu::job_pool jobPool(std::max(1u, std::thread::hardware_concurrency()));

  const auto usages = imageUsages(asset);
  std::set<size_t> opaqueImages;
  for (const auto &[index, usage] : usages) {
    fastgltf::Image &image = asset.images[index];
    std::printf("Encoding image %zu (%s)\n", index,
                usage == Usage::Color ? "color" : usage == Usage::Normal ? "normal" : "data");
    const auto replaced = plantImages.replaced.find(index);
    const basisu::image decoded =
        replaced != plantImages.replaced.end()
            ? std::move(replaced->second)
            : decode(sourceImageBytes(asset, image, input.parent_path()));
    if (!decoded.has_alpha()) {
      opaqueImages.insert(index);
    }
    std::vector<uint8_t> ktx2 =
        encodeKtx2(decoded, usage, jobPool, plantImages.masks.contains(index) ? 3 : -1);

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
                 const std::filesystem::path &output, Usage usage, int coverageChannel) {
  basisu::basisu_encoder_init();
  basisu::job_pool jobPool(std::max(1u, std::thread::hardware_concurrency()));
  const std::vector<uint8_t> ktx2 =
      encodeKtx2(decode(readFile(input)), usage, jobPool, coverageChannel);
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
      {"color", Usage::Color}, {"data", Usage::Data}, {"normal", Usage::Normal},
      {"opacity", Usage::Data}};
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
      cookTexture(argv[2], argv[3], usages.at(argv[4]),
                  std::strcmp(argv[4], "opacity") == 0 ? 0 : -1);
    } else {
      cook(argv[1], argv[2]);
    }
  } catch (const std::exception &error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
  return 0;
}
