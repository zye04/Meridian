#include <meridian/model.h>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <mikktspace.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <variant>

namespace meridian {

namespace {

// --- Tangent generation (MikkTSpace)

// MikkTSpace matches every triangle with two equal corners against all the
// others (O(n^2)): 50k of them in a million-triangle leaf mesh took minutes.
// They cover no pixels, so only the other triangles are handed to it.
struct TangentFaces {
  MeshData *mesh;
  std::vector<uint32_t> faces;
};

int faceCount(const SMikkTSpaceContext *context) {
  return static_cast<int>(static_cast<TangentFaces *>(context->m_pUserData)->faces.size());
}

int verticesOfFace(const SMikkTSpaceContext *, int) { return 3; }

Vertex &faceVertex(const SMikkTSpaceContext *context, int face, int vert) {
  auto *data = static_cast<TangentFaces *>(context->m_pUserData);
  return data->mesh->vertices[data->mesh->indices[static_cast<size_t>(data->faces[face]) * 3 + vert]];
}

void position(const SMikkTSpaceContext *context, float out[], int face, int vert) {
  const Vertex &v = faceVertex(context, face, vert);
  out[0] = v.position.x;
  out[1] = v.position.y;
  out[2] = v.position.z;
}

void normal(const SMikkTSpaceContext *context, float out[], int face, int vert) {
  const Vertex &v = faceVertex(context, face, vert);
  out[0] = v.normal.x;
  out[1] = v.normal.y;
  out[2] = v.normal.z;
}

void texCoord(const SMikkTSpaceContext *context, float out[], int face, int vert) {
  const Vertex &v = faceVertex(context, face, vert);
  out[0] = v.uvX;
  out[1] = v.uvY;
}

// MikkTSpace reports tangents per face corner; vertices shared between faces
// get the same tangent when the mesh is consistent, which glTF exporters
// guarantee by splitting vertices at UV seams.
void setTangent(const SMikkTSpaceContext *context, const float tangent[],
                float sign, int face, int vert) {
  Vertex &v = faceVertex(context, face, vert);
  v.tangent[0] = tangent[0];
  v.tangent[1] = tangent[1];
  v.tangent[2] = tangent[2];
  v.tangent[3] = sign;
}

void generateTangents(MeshData &mesh) {
  TangentFaces data{&mesh, {}};
  const auto same = [](const Vec3 &a, const Vec3 &b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
  for (size_t f = 0; f < mesh.indices.size() / 3; ++f) {
    const Vec3 &a = mesh.vertices[mesh.indices[f * 3]].position;
    const Vec3 &b = mesh.vertices[mesh.indices[f * 3 + 1]].position;
    const Vec3 &c = mesh.vertices[mesh.indices[f * 3 + 2]].position;
    if (!same(a, b) && !same(a, c) && !same(b, c)) {
      data.faces.push_back(static_cast<uint32_t>(f));
    }
  }

  SMikkTSpaceInterface interface{};
  interface.m_getNumFaces = faceCount;
  interface.m_getNumVerticesOfFace = verticesOfFace;
  interface.m_getPosition = position;
  interface.m_getNormal = normal;
  interface.m_getTexCoord = texCoord;
  interface.m_setTSpaceBasic = setTangent;

  SMikkTSpaceContext context{};
  context.m_pInterface = &interface;
  context.m_pUserData = &data;
  genTangSpaceDefault(&context);
}

// --- glTF data access

std::span<const std::byte> bufferBytes(const fastgltf::Asset &asset,
                                       const fastgltf::BufferView &view) {
  const auto &data = asset.buffers[view.bufferIndex].data;
  std::span<const std::byte> bytes;
  if (const auto *array = std::get_if<fastgltf::sources::Array>(&data)) {
    bytes = {array->bytes.data(), array->bytes.size()};
  } else if (const auto *vector = std::get_if<fastgltf::sources::Vector>(&data)) {
    bytes = {vector->bytes.data(), vector->bytes.size()};
  } else {
    throw std::runtime_error("glTF buffer was not loaded");
  }
  return bytes.subspan(view.byteOffset, view.byteLength);
}

std::vector<uint8_t> imageBytes(const fastgltf::Asset &asset,
                                const fastgltf::Image &image,
                                const std::filesystem::path &directory) {
  if (const auto *uri = std::get_if<fastgltf::sources::URI>(&image.data)) {
    std::ifstream stream(directory / uri->uri->fspath(),
                         std::ios::binary | std::ios::ate);
    if (!stream) {
      throw std::runtime_error("cannot open image " + std::string(uri->uri->path()));
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(stream.tellg()));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    return bytes;
  }
  if (const auto *view = std::get_if<fastgltf::sources::BufferView>(&image.data)) {
    auto bytes = bufferBytes(asset, asset.bufferViews[view->bufferViewIndex]);
    const auto *begin = reinterpret_cast<const uint8_t *>(bytes.data());
    return {begin, begin + bytes.size()};
  }
  throw std::runtime_error("unsupported glTF image source");
}

MeshData loadPrimitive(const fastgltf::Asset &asset,
                       const fastgltf::Primitive &primitive,
                       const UvTransform &uvTransform, bool normalMap) {
  const auto positionAttribute = primitive.findAttribute("POSITION");
  if (positionAttribute == primitive.attributes.cend() ||
      !primitive.indicesAccessor) {
    throw std::runtime_error("glTF primitive without positions or indices");
  }

  MeshData mesh;
  const auto &positions = asset.accessors[positionAttribute->accessorIndex];
  mesh.vertices.resize(positions.count);
  fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
      asset, positions, [&](fastgltf::math::fvec3 p, size_t i) {
        mesh.vertices[i].position = {p.x(), p.y(), p.z()};
      });

  if (const auto it = primitive.findAttribute("NORMAL");
      it != primitive.attributes.cend()) {
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
        asset, asset.accessors[it->accessorIndex],
        [&](fastgltf::math::fvec3 n, size_t i) {
          mesh.vertices[i].normal = {n.x(), n.y(), n.z()};
        });
  }
  if (const auto it = primitive.findAttribute("TEXCOORD_0");
      it != primitive.attributes.cend()) {
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
        asset, asset.accessors[it->accessorIndex],
        [&](fastgltf::math::fvec2 uv, size_t i) {
          mesh.vertices[i].uvX = uv.x();
          mesh.vertices[i].uvY = uv.y();
        });
    applyUvTransform(mesh, uvTransform);
  }

