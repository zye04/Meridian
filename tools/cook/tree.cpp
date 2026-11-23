// Tree cook: simplifies each prototype of an unflattened tree once, so the
// flattened tree lands within a triangle budget, then flattens it into one
// mesh per material.
//
// Wood is simplified as a whole mesh. Leaves are disjoint pieces, so each
// piece is simplified on its own; if the tree is still over budget, whole
// leaf pieces are pruned in random order and the survivors scaled up so the
// total leaf area stays the same (stochastic pruning, Cook et al. 2007). The
// pieces holding the tree's extreme points are always kept and never scaled,
// and no piece is scaled past the tree's bounds, so the bounds don't move.

#include "tree.h"

#include "ktx2.h"
#include "leaf_trace.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

namespace meridian::cook {
namespace {

using Vec3 = std::array<float, 3>;

using Vertex = CardVertex;

// One glTF primitive of a prototype mesh.
struct Prototype {
  size_t material = 0;
  bool leaf = false;
  // The material has a base color texture: the source UVs are kept.
  bool textured = false;
  std::vector<Vertex> vertices;
  // Leaves: triangles grouped by piece; pieces[i]..pieces[i + 1] is piece
  // i's index range.
  std::vector<uint32_t> indices, pieces;
  std::vector<uint8_t> locked;
  size_t instances = 0;
  // Simplified, in the same layout as indices/pieces.
  std::vector<uint32_t> cooked, cookedPieces;
};

struct Instance {
  size_t prototype;
  fastgltf::math::fmat4x4 matrix;
};

// Absolute simplification error limits in meters (prototype space): wood
// silhouettes move by at most a centimeter, leaf outlines by 2 mm, so a 5-8 cm
// leaf keeps its shape up close (the cluster LOD coarsens it with distance).
// A tree over budget at 2 mm coarsens its leaf outlines up to 5 mm before it
// prunes leaves: survivors scaled up read as oversized leaves, far more than a
// slightly polygonal edge does.
constexpr float kWoodSimplifyError = 0.01f;
constexpr float kLeafSimplifyErrors[] = {0.002f, 0.003f, 0.004f, 0.005f};
constexpr float kNormalWeight = 0.5f;
// Textured meshes also weigh UVs, scaled by the piece's texel density (surface
// length per UV unit): a UV error costs as much as the position error of the
// same length on the surface, so the texture slides no further across the
// surface than the outline moves.
constexpr float kUvWeight = 1.0f;
// Thin leaves transmit about as much light as they reflect diffusely; kept
// below that so reflectance + transmittance stays under 1.
constexpr float kLeafDiffuseTransmission = 0.35f;
// Leaf tint range around the species color: hue in turns, value as a factor.
constexpr float kLeafHueJitter = 0.03f;
constexpr float kLeafValueJitter = 0.08f;

Vec3 transformPoint(const fastgltf::math::fmat4x4 &m, const Vec3 &p) {
  Vec3 out;
  for (int r = 0; r < 3; ++r) {
    out[r] = m[0][r] * p[0] + m[1][r] * p[1] + m[2][r] * p[2] + m[3][r];
  }
  return out;
}

Vec3 sub(const Vec3 &a, const Vec3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

Vec3 cross(const Vec3 &a, const Vec3 &b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

float length(const Vec3 &v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

Vec3 normalize(const Vec3 &v) {
  const float l = length(v);
  return l > 0.0f ? Vec3{v[0] / l, v[1] / l, v[2] / l} : Vec3{0.0f, 1.0f, 0.0f};
}

double triangleArea(const Vec3 &a, const Vec3 &b, const Vec3 &c) {
  return 0.5 * length(cross(sub(b, a), sub(c, a)));
}

uint64_t hash(uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

uint64_t pieceHash(size_t instance, size_t piece, uint64_t salt) {
  return hash(hash(instance ^ salt) ^ piece);
}

// Smooth normals over position-welded vertices, for meshes without any.
void computeNormals(Prototype &p) {
  std::vector<uint32_t> shadow(p.indices.size());
  meshopt_generateShadowIndexBuffer(shadow.data(), p.indices.data(), p.indices.size(),
                                    p.vertices.data(), p.vertices.size(), sizeof(Vec3),
                                    sizeof(Vertex));
  std::vector<Vec3> sums(p.vertices.size(), Vec3{});
  for (size_t i = 0; i < shadow.size(); i += 3) {
    const Vec3 n = cross(sub(p.vertices[shadow[i + 1]].position, p.vertices[shadow[i]].position),
                         sub(p.vertices[shadow[i + 2]].position, p.vertices[shadow[i]].position));
    for (int k = 0; k < 3; ++k) {
      for (int c = 0; c < 3; ++c) {
        sums[shadow[i + k]][c] += n[c];
      }
    }
  }
  for (size_t i = 0; i < shadow.size(); ++i) {
    p.vertices[p.indices[i]].normal = normalize(sums[shadow[i]]);
  }
}

// Untextured leaves keep no UVs (the cook assigns palette UVs), so welding
// them drops UV seams that would stop simplification; then triangles are
// grouped into position-connected pieces.
void splitLeafPieces(Prototype &p) {
  if (!p.textured) {
    for (Vertex &v : p.vertices) {
      v.uv[0] = v.uv[1] = 0.0f;
    }
  }
  std::vector<uint32_t> remap(p.vertices.size());
  const size_t unique =
      meshopt_generateVertexRemap(remap.data(), p.indices.data(), p.indices.size(),
                                  p.vertices.data(), p.vertices.size(), sizeof(Vertex));
  meshopt_remapIndexBuffer(p.indices.data(), p.indices.data(), p.indices.size(), remap.data());
  meshopt_remapVertexBuffer(p.vertices.data(), p.vertices.data(), p.vertices.size(),
                            sizeof(Vertex), remap.data());
  p.vertices.resize(unique);

  std::vector<uint32_t> shadow(p.indices.size());
  meshopt_generateShadowIndexBuffer(shadow.data(), p.indices.data(), p.indices.size(),
                                    p.vertices.data(), p.vertices.size(), sizeof(Vec3),
                                    sizeof(Vertex));
  std::vector<uint32_t> parent(p.vertices.size());
  std::iota(parent.begin(), parent.end(), 0u);
  auto root = [&](uint32_t i) {
    while (parent[i] != i) {
      i = parent[i] = parent[parent[i]];
    }
    return i;
  };
  for (size_t i = 0; i < p.indices.size(); ++i) {
    parent[root(p.indices[i])] = root(shadow[i]);
    if (i % 3) {
      parent[root(p.indices[i])] = root(p.indices[i - i % 3]);
    }
  }
  std::vector<uint32_t> roots(parent.size());
  for (uint32_t i = 0; i < roots.size(); ++i) {
    roots[i] = root(i);
  }
  std::vector<uint32_t> triangles(p.indices.size() / 3);
  std::iota(triangles.begin(), triangles.end(), 0u);
  std::stable_sort(triangles.begin(), triangles.end(), [&](uint32_t a, uint32_t b) {
    return roots[p.indices[3 * a]] < roots[p.indices[3 * b]];
  });
  std::vector<uint32_t> sorted;
  sorted.reserve(p.indices.size());
  p.pieces.clear();
  for (uint32_t t : triangles) {
    if (p.pieces.empty() || roots[p.indices[3 * t]] != roots[sorted[sorted.size() - 3]]) {
      p.pieces.push_back(static_cast<uint32_t>(sorted.size()));
    }
    sorted.insert(sorted.end(), &p.indices[3 * t], &p.indices[3 * t] + 3);
  }
  p.pieces.push_back(static_cast<uint32_t>(sorted.size()));
  p.indices = std::move(sorted);
}

} // namespace

bool isLeafMaterial(const fastgltf::Material &material) {
  if (material.doubleSided || material.alphaMode == fastgltf::AlphaMode::Mask) {
    return true;
  }
  std::string name(material.name);
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const char *word : {"leaf", "leaves", "foliage", "needle", "twosided", "two_sided"}) {
    if (name.find(word) != std::string::npos) {
      return true;
    }
  }
  return false;
}

namespace {

// With `atlas`, the primitive's triangles are alpha-tested cards, replaced by
// the atlas's traced leaves clipped onto them.
Prototype loadPrototype(const fastgltf::Asset &asset, const fastgltf::Primitive &primitive,
                        const AtlasMesh *atlas) {
  const auto position = primitive.findAttribute("POSITION");
  if (position == primitive.attributes.cend()) {
    throw std::runtime_error("tree primitive without positions");
  }
  Prototype p;
  p.material = primitive.materialIndex.value_or(0);
  if (p.material >= asset.materials.size()) {
    throw std::runtime_error("tree primitive without a material");
  }
  p.leaf = isLeafMaterial(asset.materials[p.material]);
  p.textured = asset.materials[p.material].pbrData.baseColorTexture.has_value();
  const auto &positions = asset.accessors[position->accessorIndex];
  p.vertices.resize(positions.count, Vertex{});
  fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
      asset, positions, [&](fastgltf::math::fvec3 v, size_t i) {
        p.vertices[i].position = {v.x(), v.y(), v.z()};
      });
  const auto normal = primitive.findAttribute("NORMAL");
  if (normal != primitive.attributes.cend()) {
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
        asset, asset.accessors[normal->accessorIndex], [&](fastgltf::math::fvec3 v, size_t i) {
          p.vertices[i].normal = {v.x(), v.y(), v.z()};
        });
  }
  const auto uv = primitive.findAttribute("TEXCOORD_0");
  if (p.textured && uv == primitive.attributes.cend()) {
    throw std::runtime_error("a primitive of textured material " +
                             std::string(asset.materials[p.material].name) +
                             " has no TEXCOORD_0");
  }
  if (uv != primitive.attributes.cend()) {
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
        asset, asset.accessors[uv->accessorIndex], [&](fastgltf::math::fvec2 v, size_t i) {
          p.vertices[i].uv[0] = v.x();
          p.vertices[i].uv[1] = v.y();
        });
  }
  if (primitive.indicesAccessor) {
    const auto &indices = asset.accessors[*primitive.indicesAccessor];
    p.indices.resize(indices.count);
    fastgltf::copyFromAccessor<uint32_t>(asset, indices, p.indices.data());
  } else {
    p.indices.resize(p.vertices.size());
    std::iota(p.indices.begin(), p.indices.end(), 0u);
  }
  if (normal == primitive.attributes.cend()) {
    computeNormals(p);
  }
  if (atlas) {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    clipToCards(*atlas, p.vertices, p.indices, vertices, indices);
    std::printf("Traced leaf material %s: %zu card triangles -> %zu triangles\n",
                std::string(asset.materials[p.material].name).c_str(), p.indices.size() / 3,
                indices.size() / 3);
    p.vertices = std::move(vertices);
    p.indices = std::move(indices);
  }
  if (p.leaf) {
    splitLeafPieces(p);
  } else {
    p.pieces = {0, static_cast<uint32_t>(p.indices.size())};
  }
  p.locked.assign(p.vertices.size(), 0);
  return p;
}

// Each piece is simplified on its own compact vertex buffer: meshoptimizer's
// cost scales with the buffer it is given, and a prototype holds many pieces.
void simplify(Prototype &p, double ratio, float leafError) {
  p.cooked.clear();
  p.cookedPieces.clear();
  float weights[5] = {kNormalWeight, kNormalWeight, kNormalWeight, 0.0f, 0.0f};
  const size_t attributes = p.textured ? 5 : 3;
  std::vector<uint32_t> local(p.vertices.size(), UINT32_MAX), global, indices, out;
  std::vector<Vertex> vertices;
  std::vector<uint8_t> locked;
  for (size_t piece = 0; piece + 1 < p.pieces.size(); ++piece) {
    p.cookedPieces.push_back(static_cast<uint32_t>(p.cooked.size()));
    const auto begin = p.indices.begin() + p.pieces[piece];
    const auto end = p.indices.begin() + p.pieces[piece + 1];
    const size_t count = static_cast<size_t>(end - begin);
    const size_t target = std::max<size_t>(1, std::lround(count / 3 * ratio)) * 3;
    if (target >= count) {
      p.cooked.insert(p.cooked.end(), begin, end);
      continue;
    }
    // Local vertices keep the prototype's order, so results match a
    // simplification against the whole buffer.
    global.assign(begin, end);
    std::sort(global.begin(), global.end());
    global.erase(std::unique(global.begin(), global.end()), global.end());
    vertices.clear();
    locked.clear();
    for (uint32_t i = 0; i < global.size(); ++i) {
      local[global[i]] = i;
      vertices.push_back(p.vertices[global[i]]);
      locked.push_back(p.locked[global[i]]);
    }
    indices.clear();
    for (auto it = begin; it != end; ++it) {
      indices.push_back(local[*it]);
    }
    if (p.textured) {
      // meshoptimizer normalizes positions by the buffer's largest extent.
      Vec3 low{INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY};
      for (const Vertex &v : vertices) {
        for (int c = 0; c < 3; ++c) {
          low[c] = std::min(low[c], v.position[c]);
          high[c] = std::max(high[c], v.position[c]);
        }
      }
      const float extent = std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]});
      double area = 0.0, uvArea = 0.0;
      for (size_t t = 0; t < count; t += 3) {
        const Vertex &a = vertices[indices[t]], &b = vertices[indices[t + 1]],
                     &c = vertices[indices[t + 2]];
        area += triangleArea(a.position, b.position, c.position);
        uvArea += 0.5 * std::abs((b.uv[0] - a.uv[0]) * (c.uv[1] - a.uv[1]) -
                                 (c.uv[0] - a.uv[0]) * (b.uv[1] - a.uv[1]));
      }
      // Degenerate UVs (one texel for the whole piece) have nothing to keep.
      const bool mapped = uvArea > 1e-12 && extent > 0.0f;
      weights[3] = weights[4] =
          mapped ? kUvWeight * static_cast<float>(std::sqrt(area / uvArea)) / extent : 0.0f;
    }
    out.resize(count);
    // Normal and UV are adjacent in Vertex, so one attribute stream holds both.
    static_assert(offsetof(Vertex, uv) == offsetof(Vertex, normal) + sizeof(Vec3));
    const size_t kept = meshopt_simplifyWithAttributes(
        out.data(), indices.data(), count, vertices[0].position.data(), vertices.size(),
        sizeof(Vertex), vertices[0].normal.data(), sizeof(Vertex), weights, attributes,
        locked.data(), target, p.leaf ? leafError : kWoodSimplifyError,
        meshopt_SimplifyErrorAbsolute, nullptr);
    // A piece is never simplified away; pruning decides which go.
    if (kept == 0) {
      p.cooked.insert(p.cooked.end(), begin, end);
      continue;
    }
    for (size_t k = 0; k < kept; ++k) {
      p.cooked.push_back(global[out[k]]);
    }
  }
  p.cookedPieces.push_back(static_cast<uint32_t>(p.cooked.size()));
}