  // Only COLOR_0's alpha is kept (a masked material's alpha test multiplies
  // it in); a VEC3 COLOR_0 has none.
  if (const auto it = primitive.findAttribute("COLOR_0");
      it != primitive.attributes.cend() &&
      asset.accessors[it->accessorIndex].type == fastgltf::AccessorType::Vec4) {
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
        asset, asset.accessors[it->accessorIndex],
        [&](fastgltf::math::fvec4 c, size_t i) { mesh.vertices[i].alpha = c.w(); });
  }

  const auto &indices = asset.accessors[*primitive.indicesAccessor];
  mesh.indices.resize(indices.count);
  fastgltf::copyFromAccessor<uint32_t>(asset, indices, mesh.indices.data());

  if (const auto it = primitive.findAttribute("TANGENT");
      it != primitive.attributes.cend()) {
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
        asset, asset.accessors[it->accessorIndex],
        [&](fastgltf::math::fvec4 t, size_t i) {
          for (int c = 0; c < 4; ++c) {
            mesh.vertices[i].tangent[c] = t[c];
          }
        });
  } else if (normalMap &&
             primitive.findAttribute("NORMAL") != primitive.attributes.cend() &&
             primitive.findAttribute("TEXCOORD_0") != primitive.attributes.cend()) {
    generateTangents(mesh);
  }
  return mesh;
}

Mat4 toMat4(const fastgltf::math::fmat4x4 &m) {
  Mat4 result;
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      result.m[column][row] = m[column][row];
    }
  }
  return result;
}

// Uploads textures on first use. The same image can be sampled as color
// (sRGB) by one material and as data (linear) by another, so the color
// space is part of the key.
class TextureCache {
public:
  TextureCache(Renderer &renderer, const fastgltf::Asset &asset,
               std::filesystem::path directory)
      : renderer_(renderer), asset_(asset), directory_(std::move(directory)) {}