size_t cookedTriangles(const std::vector<Prototype> &prototypes, bool leaves) {
  size_t total = 0;
  for (const Prototype &p : prototypes) {
    if (p.leaf == leaves) {
      total += p.cooked.size() / 3 * p.instances;
    }
  }
  return total;
}

struct Bounds {
  Vec3 min{INFINITY, INFINITY, INFINITY}, max{-INFINITY, -INFINITY, -INFINITY};
};

// A leaf piece of one instance, after simplification.
struct LeafPiece {
  size_t instance, piece, triangles;
  double area;
  Vec3 centroid;
  Bounds bounds;
  uint64_t order;
  bool fixed;
  float scale = 1.0f;
};

void rgbToHsv(const float rgb[3], float hsv[3]) {
  const float max = std::max({rgb[0], rgb[1], rgb[2]});
  const float min = std::min({rgb[0], rgb[1], rgb[2]});
  const float delta = max - min;
  float h = 0.0f;
  if (delta > 0.0f) {
    if (max == rgb[0]) {
      h = std::fmod((rgb[1] - rgb[2]) / delta + 6.0f, 6.0f);
    } else if (max == rgb[1]) {
      h = (rgb[2] - rgb[0]) / delta + 2.0f;
    } else {
      h = (rgb[0] - rgb[1]) / delta + 4.0f;
    }
  }
  hsv[0] = h / 6.0f;
  hsv[1] = max > 0.0f ? delta / max : 0.0f;
  hsv[2] = max;
}

void hsvToRgb(const float hsv[3], float rgb[3]) {
  const float h = (hsv[0] - std::floor(hsv[0])) * 6.0f;
  for (int c = 0; c < 3; ++c) {
    const float k = std::fmod(5.0f - 2.0f * static_cast<float>(c) + h, 6.0f);
    rgb[c] = hsv[2] - hsv[2] * hsv[1] * std::clamp(std::min(k, 4.0f - k), 0.0f, 1.0f);
  }
}

// Tints of the species leaf color: a few percent of hue and brightness.
basisu::image leafPalette(const float linear[3]) {
  float hsv[3];
  rgbToHsv(linear, hsv);
  basisu::image image(kLeafPaletteSize, kLeafPaletteSize);
  for (uint32_t y = 0; y < kLeafPaletteSize; ++y) {
    for (uint32_t x = 0; x < kLeafPaletteSize; ++x) {
      const uint64_t h = hash(y * kLeafPaletteSize + x);
      const float a = static_cast<float>(h & 0xffff) / 65535.0f * 2.0f - 1.0f;
      const float b = static_cast<float>((h >> 16) & 0xffff) / 65535.0f * 2.0f - 1.0f;
      const float tint[3] = {hsv[0] + kLeafHueJitter * a, hsv[1],
                             hsv[2] * (1.0f + kLeafValueJitter * b)};
      float rgb[3];
      hsvToRgb(tint, rgb);
      image(x, y).set(linearToSrgb8(rgb[0]), linearToSrgb8(rgb[1]), linearToSrgb8(rgb[2]), 255);
    }
  }
  return image;
}

std::filesystem::path barkMap(const std::filesystem::path &dir, const std::string &suffix) {
  for (const auto &entry : std::filesystem::directory_iterator(dir)) {
    const std::string name = entry.path().filename().string();
    if (name.ends_with(suffix)) {
      return entry.path();
    }
  }
  throw std::runtime_error("no *" + suffix + " in " + dir.string());
}

class Writer {
public:
  size_t addAccessor(const void *data, size_t bytes, size_t count, fastgltf::AccessorType type,
                     fastgltf::ComponentType component, fastgltf::BufferTarget target) {
    while (bytes_.size() % 4) {
      bytes_.push_back(std::byte{0});
    }
    fastgltf::BufferView view;
    view.bufferIndex = 0;
    view.byteOffset = bytes_.size();
    view.byteLength = bytes;
    view.target = target;
    const auto *begin = static_cast<const std::byte *>(data);
    bytes_.insert(bytes_.end(), begin, begin + bytes);
    asset.bufferViews.push_back(std::move(view));
    fastgltf::Accessor accessor;
    accessor.bufferViewIndex = asset.bufferViews.size() - 1;
    accessor.count = count;
    accessor.type = type;
    accessor.componentType = component;
    asset.accessors.push_back(std::move(accessor));
    return asset.accessors.size() - 1;
  }

  size_t addTexture(std::vector<uint8_t> ktx2, const std::string &name) {
    fastgltf::sources::Vector source;
    source.bytes.resize(ktx2.size());
    std::memcpy(source.bytes.data(), ktx2.data(), ktx2.size());
    source.mimeType = fastgltf::MimeType::KTX2;
    fastgltf::Image image;
    image.data = std::move(source);
    image.name = name;
    asset.images.push_back(std::move(image));
    fastgltf::Texture texture;
    texture.basisuImageIndex = asset.images.size() - 1;
    asset.textures.push_back(std::move(texture));
    return asset.textures.size() - 1;
  }