  std::optional<TextureId> get(const fastgltf::TextureInfo *info,
                               ColorSpace colorSpace) {
    if (info == nullptr) {
      return std::nullopt;
    }
    const fastgltf::Texture &texture = asset_.textures[info->textureIndex];
    if (!texture.basisuImageIndex) {
      throw std::runtime_error(
          "glTF texture is not KTX2; run the asset cooker on this model");
    }
    auto key = std::make_pair(*texture.basisuImageIndex, colorSpace);
    if (auto it = cache_.find(key); it != cache_.end()) {
      return it->second;
    }
    TextureId id = renderer_.uploadTexture(loadKtx2(
        imageBytes(asset_, asset_.images[*texture.basisuImageIndex], directory_),
        colorSpace));
    cache_.emplace(key, id);
    return id;
  }

private:
  Renderer &renderer_;
  const fastgltf::Asset &asset_;
  std::filesystem::path directory_;
  std::map<std::pair<size_t, ColorSpace>, TextureId> cache_;
};

template <typename T>
const fastgltf::TextureInfo *textureInfo(const T &optionalInfo) {
  return optionalInfo ? &*optionalInfo : nullptr;
}

// The one transform shared by all of a material's textures.
UvTransform materialUvTransform(const fastgltf::Material &m) {
  const fastgltf::TextureInfo *infos[] = {
      textureInfo(m.pbrData.baseColorTexture),
      textureInfo(m.pbrData.metallicRoughnessTexture),
      textureInfo(m.normalTexture), textureInfo(m.occlusionTexture),
      textureInfo(m.emissiveTexture),
      m.diffuseTransmission ? textureInfo(m.diffuseTransmission->diffuseTransmissionColorTexture)
                            : nullptr};
  std::optional<UvTransform> shared;
  for (const fastgltf::TextureInfo *info : infos) {
    if (!info) {
      continue;
    }
    UvTransform transform;
    if (info->transform) {
      if (info->transform->texCoordIndex.value_or(0) != 0) {
        throw std::runtime_error("texture transform selects another TEXCOORD set");
      }
      transform = {{info->transform->uvOffset.x(), info->transform->uvOffset.y()},
                   {info->transform->uvScale.x(), info->transform->uvScale.y()},
                   info->transform->rotation};
    }
    const auto same = [](const UvTransform &a, const UvTransform &b) {
      return a.offset[0] == b.offset[0] && a.offset[1] == b.offset[1] &&
             a.scale[0] == b.scale[0] && a.scale[1] == b.scale[1] &&
             a.rotation == b.rotation;
    };
    if (shared && !same(*shared, transform)) {
      throw std::runtime_error("material " + std::string(m.name) +
                               " has different texture transforms per texture");
    }
    shared = transform;
  }
  return shared.value_or(UvTransform{});
}

// glTF emissive is relative; with KHR_materials_emissive_strength it can
// exceed 1. We map 1.0 to the luminance of a bright display (1000 cd/m^2).
constexpr float kEmissiveLuminance = 1000.0f;


// What a voxelized model's primitives sample on the CPU: per glTF material
// (the last entry is the default material), and the texture bytes the cache
// key covers. Images are decoded only when the clusters are built.
struct VoxelInputs {
  std::vector<VoxelMaterial> materials;
  std::vector<uint32_t> primitiveMaterial;
  std::vector<std::vector<uint8_t>> textureBytes;
  std::vector<CpuImage> images; // decoded textureBytes, same order
  // Per material: indices into images for base colour, transmission colour,
  // metallic-roughness (-1: none).
  std::vector<std::array<int, 3>> imageOf;
};

// Voxels sample textures at this size at most (a leaf atlas texel is then a
// few millimetres to a centimetre, finer than any voxel).
constexpr uint32_t kVoxelTextureSize = 512;