  void write(const std::filesystem::path &target) {
    fastgltf::Buffer buffer;
    buffer.byteLength = bytes_.size();
    fastgltf::sources::Vector source;
    source.bytes = std::move(bytes_);
    source.mimeType = fastgltf::MimeType::GltfBuffer;
    buffer.data = std::move(source);
    buffer.name = target.stem().string();
    asset.buffers.push_back(std::move(buffer));
    if (!asset.textures.empty()) {
      asset.extensionsUsed.emplace_back("KHR_texture_basisu");
      asset.extensionsRequired.emplace_back("KHR_texture_basisu");
    }
    if (std::any_of(asset.materials.begin(), asset.materials.end(),
                    [](const fastgltf::Material &m) { return m.diffuseTransmission.has_value(); })) {
      asset.extensionsUsed.emplace_back("KHR_materials_diffuse_transmission");
    }
    const auto transformed = [](const auto &info) { return info && info->transform; };
    if (std::any_of(asset.materials.begin(), asset.materials.end(),
                    [&](const fastgltf::Material &m) {
                      return transformed(m.pbrData.baseColorTexture) ||
                             transformed(m.pbrData.metallicRoughnessTexture) ||
                             transformed(m.normalTexture) || transformed(m.occlusionTexture);
                    })) {
      asset.extensionsUsed.emplace_back("KHR_texture_transform");
    }
    fastgltf::FileExporter exporter;
    exporter.setBufferPath("buffers");
    exporter.setImagePath("textures");
    if (exporter.writeGltfJson(asset, target) != fastgltf::Error::None) {
      throw std::runtime_error("cannot write " + target.string());
    }
  }

  fastgltf::Asset asset;

private:
  std::vector<std::byte> bytes_;
};

// A textured leaf whose base color alpha is an alpha-test mask: its cards are
// traced into opaque leaves. An opaque source's alpha may hold something else
// (gloss, a packed mask).
bool tracedMaterial(const fastgltf::Material &material, bool leaf, const basisu::image &color) {
  return leaf && color.has_alpha() && material.alphaMode != fastgltf::AlphaMode::Opaque;
}

float alphaCutoff(const fastgltf::Material &material) {
  return material.alphaMode == fastgltf::AlphaMode::Mask ? material.alphaCutoff : 0.5f;
}

// The source's leaf transmission t, as the source tools write it (Unreal's
// translucency, tools/unreal/cook_ready.py): the back transmits t times the
// base colour. Its texture is averaged over the opaque texels, as the engine
// has no transmission factor map.
template <typename SourceImage>
float sourceLeafTransmission(const fastgltf::Material &from, const std::string &name,
                             const basisu::image &color, bool traced, float cutoff,
                             SourceImage &&sourceImage) {
  if (!from.diffuseTransmission) {
    return kLeafDiffuseTransmission;
  }
  float transmission = from.diffuseTransmission->diffuseTransmissionFactor;
  if (const auto &texture = from.diffuseTransmission->diffuseTransmissionTexture) {
    const basisu::image &map = sourceImage(texture->textureIndex);
    const bool sameSize = map.get_width() == color.get_width() &&
                          map.get_height() == color.get_height();
    if (traced && !sameSize) {
      std::printf("%s: transmission map and base color differ in size; averaging the whole "
                  "map\n", name.c_str());
    }
    double sum = 0.0;
    size_t opaque = 0;
    for (uint32_t i = 0; i < map.get_total_pixels(); ++i) {
      if (!traced || !sameSize || color.get_ptr()[i].a >= cutoff * 255.0f) {
        sum += map.get_ptr()[i].a / 255.0;
        ++opaque;
      }
    }
    transmission *= opaque ? static_cast<float>(sum / opaque) : 0.0f;
  }
  return transmission;
}

// A textured material's maps, re-encoded. A leaf's transmission becomes
// KHR_materials_diffuse_transmission (khrLeafTransmission): its base color
// texture, scaled, is also the transmission color texture.
template <typename SourceImage, typename CookTexture>
void cookTexturedMaterial(const fastgltf::Material &from, bool leaf, const std::string &name,
                          fastgltf::Material &material, SourceImage &&sourceImage,
                          CookTexture &&cookTexture) {
  const basisu::image &color = sourceImage(from.pbrData.baseColorTexture->textureIndex);
  const bool traced = tracedMaterial(from, leaf, color);
  const float cutoff = alphaCutoff(from);
  const float transmission =
      leaf ? sourceLeafTransmission(from, name, color, traced, cutoff, sourceImage) : 0.0f;
  const KhrLeafTransmission khr = khrLeafTransmission(transmission);
  material.pbrData.baseColorTexture = cookTexture(*from.pbrData.baseColorTexture, Usage::Color,
                                                  name + "_color", khr.colorScale);
  if (from.pbrData.metallicRoughnessTexture) {
    material.pbrData.metallicRoughnessTexture =
        cookTexture(*from.pbrData.metallicRoughnessTexture, Usage::Data, name + "_orm", 1.0f);
  }
  if (from.occlusionTexture) {
    fastgltf::OcclusionTextureInfo occlusion;
    static_cast<fastgltf::TextureInfo &>(occlusion) =
        cookTexture(*from.occlusionTexture, Usage::Data, name + "_occlusion", 1.0f);
    occlusion.strength = from.occlusionTexture->strength;
    material.occlusionTexture = std::move(occlusion);
  }
  if (from.normalTexture) {
    fastgltf::NormalTextureInfo normal;
    static_cast<fastgltf::TextureInfo &>(normal) =
        cookTexture(*from.normalTexture, Usage::Normal, name + "_normal", 1.0f);
    normal.scale = from.normalTexture->scale;
    material.normalTexture = std::move(normal);
  }
  if (!leaf) {
    std::printf("Textured wood material %s\n", name.c_str());
    return;
  }
  material.diffuseTransmission = fastgltf::MaterialDiffuseTransmission{};
  material.diffuseTransmission->diffuseTransmissionFactor = khr.factor;
  material.diffuseTransmission->diffuseTransmissionColorTexture =
      material.pbrData.baseColorTexture;
  std::printf("Textured leaf material %s: %s, transmits %.3f x base color (diffuse transmission "
              "%.3f, color x %.3f)\n",
              name.c_str(), traced ? "traced" : "opaque", transmission, khr.factor,
              khr.colorScale);
}

} // namespace