// Fills every primitive's cluster hierarchy from the cache next to the
// glTF, building and caching them when it is missing or stale.
void loadClusters(const std::filesystem::path &gltf, std::vector<MeshData> &meshes,
                  VoxelInputs &voxels) {
  const uint64_t key = clusterSourceKey(meshes, voxels.textureBytes, voxels.materials,
                                        voxels.primitiveMaterial, voxels.imageOf);
  std::filesystem::path cache = gltf;
  cache.replace_extension(clusterCacheExtension());
  seedVoxels(meshes);
  auto cached = readClusterCache(cache, key);
  bool valid = cached && cached->size() == meshes.size();
  for (size_t i = 0; valid && i < meshes.size(); ++i) {
    valid = validClusterLod((*cached)[i], meshes[i].vertices.size());
  }
  if (valid) {
    for (size_t i = 0; i < meshes.size(); ++i) {
      meshes[i].clusters = std::move((*cached)[i]);
      appendClusterVertices(meshes[i]);
    }
    return;
  }
  std::printf("Building clusters for %s\n", gltf.string().c_str());
  for (const std::vector<uint8_t> &bytes : voxels.textureBytes) {
    voxels.images.push_back(loadKtx2Rgba8(bytes, kVoxelTextureSize));
  }
  for (size_t m = 0; m < voxels.imageOf.size(); ++m) {
    const auto image = [&](int i) { return i < 0 ? nullptr : &voxels.images[size_t(i)]; };
    voxels.materials[m].baseColor = image(voxels.imageOf[m][0]);
    voxels.materials[m].transmissionColorImage = image(voxels.imageOf[m][1]);
    voxels.materials[m].metallicRoughness = image(voxels.imageOf[m][2]);
  }
  std::vector<ClusterLod> lods;
  for (size_t i = 0; i < meshes.size(); ++i) {
    const VoxelSource source{voxels.materials,
                             voxels.primitiveMaterial.empty() ? 0 : voxels.primitiveMaterial[i]};
    meshes[i].clusters = buildClusterLod(meshes[i], &source);
    lods.push_back(meshes[i].clusters);
  }
  bakeModelAxisDepths(lods);
  for (size_t i = 0; i < meshes.size(); ++i) {
    meshes[i].clusters = lods[i];
  }
  for (MeshData &mesh : meshes) {
    appendClusterVertices(mesh);
  }
  // The cache only saves time: a model still loads where it can't be written.
  try {
    writeClusterCache(cache, key, lods);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "warning: %s\n", error.what());
  }
}

} // namespace

// The rotation sign follows the extension's worked example (and three.js);
// the matrix in its sample shader rotates the other way.
void applyUvTransform(MeshData &mesh, const UvTransform &t) {
  const float c = std::cos(t.rotation);
  const float s = std::sin(t.rotation);
  for (Vertex &v : mesh.vertices) {
    const float u = v.uvX * t.scale[0];
    const float w = v.uvY * t.scale[1];
    v.uvX = c * u + s * w + t.offset[0];
    v.uvY = -s * u + c * w + t.offset[1];
  }
}

Model loadGltf(Renderer &renderer, const std::filesystem::path &path) {
  fastgltf::Parser parser(fastgltf::Extensions::KHR_texture_basisu |
                          fastgltf::Extensions::KHR_materials_emissive_strength |
                          fastgltf::Extensions::KHR_texture_transform |
                          fastgltf::Extensions::KHR_materials_diffuse_transmission);
  auto data = fastgltf::GltfDataBuffer::FromPath(path);
  if (data.error() != fastgltf::Error::None) {
    throw std::runtime_error("cannot read " + path.string());
  }
  const std::filesystem::path directory = path.parent_path();
  auto loaded = parser.loadGltf(data.get(), directory,
                                fastgltf::Options::LoadExternalBuffers);
  if (loaded.error() != fastgltf::Error::None) {
    throw std::runtime_error("invalid glTF " + path.string() + ": " +
                             std::string(fastgltf::getErrorMessage(loaded.error())));
  }
  fastgltf::Asset &asset = loaded.get();

  TextureCache textures(renderer, asset, directory);
  std::vector<MaterialId> materials;
  for (const fastgltf::Material &m : asset.materials) {
    MaterialData material;
    for (int c = 0; c < 4; ++c) {
      material.baseColorFactor[c] = m.pbrData.baseColorFactor[c];
    }
    material.metallicFactor = m.pbrData.metallicFactor;
    material.roughnessFactor = m.pbrData.roughnessFactor;
    const float emissive = m.emissiveStrength * kEmissiveLuminance;
    material.emissiveFactor = {m.emissiveFactor.x() * emissive,
                               m.emissiveFactor.y() * emissive,
                               m.emissiveFactor.z() * emissive};
    material.baseColorTexture =
        textures.get(textureInfo(m.pbrData.baseColorTexture), ColorSpace::Srgb);
    material.metallicRoughnessTexture = textures.get(
        textureInfo(m.pbrData.metallicRoughnessTexture), ColorSpace::Linear);
    if (m.normalTexture) {
      material.normalScale = m.normalTexture->scale;
      material.normalTexture =
          textures.get(&*m.normalTexture, ColorSpace::Linear);
    }
    if (m.occlusionTexture) {
      material.occlusionStrength = m.occlusionTexture->strength;
      material.occlusionTexture =
          textures.get(&*m.occlusionTexture, ColorSpace::Linear);
    }
    material.emissiveTexture =
        textures.get(textureInfo(m.emissiveTexture), ColorSpace::Srgb);
    switch (m.alphaMode) {
    case fastgltf::AlphaMode::Opaque:
      material.alphaMode = AlphaMode::Opaque;
      break;
    case fastgltf::AlphaMode::Mask:
      material.alphaMode = AlphaMode::Mask;
      break;
    case fastgltf::AlphaMode::Blend:
      material.alphaMode = AlphaMode::Blend;
      break;
    }
    material.alphaCutoff = m.alphaCutoff;
    material.doubleSided = m.doubleSided;
    if (m.diffuseTransmission) {
      material.diffuseTransmission = m.diffuseTransmission->diffuseTransmissionFactor;
      const auto &color = m.diffuseTransmission->diffuseTransmissionColorFactor;
      material.diffuseTransmissionColor = {color.x(), color.y(), color.z()};
      material.diffuseTransmissionColorTexture = textures.get(
          textureInfo(m.diffuseTransmission->diffuseTransmissionColorTexture), ColorSpace::Srgb);
    }
    materials.push_back(renderer.createMaterial(material));
    renderer.setMaterialName(materials.back(), std::string(m.name));
  }
  const MaterialId defaultMaterial = renderer.createMaterial({});

  // Each glTF mesh may hold several primitives; upload each once and
  // instance it for every node that references the mesh.
  std::vector<MeshData> primitives;
  for (const fastgltf::Mesh &mesh : asset.meshes) {
    for (const fastgltf::Primitive &primitive : mesh.primitives) {
      const fastgltf::Material *material =
          primitive.materialIndex ? &asset.materials[*primitive.materialIndex] : nullptr;
      const UvTransform uvTransform =
          material ? materialUvTransform(*material) : UvTransform{};
      primitives.push_back(loadPrimitive(asset, primitive, uvTransform,
                                         material && material->normalTexture.has_value()));
      primitives.back().leaf =
          material && keepsLeafArea(material->alphaMode == fastgltf::AlphaMode::Opaque,
                                    material->doubleSided,
                                    material->diffuseTransmission
                                        ? material->diffuseTransmission->diffuseTransmissionFactor
                                        : 0.0f);
    }
  }
  VoxelInputs voxels;
  {
    std::vector<uint8_t> foliage;
    for (const fastgltf::Mesh &mesh : asset.meshes) {
      for (const fastgltf::Primitive &primitive : mesh.primitives) {
        const fastgltf::Material *m =
            primitive.materialIndex ? &asset.materials[*primitive.materialIndex] : nullptr;
        // A mask cut by vertex alpha is a fade on a solid part (a trunk
        // base), not foliage.
        const std::vector<Vertex> &vertices = primitives[foliage.size()].vertices;
        const bool fades = std::any_of(vertices.begin(), vertices.end(),
                                       [](const Vertex &v) { return v.alpha < 1.0f; });
        foliage.push_back(m && ((m->diffuseTransmission &&
                                 m->diffuseTransmission->diffuseTransmissionFactor > 0.0f) ||
                                (m->alphaMode == fastgltf::AlphaMode::Mask && !fades)));
        voxels.primitiveMaterial.push_back(
            primitive.materialIndex ? uint32_t(*primitive.materialIndex)
                                    : uint32_t(asset.materials.size()));
      }
    }
    if (voxelizesModel(primitives, foliage)) {
      std::map<size_t, int> imageIndex;
      const auto image = [&](const fastgltf::TextureInfo *info) {
        if (!info || !asset.textures[info->textureIndex].basisuImageIndex) {
          return -1;
        }
        const size_t source = *asset.textures[info->textureIndex].basisuImageIndex;
        const auto [it, inserted] = imageIndex.emplace(source, int(voxels.textureBytes.size()));
        if (inserted) {
          voxels.textureBytes.push_back(imageBytes(asset, asset.images[source], directory));
        }
        return it->second;
      };
      for (const fastgltf::Material &m : asset.materials) {
        VoxelMaterial v;
        v.baseColorFactor = {m.pbrData.baseColorFactor[0], m.pbrData.baseColorFactor[1],
                             m.pbrData.baseColorFactor[2]};
        v.alphaMask = m.alphaMode == fastgltf::AlphaMode::Mask;
        v.alphaCutoff = m.alphaCutoff;
        v.roughnessFactor = m.pbrData.roughnessFactor;
        std::array<int, 3> images = {image(textureInfo(m.pbrData.baseColorTexture)), -1,
                                     image(textureInfo(m.pbrData.metallicRoughnessTexture))};
        if (m.diffuseTransmission) {
          v.diffuseTransmission = m.diffuseTransmission->diffuseTransmissionFactor;
          const auto &color = m.diffuseTransmission->diffuseTransmissionColorFactor;
          v.transmissionColor = {color.x(), color.y(), color.z()};
          images[1] = image(textureInfo(m.diffuseTransmission->diffuseTransmissionColorTexture));
        }
        voxels.materials.push_back(v);
        voxels.imageOf.push_back(images);
      }
      voxels.materials.push_back({});
      voxels.imageOf.push_back({-1, -1, -1});
      float lo = FLT_MAX, hi = -FLT_MAX;
      for (const MeshData &primitive : primitives) {
        for (const Vertex &v : primitive.vertices) {
          lo = std::min(lo, v.position.y);
          hi = std::max(hi, v.position.y);
        }
      }
      for (size_t i = 0; i < primitives.size(); ++i) {
        primitives[i].voxelize = true;
        // Solid parts take the model's finest voxel; foliage keeps its own,
        // which sets where the crown switches.
        primitives[i].voxelModelHeight = foliage[i] ? 0.0f : hi - lo;
      }
    } else {
      voxels.primitiveMaterial.clear();
    }
  }
  loadClusters(path, primitives, voxels);
  std::vector<std::vector<MeshId>> meshes(asset.meshes.size());
  size_t next = 0;
  for (size_t i = 0; i < asset.meshes.size(); ++i) {
    for (const fastgltf::Primitive &primitive : asset.meshes[i].primitives) {
      MaterialId material = primitive.materialIndex
                                ? materials[*primitive.materialIndex]
                                : defaultMaterial;
      meshes[i].push_back(renderer.uploadMesh(primitives[next++], material));
      renderer.setMeshName(meshes[i].back(), path.filename().string() + "/" +
                                                 std::string(asset.meshes[i].name) + "#" +
                                                 std::to_string(meshes[i].size() - 1));
    }
  }

  Model model;
  const size_t scene = asset.defaultScene.value_or(0);
  fastgltf::iterateSceneNodes(
      asset, scene, fastgltf::math::fmat4x4(),
      [&](fastgltf::Node &node, const fastgltf::math::fmat4x4 &transform) {
        if (!node.meshIndex) {
          return;
        }
        for (MeshId mesh : meshes[*node.meshIndex]) {
          model.drawItems.push_back({mesh, toMat4(transform)});
        }
      });
  return model;
}

} // namespace meridian