namespace {

// Encoding a 4K atlas takes most of a tree's cook, and a species' variants
// share their maps, so encodings are cached beside the cooked trees, keyed by
// the image's pixels and the encoding settings.
std::vector<uint8_t> cachedEncode(const basisu::image &image, Usage usage,
                                  basisu::job_pool &jobPool, int coverageChannel,
                                  const std::filesystem::path &cacheDir) {
  constexpr uint32_t kVersion = 1;
  uint64_t hash = 0xcbf29ce484222325ull;
  const auto mix = [&](const void *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
      hash = (hash ^ static_cast<const uint8_t *>(data)[i]) * 0x100000001b3ull;
    }
  };
  const uint32_t header[] = {kVersion, image.get_width(), image.get_height(),
                             static_cast<uint32_t>(usage), static_cast<uint32_t>(coverageChannel)};
  mix(header, sizeof(header));
  mix(image.get_ptr(), image.get_total_pixels() * sizeof(basisu::color_rgba));
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.ktx2", static_cast<unsigned long long>(hash));
  const std::filesystem::path path = cacheDir / name;
  if (std::filesystem::exists(path)) {
    return readFile(path);
  }
  std::vector<uint8_t> ktx2 = encodeKtx2(image, usage, jobPool, coverageChannel);
  std::error_code error;
  std::filesystem::create_directories(cacheDir, error);
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(ktx2.data()), static_cast<std::streamsize>(ktx2.size()));
  if (!file) {
    std::printf("Warning: cannot write the texture cache %s\n", path.string().c_str());
  }
  return ktx2;
}

} // namespace

TreeCookResult cookTree(const std::filesystem::path &in, const std::filesystem::path &outDir,
                        size_t triangleBudget, const std::filesystem::path &barkTexture) {
  fastgltf::Parser parser(fastgltf::Extensions::KHR_texture_transform |
                          fastgltf::Extensions::KHR_materials_diffuse_transmission |
                          fastgltf::Extensions::KHR_materials_emissive_strength |
                          fastgltf::Extensions::KHR_mesh_quantization);
  auto data = fastgltf::GltfDataBuffer::FromPath(in);
  if (data.error() != fastgltf::Error::None) {
    throw std::runtime_error("cannot read " + in.string());
  }
  auto loaded = parser.loadGltf(data.get(), in.parent_path(),
                                fastgltf::Options::LoadExternalBuffers);
  if (loaded.error() != fastgltf::Error::None) {
    throw std::runtime_error(std::string("invalid glTF: ") +
                             std::string(fastgltf::getErrorMessage(loaded.error())));
  }
  const fastgltf::Asset &source = loaded.get();

  std::map<size_t, basisu::image> images;
  const auto sourceImage = [&](size_t texture) -> basisu::image & {
    const auto index = source.textures.at(texture).imageIndex;
    if (!index) {
      throw std::runtime_error("texture " + std::to_string(texture) +
                               " has no PNG or JPEG image");
    }
    auto it = images.find(*index);
    if (it == images.end()) {
      it = images
               .emplace(*index, decode(sourceImageBytes(source, source.images[*index],
                                                        in.parent_path())))
               .first;
    }
    return it->second;
  };

  // Alpha-tested leaf materials: their atlas traced once, for every card.
  std::map<size_t, AtlasMesh> atlases;
  for (size_t m = 0; m < source.materials.size(); ++m) {
    const fastgltf::Material &material = source.materials[m];
    if (!material.pbrData.baseColorTexture) {
      continue;
    }
    const basisu::image &color = sourceImage(material.pbrData.baseColorTexture->textureIndex);
    if (!tracedMaterial(material, isLeafMaterial(material), color)) {
      continue;
    }
    const float cutoff = alphaCutoff(material);
    if (!(cutoff > 0.0f && cutoff < 1.0f)) {
      throw std::runtime_error(std::string(material.name) + ": alpha cutoff outside (0, 1)");
    }
    std::vector<uint8_t> alpha(color.get_total_pixels());
    for (size_t i = 0; i < alpha.size(); ++i) {
      alpha[i] = color.get_ptr()[i].a;
    }
    const AtlasMesh &atlas = atlases[m] =
        traceAtlas(alpha, color.get_width(), color.get_height(), cutoff);
    std::printf("Traced atlas of %s: %zu triangles\n", std::string(material.name).c_str(),
                atlas.indices.size() / 3);
  }

  std::vector<Prototype> prototypes;
  std::vector<std::vector<size_t>> meshPrototypes(source.meshes.size());
  for (size_t m = 0; m < source.meshes.size(); ++m) {
    for (const fastgltf::Primitive &primitive : source.meshes[m].primitives) {
      meshPrototypes[m].push_back(prototypes.size());
      const auto atlas = atlases.find(primitive.materialIndex.value_or(0));
      prototypes.push_back(
          loadPrototype(source, primitive, atlas == atlases.end() ? nullptr : &atlas->second));
    }
  }
  std::vector<Instance> instances;
  fastgltf::iterateSceneNodes(
      source, source.defaultScene.value_or(0), fastgltf::math::fmat4x4(),
      [&](const fastgltf::Node &node, const fastgltf::math::fmat4x4 &matrix) {
        if (node.meshIndex) {
          for (size_t p : meshPrototypes[*node.meshIndex]) {
            instances.push_back({p, matrix});
            ++prototypes[p].instances;
          }
        }
      });

  // Source triangles, leaf area, and which instance vertices hold the
  // bounds: they are locked in simplification, and their pieces are kept.
  TreeCookResult result{};
  Bounds bounds;
  struct Extreme {
    size_t instance;
    uint32_t vertex;
  };
  std::array<Extreme, 6> extremes{};
  std::vector<Vec3> world;
  for (size_t i = 0; i < instances.size(); ++i) {
    const Prototype &p = prototypes[instances[i].prototype];
    world.resize(p.vertices.size());
    for (uint32_t v = 0; v < p.vertices.size(); ++v) {
      world[v] = transformPoint(instances[i].matrix, p.vertices[v].position);
      for (int c = 0; c < 3; ++c) {
        if (world[v][c] < bounds.min[c]) {
          bounds.min[c] = world[v][c];
          extremes[2 * c] = {i, v};
        }
        if (world[v][c] > bounds.max[c]) {
          bounds.max[c] = world[v][c];
          extremes[2 * c + 1] = {i, v};
        }
      }
    }
    result.sourceTriangles += p.indices.size() / 3;
    for (size_t t = 0; p.leaf && t < p.indices.size(); t += 3) {
      result.leafAreaBefore +=
          triangleArea(world[p.indices[t]], world[p.indices[t + 1]], world[p.indices[t + 2]]);
    }
  }
  if (instances.empty()) {
    throw std::runtime_error("tree has no mesh instances");
  }
  std::set<std::pair<size_t, uint32_t>> fixedVertices;
  for (const Extreme &e : extremes) {
    prototypes[instances[e.instance].prototype].locked[e.vertex] = 1;
    fixedVertices.insert({e.instance, e.vertex});
  }

  // One ratio for every prototype, so each gets a budget share weighted by
  // its instance count; lowered until the total fits or simplification
  // stalls at its error limits.
  double ratio = 1.0;
  float leafError = 0.0f;
  for (const float error : kLeafSimplifyErrors) {
    leafError = error;
    ratio = std::min(1.0, static_cast<double>(triangleBudget) / result.sourceTriangles);
    size_t previous = SIZE_MAX, total = 0;
    for (int pass = 0; pass < 8; ++pass) {
      for (Prototype &p : prototypes) {
        simplify(p, ratio, leafError);
      }
      total = cookedTriangles(prototypes, false) + cookedTriangles(prototypes, true);
      if (total <= triangleBudget || total > previous - previous / 100) {
        break;
      }
      previous = total;
      ratio *= static_cast<double>(triangleBudget) / total;
    }
    if (total <= triangleBudget) {
      break;
    }
  }
  const size_t woodTriangles = cookedTriangles(prototypes, false);
  if (woodTriangles > triangleBudget) {
    throw std::runtime_error("wood alone exceeds the triangle budget");
  }

  std::vector<LeafPiece> pieces;
  for (size_t i = 0; i < instances.size(); ++i) {
    const Prototype &p = prototypes[instances[i].prototype];
    for (size_t piece = 0; p.leaf && piece + 1 < p.cookedPieces.size(); ++piece) {
      LeafPiece leaf{i, piece, 0, 0.0, {}, {}, pieceHash(i, piece, 1), false};
      std::set<uint32_t> used;
      for (uint32_t t = p.cookedPieces[piece]; t < p.cookedPieces[piece + 1]; t += 3) {
        Vec3 corners[3];
        for (int k = 0; k < 3; ++k) {
          const uint32_t v = p.cooked[t + k];
          corners[k] = transformPoint(instances[i].matrix, p.vertices[v].position);
          if (used.insert(v).second) {
            leaf.fixed |= fixedVertices.contains({i, v});
            for (int c = 0; c < 3; ++c) {
              leaf.centroid[c] += corners[k][c];
              leaf.bounds.min[c] = std::min(leaf.bounds.min[c], corners[k][c]);
              leaf.bounds.max[c] = std::max(leaf.bounds.max[c], corners[k][c]);
            }
          }
        }
        leaf.area += triangleArea(corners[0], corners[1], corners[2]);
        ++leaf.triangles;
      }
      for (float &c : leaf.centroid) {
        c /= static_cast<float>(used.size());
      }
      pieces.push_back(leaf);
    }
  }

  // Stochastic pruning: the fixed pieces first, then a random order.
  std::sort(pieces.begin(), pieces.end(), [](const LeafPiece &a, const LeafPiece &b) {
    return a.fixed != b.fixed ? a.fixed : a.order < b.order;
  });
  const size_t leafPieceCount = pieces.size();
  const size_t leafBudget = triangleBudget - woodTriangles;
  size_t fixedTriangles = 0;
  for (const LeafPiece &leaf : pieces) {
    fixedTriangles += leaf.fixed ? leaf.triangles : 0;
  }
  if (fixedTriangles > leafBudget) {
    throw std::runtime_error("the leaf pieces holding the tree's bounds alone exceed the "
                             "triangle budget left after the wood");
  }
  size_t leafTriangles = 0;
  size_t kept = 0;
  for (; kept < pieces.size() && (pieces[kept].fixed ||
                                  leafTriangles + pieces[kept].triangles <= leafBudget);
       ++kept) {
    leafTriangles += pieces[kept].triangles;
  }
  pieces.resize(kept);

  // Area compensation: scale the kept pieces about their centroids by a
  // common factor, each clamped so it stays inside the bounds.
  double fixedArea = 0.0;
  std::vector<float> limits(pieces.size(), INFINITY);
  for (size_t i = 0; i < pieces.size(); ++i) {
    const LeafPiece &leaf = pieces[i];
    fixedArea += leaf.fixed ? leaf.area : 0.0;
    for (int c = 0; c < 3; ++c) {
      const float low = leaf.centroid[c] - leaf.bounds.min[c];
      const float high = leaf.bounds.max[c] - leaf.centroid[c];
      if (low > 0.0f) {
        limits[i] = std::min(limits[i], (leaf.centroid[c] - bounds.min[c]) / low);
      }
      if (high > 0.0f) {
        limits[i] = std::min(limits[i], (bounds.max[c] - leaf.centroid[c]) / high);
      }
    }
  }
  double scale = 1.0;
  for (int pass = 0; pass < 8; ++pass) {
    double clampedArea = fixedArea, freeArea = 0.0;
    for (size_t i = 0; i < pieces.size(); ++i) {
      if (pieces[i].fixed) {
        continue;
      }
      if (limits[i] < scale) {
        clampedArea += pieces[i].area * limits[i] * limits[i];
      } else {
        freeArea += pieces[i].area;
      }
    }
    if (freeArea <= 0.0) {
      break;
    }
    scale = std::sqrt(std::max(0.0, result.leafAreaBefore - clampedArea) / freeArea);
  }
  for (size_t i = 0; i < pieces.size(); ++i) {
    if (!pieces[i].fixed) {
      pieces[i].scale = std::min(limits[i], static_cast<float>(scale));
    }
    result.leafAreaAfter += pieces[i].area * pieces[i].scale * pieces[i].scale;
  }
  std::printf("simplified to %.3f of the source (leaf outlines %.0f mm); wood %zu tris; leaves "
              "%zu of %zu pieces kept "
              "(%zu tris), scaled by %.3f; bounds (%.2f %.2f %.2f)..(%.2f %.2f %.2f) m\n",
              ratio, leafError * 1000.0f, woodTriangles, kept, leafPieceCount, leafTriangles, scale, bounds.min[0],
              bounds.min[1], bounds.min[2], bounds.max[0], bounds.max[1], bounds.max[2]);
  std::vector<std::vector<const LeafPiece *>> instancePieces(instances.size());
  for (const LeafPiece &leaf : pieces) {
    instancePieces[leaf.instance].push_back(&leaf);
  }

  // Flatten into one vertex/index list per material.
  struct Output {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
  };
  std::vector<Output> outputs(source.materials.size());
  std::vector<uint32_t> remap;
  for (size_t i = 0; i < instances.size(); ++i) {
    const Prototype &p = prototypes[instances[i].prototype];
    const fastgltf::math::fmat4x4 &m = instances[i].matrix;
    const Vec3 columns[3] = {{m[0][0], m[0][1], m[0][2]},
                             {m[1][0], m[1][1], m[1][2]},
                             {m[2][0], m[2][1], m[2][2]}};
    // Normals transform by the inverse transpose: the cofactor matrix,
    // divided by the determinant (its sign is enough before normalizing).
    const Vec3 cofactor[3] = {cross(columns[1], columns[2]), cross(columns[2], columns[0]),
                              cross(columns[0], columns[1])};
    const float det = cofactor[0][0] * columns[0][0] + cofactor[0][1] * columns[0][1] +
                      cofactor[0][2] * columns[0][2];
    const float sign = det < 0.0f ? -1.0f : 1.0f;
    Output &out = outputs[p.material];
    remap.assign(p.vertices.size(), UINT32_MAX);
    auto emit = [&](uint32_t v, const Vec3 &centroid, float scale, const float *uv) {
      if (remap[v] == UINT32_MAX) {
        Vertex vertex = p.vertices[v];
        vertex.position = transformPoint(m, vertex.position);
        for (int c = 0; c < 3; ++c) {
          vertex.position[c] = centroid[c] + (vertex.position[c] - centroid[c]) * scale;
        }
        const Vec3 &n = p.vertices[v].normal;
        vertex.normal = normalize({sign * (cofactor[0][0] * n[0] + cofactor[1][0] * n[1] +
                                           cofactor[2][0] * n[2]),
                                   sign * (cofactor[0][1] * n[0] + cofactor[1][1] * n[1] +
                                           cofactor[2][1] * n[2]),
                                   sign * (cofactor[0][2] * n[0] + cofactor[1][2] * n[1] +
                                           cofactor[2][2] * n[2])});
        if (uv) {
          vertex.uv[0] = uv[0];
          vertex.uv[1] = uv[1];
        }
        remap[v] = static_cast<uint32_t>(out.vertices.size());
        out.vertices.push_back(vertex);
      }
      return remap[v];
    };
    auto emitTriangles = [&](uint32_t begin, uint32_t end, const Vec3 &centroid, float scale,
                             const float *uv) {
      for (uint32_t t = begin; t < end; t += 3) {
        const uint32_t a = emit(p.cooked[t], centroid, scale, uv);
        const uint32_t b = emit(p.cooked[t + 1], centroid, scale, uv);
        const uint32_t c = emit(p.cooked[t + 2], centroid, scale, uv);
        // A mirroring transform flips the winding.
        out.indices.insert(out.indices.end(), {a, det < 0.0f ? c : b, det < 0.0f ? b : c});
      }
    };
    if (!p.leaf) {
      emitTriangles(0, static_cast<uint32_t>(p.cooked.size()), Vec3{}, 1.0f, nullptr);
      continue;
    }
    for (const LeafPiece *leaf : instancePieces[i]) {
      const uint64_t texel = pieceHash(i, leaf->piece, 2) % (kLeafPaletteSize * kLeafPaletteSize);
      const float uv[2] = {(static_cast<float>(texel % kLeafPaletteSize) + 0.5f) / kLeafPaletteSize,
                           (static_cast<float>(texel / kLeafPaletteSize) + 0.5f) / kLeafPaletteSize};
      emitTriangles(p.cookedPieces[leaf->piece], p.cookedPieces[leaf->piece + 1],
                    leaf->centroid, leaf->scale, p.textured ? nullptr : uv);
    }
  }

  // Materials. Textured ones keep their own textures, re-encoded (leaf base
  // colors alpha-tested, with coverage-preserving mips). Untextured leaves
  // sample the tint palette; untextured wood gets the bark texture set, with
  // its base color factor chosen so the texture averages to the species
  // color.
  basisu::basisu_encoder_init();
  basisu::job_pool jobPool(std::max(1u, std::thread::hardware_concurrency()));
  Writer writer;
  std::vector<bool> leafMaterials(source.materials.size());
  bool untexturedWood = false;
  for (size_t m = 0; m < source.materials.size(); ++m) {
    leafMaterials[m] = isLeafMaterial(source.materials[m]);
    untexturedWood |= !leafMaterials[m] && !source.materials[m].pbrData.baseColorTexture;
  }
  std::optional<size_t> barkColor, barkNormal, barkArm;
  float barkMean[3] = {1.0f, 1.0f, 1.0f};
  if (!barkTexture.empty() && untexturedWood) {
    const basisu::image color = decode(readFile(barkMap(barkTexture, "_Color.jpg")));
    double sums[3] = {};
    for (uint32_t y = 0; y < color.get_height(); ++y) {
      for (uint32_t x = 0; x < color.get_width(); ++x) {
        for (int c = 0; c < 3; ++c) {
          sums[c] += srgb8ToLinear(color(x, y)[c]);
        }
      }
    }
    for (int c = 0; c < 3; ++c) {
      barkMean[c] = static_cast<float>(sums[c] / color.get_total_pixels());
    }
    barkColor = writer.addTexture(encodeKtx2(color, Usage::Color, jobPool), "bark_color");
    barkNormal = writer.addTexture(
        encodeKtx2(decode(readFile(barkMap(barkTexture, "_NormalGL.jpg"))), Usage::Normal,
                   jobPool),
        "bark_normal");
    // glTF packs occlusion, roughness and metalness into R, G and B.
    basisu::image arm = decode(readFile(barkMap(barkTexture, "_AmbientOcclusion.jpg")));
    const basisu::image roughness = decode(readFile(barkMap(barkTexture, "_Roughness.jpg")));
    if (roughness.get_width() != arm.get_width() || roughness.get_height() != arm.get_height()) {
      throw std::runtime_error("bark occlusion and roughness maps differ in size");
    }
    for (uint32_t y = 0; y < arm.get_height(); ++y) {
      for (uint32_t x = 0; x < arm.get_width(); ++x) {
        arm(x, y).set(arm(x, y)[0], roughness(x, y)[0], 0, 255);
      }
    }
    barkArm = writer.addTexture(encodeKtx2(arm, Usage::Data, jobPool), "bark_arm");
  }

  std::map<std::tuple<size_t, Usage, float>, size_t> encoded;
  const std::filesystem::path textureCache = outDir.parent_path() / ".ktx2-cache";
  const auto cookTexture = [&](const fastgltf::TextureInfo &info, Usage usage,
                               const std::string &name, float colorScale) {
    const size_t texCoord =
        info.transform ? info.transform->texCoordIndex.value_or(info.texCoordIndex)
                       : info.texCoordIndex;
    if (texCoord != 0) {
      throw std::runtime_error(name + " reads TEXCOORD_" + std::to_string(texCoord) +
                               "; the tree cook keeps only TEXCOORD_0");
    }
    const auto [it, inserted] = encoded.emplace(
        std::tuple{*source.textures[info.textureIndex].imageIndex, usage, colorScale}, 0);
    if (inserted) {
      basisu::image image = sourceImage(info.textureIndex);
      if (colorScale != 1.0f) {
        const double clamped = scaleLinearColor(image, colorScale);
        if (clamped > 0.0) {
          std::printf("%s: %.2f%% of color channels clamp at x %.3f\n", name.c_str(),
                      100.0 * clamped, colorScale);
        }
      }
      it->second = writer.addTexture(cachedEncode(image, usage, jobPool, -1, textureCache), name);
    }
    fastgltf::TextureInfo out = info;
    out.textureIndex = it->second;
    return out;
  };

  fastgltf::Asset &asset = writer.asset;
  fastgltf::Scene scene;
  for (size_t m = 0; m < source.materials.size(); ++m) {
    const fastgltf::Material &from = source.materials[m];
    const bool leaf = leafMaterials[m];
    fastgltf::Material material;
    material.name = from.name;
    material.doubleSided = leaf;
    material.pbrData.baseColorFactor = from.pbrData.baseColorFactor;
    material.pbrData.metallicFactor = from.pbrData.metallicFactor;
    material.pbrData.roughnessFactor = from.pbrData.roughnessFactor;
    const std::string name(from.name);
    if (from.pbrData.baseColorTexture) {
      if (!outputs[m].indices.empty()) {
        cookTexturedMaterial(from, leaf, name, material, sourceImage, cookTexture);
      }
    } else if (leaf) {
      const KhrLeafTransmission khr = khrLeafTransmission(kLeafDiffuseTransmission);
      basisu::image tint = leafPalette(from.pbrData.baseColorFactor.data());
      scaleLinearColor(tint, khr.colorScale);
      const size_t palette =
          writer.addTexture(encodeKtx2(tint, Usage::Color, jobPool), name + "_tint");
      material.pbrData.baseColorFactor = {1.0f, 1.0f, 1.0f, from.pbrData.baseColorFactor[3]};
      material.pbrData.baseColorTexture = fastgltf::TextureInfo{palette, 0, {}};
      material.diffuseTransmission = fastgltf::MaterialDiffuseTransmission{};
      material.diffuseTransmission->diffuseTransmissionFactor = khr.factor;
      material.diffuseTransmission->diffuseTransmissionColorTexture =
          material.pbrData.baseColorTexture;
    } else if (barkColor) {
      for (int c = 0; c < 3; ++c) {
        material.pbrData.baseColorFactor[c] =
            std::min(1.0f, from.pbrData.baseColorFactor[c] / barkMean[c]);
      }
      material.pbrData.baseColorTexture = fastgltf::TextureInfo{*barkColor, 0, {}};
      material.pbrData.roughnessFactor = 1.0f;
      material.pbrData.metallicFactor = 0.0f;
      material.pbrData.metallicRoughnessTexture = fastgltf::TextureInfo{*barkArm, 0, {}};
      fastgltf::NormalTextureInfo normal;
      normal.textureIndex = *barkNormal;
      material.normalTexture = normal;
      fastgltf::OcclusionTextureInfo occlusion;
      occlusion.textureIndex = *barkArm;
      material.occlusionTexture = occlusion;
    }
    asset.materials.push_back(std::move(material));

    Output &out = outputs[m];
    if (out.indices.empty()) {
      continue;
    }
    result.triangles += out.indices.size() / 3;
    std::vector<float> positions, normals, uvs;
    for (const Vertex &v : out.vertices) {
      positions.insert(positions.end(), v.position.begin(), v.position.end());
      normals.insert(normals.end(), v.normal.begin(), v.normal.end());
      uvs.insert(uvs.end(), {v.uv[0], v.uv[1]});
    }
    const size_t count = out.vertices.size();
    fastgltf::Primitive primitive;
    primitive.materialIndex = m;
    const size_t position = writer.addAccessor(
        positions.data(), positions.size() * 4, count, fastgltf::AccessorType::Vec3,
        fastgltf::ComponentType::Float, fastgltf::BufferTarget::ArrayBuffer);
    auto min = fastgltf::AccessorBoundsArray::ForType<double>(3);
    auto max = fastgltf::AccessorBoundsArray::ForType<double>(3);
    for (size_t c = 0; c < 3; ++c) {
      double low = INFINITY, high = -INFINITY;
      for (size_t v = c; v < positions.size(); v += 3) {
        low = std::min<double>(low, positions[v]);
        high = std::max<double>(high, positions[v]);
      }
      min.set<double>(c, low);
      max.set<double>(c, high);
    }
    asset.accessors[position].min = std::move(min);
    asset.accessors[position].max = std::move(max);
    primitive.attributes.push_back({"POSITION", position});
    primitive.attributes.push_back(
        {"NORMAL", writer.addAccessor(normals.data(), normals.size() * 4, count,
                                      fastgltf::AccessorType::Vec3, fastgltf::ComponentType::Float,
                                      fastgltf::BufferTarget::ArrayBuffer)});
    primitive.attributes.push_back(
        {"TEXCOORD_0", writer.addAccessor(uvs.data(), uvs.size() * 4, count,
                                          fastgltf::AccessorType::Vec2,
                                          fastgltf::ComponentType::Float,
                                          fastgltf::BufferTarget::ArrayBuffer)});
    primitive.indicesAccessor = writer.addAccessor(
        out.indices.data(), out.indices.size() * 4, out.indices.size(),
        fastgltf::AccessorType::Scalar, fastgltf::ComponentType::UnsignedInt,
        fastgltf::BufferTarget::ElementArrayBuffer);
    fastgltf::Mesh mesh;
    mesh.name = material.name;
    mesh.primitives.push_back(std::move(primitive));
    asset.meshes.push_back(std::move(mesh));
    fastgltf::Node node;
    node.meshIndex = asset.meshes.size() - 1;
    node.name = asset.meshes.back().name;
    asset.nodes.push_back(std::move(node));
    scene.nodeIndices.push_back(asset.nodes.size() - 1);
  }
  asset.scenes.push_back(std::move(scene));
  asset.defaultScene = 0;

  std::filesystem::create_directories(outDir);
  writer.write(outDir / (in.stem().string() + ".gltf"));
  return result;
}

} // namespace meridian::cook
