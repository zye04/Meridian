#include <doctest/doctest.h>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>

#include "ktx2.h"
#include "tree.h"

#include <basisu_enc.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

struct Bounds {
  std::array<float, 3> min{1e30f, 1e30f, 1e30f}, max{-1e30f, -1e30f, -1e30f};
  void add(const std::array<float, 3> &p) {
    for (int c = 0; c < 3; ++c) {
      min[c] = std::min(min[c], p[c]);
      max[c] = std::max(max[c], p[c]);
    }
  }
};

using Matrix = std::array<float, 16>; // column-major

std::array<float, 3> transform(const Matrix &m, const float *p) {
  return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
          m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
          m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

// Rotation about Y, uniform scale and a translation.
Matrix placement(float yaw, float scale, float x, float y, float z) {
  const float c = std::cos(yaw) * scale, s = std::sin(yaw) * scale;
  return {c, 0, -s, 0, 0, scale, 0, 0, s, 0, c, 0, x, y, z, 1};
}

struct Mesh {
  std::vector<float> positions, normals, uvs;
  std::vector<uint32_t> indices;
};

// A 2 m open tube of 32 x 16 quads: 1024 triangles of wood.
Mesh tube() {
  Mesh mesh;
  for (int ring = 0; ring <= 16; ++ring) {
    for (int side = 0; side <= 32; ++side) {
      const float angle = 6.2831853f * side / 32;
      mesh.positions.insert(mesh.positions.end(),
                            {0.1f * std::cos(angle), 2.0f * ring / 16, 0.1f * std::sin(angle)});
      mesh.normals.insert(mesh.normals.end(), {std::cos(angle), 0.0f, std::sin(angle)});
      mesh.uvs.insert(mesh.uvs.end(), {side / 32.0f, ring / 16.0f});
    }
  }
  for (uint32_t ring = 0; ring < 16; ++ring) {
    for (uint32_t side = 0; side < 32; ++side) {
      const uint32_t a = ring * 33 + side, b = a + 1, c = a + 33, d = c + 1;
      mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
    }
  }
  return mesh;
}

// 200 disjoint 10 x 6 cm leaves scattered around a 1 m crown, each a flat
// grid x grid of quads. Leaf i maps to its own UV cell: 16 x 16 cells, the
// leaf covering 90% of its cell.
constexpr float kLeafUvCell = 1.0f / 16.0f;
constexpr float kLeafUvSize = 0.9f * kLeafUvCell;

Mesh leaves(int grid = 1) {
  Mesh mesh;
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  for (uint32_t leaf = 0; leaf < 200; ++leaf) {
    const float cx = unit(rng) - 0.5f, cy = 1.5f + unit(rng), cz = unit(rng) - 0.5f;
    const float angle = 6.2831853f * unit(rng);
    const float ux = 0.05f * std::cos(angle), uz = 0.05f * std::sin(angle);
    const uint32_t base = static_cast<uint32_t>(mesh.positions.size() / 3);
    for (int j = 0; j <= grid; ++j) {
      for (int i = 0; i <= grid; ++i) {
        const float u = 2.0f * i / grid - 1.0f, v = 2.0f * j / grid - 1.0f;
        mesh.positions.insert(mesh.positions.end(), {cx + u * ux, cy + v * 0.03f, cz + u * uz});
        mesh.normals.insert(mesh.normals.end(), {-std::sin(angle), 0.0f, std::cos(angle)});
        mesh.uvs.insert(mesh.uvs.end(), {(leaf % 16) * kLeafUvCell + (u + 1) / 2 * kLeafUvSize,
                                         (leaf / 16) * kLeafUvCell + (v + 1) / 2 * kLeafUvSize});
      }
    }
    for (uint32_t j = 0; j < static_cast<uint32_t>(grid); ++j) {
      for (uint32_t i = 0; i < static_cast<uint32_t>(grid); ++i) {
        const uint32_t a = base + j * (grid + 1) + i, b = a + 1, c = a + grid + 1, d = c + 1;
        mesh.indices.insert(mesh.indices.end(), {a, b, d, a, d, c});
      }
    }
  }
  return mesh;
}

double triangleArea(const std::array<float, 3> &a, const std::array<float, 3> &b,
                    const std::array<float, 3> &c) {
  const double e[2][3] = {{b[0] - a[0], b[1] - a[1], b[2] - a[2]},
                          {c[0] - a[0], c[1] - a[1], c[2] - a[2]}};
  const double x = e[0][1] * e[1][2] - e[0][2] * e[1][1];
  const double y = e[0][2] * e[1][0] - e[0][0] * e[1][2];
  const double z = e[0][0] * e[1][1] - e[0][1] * e[1][0];
  return 0.5 * std::sqrt(x * x + y * y + z * z);
}

std::string floatList(const float *values, size_t count) {
  std::string out;
  for (size_t i = 0; i < count; ++i) {
    out += (i ? "," : "") + std::to_string(values[i]);
  }
  return out;
}

// Writes the two prototypes as an unflattened glTF like usd2gltf's: one mesh
// each, one node per instance.
// With `leafTexture`, the leaf material samples that image (beside the glTF),
// alpha-masked at 0.3.
void writeSource(const std::filesystem::path &path, const Mesh meshes[2],
                 const std::vector<std::pair<int, Matrix>> &nodes,
                 const std::string &leafTexture = {}) {
  std::vector<char> bin;
  std::string views, accessors, meshJson;
  int view = 0;
  auto add = [&](const void *data, size_t bytes, size_t count, const char *type,
                 int component, const std::string &extra) {
    views += std::string(view ? "," : "") + "{\"buffer\":0,\"byteOffset\":" +
             std::to_string(bin.size()) + ",\"byteLength\":" + std::to_string(bytes) + "}";
    bin.insert(bin.end(), static_cast<const char *>(data),
               static_cast<const char *>(data) + bytes);
    accessors += std::string(view ? "," : "") + "{\"bufferView\":" + std::to_string(view) +
                 ",\"componentType\":" + std::to_string(component) +
                 ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"" +
                 extra + "}";
    return view++;
  };
  for (int m = 0; m < 2; ++m) {
    const Mesh &mesh = meshes[m];
    Bounds bounds;
    for (size_t i = 0; i < mesh.positions.size(); i += 3) {
      bounds.add({mesh.positions[i], mesh.positions[i + 1], mesh.positions[i + 2]});
    }
    const size_t vertices = mesh.positions.size() / 3;
    const int position = add(mesh.positions.data(), mesh.positions.size() * 4, vertices,
                             "VEC3", 5126,
                             ",\"min\":[" + floatList(bounds.min.data(), 3) + "],\"max\":[" +
                                 floatList(bounds.max.data(), 3) + "]");
    const int normal = add(mesh.normals.data(), mesh.normals.size() * 4, vertices, "VEC3",
                           5126, "");
    const int uv = add(mesh.uvs.data(), mesh.uvs.size() * 4, vertices, "VEC2", 5126, "");
    const int index = add(mesh.indices.data(), mesh.indices.size() * 4,
                          mesh.indices.size(), "SCALAR", 5125, "");
    meshJson += std::string(m ? "," : "") + "{\"primitives\":[{\"attributes\":{" +
                "\"POSITION\":" + std::to_string(position) +
                ",\"NORMAL\":" + std::to_string(normal) +
                ",\"TEXCOORD_0\":" + std::to_string(uv) + "},\"indices\":" +
                std::to_string(index) + ",\"material\":" + std::to_string(m) + "}]}";
  }
  std::string nodeJson, sceneNodes;
  for (size_t i = 0; i < nodes.size(); ++i) {
    nodeJson += std::string(i ? "," : "") + "{\"mesh\":" + std::to_string(nodes[i].first) +
                ",\"matrix\":[" + floatList(nodes[i].second.data(), 16) + "]}";
    sceneNodes += std::string(i ? "," : "") + std::to_string(i);
  }
  const std::string binName = path.stem().string() + ".bin";
  std::ofstream(path.parent_path() / binName, std::ios::binary)
      .write(bin.data(), static_cast<std::streamsize>(bin.size()));
  std::ofstream(path)
      << "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[" << sceneNodes
      << "]}],\"nodes\":[" << nodeJson << "],\"meshes\":[" << meshJson
      << "],\"materials\":["
         "{\"name\":\"Bark\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.142,0.066,"
         "0.031,1],\"metallicFactor\":0,\"roughnessFactor\":0.5}},"
         "{\"name\":\"Leaves\",\"doubleSided\":true,\"pbrMetallicRoughness\":{"
         "\"baseColorFactor\":[0.087,0.153,0.021,1],\"metallicFactor\":0,"
         "\"roughnessFactor\":0.5"
      << (leafTexture.empty() ? "}"
                              : ",\"baseColorTexture\":{\"index\":0}},\"alphaMode\":\"MASK\","
                                "\"alphaCutoff\":0.3")
      << "}],"
      << (leafTexture.empty() ? ""
                              : "\"images\":[{\"uri\":\"" + leafTexture +
                                    "\"}],\"textures\":[{\"source\":0}],")
      << "\"accessors\":["
      << accessors << "],\"bufferViews\":[" << views << "],\"buffers\":[{\"uri\":\""
      << binName << "\",\"byteLength\":" << bin.size() << "}]}";
}

// Every file under `dir`, keyed by relative path.
std::map<std::string, std::string> readTree(const std::filesystem::path &dir) {
  std::map<std::string, std::string> files;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      std::ifstream stream(entry.path(), std::ios::binary);
      files[std::filesystem::relative(entry.path(), dir).string()] =
          std::string(std::istreambuf_iterator<char>(stream), {});
    }
  }
  return files;
}

size_t findRoot(std::vector<size_t> &parent, size_t i) {
  while (parent[i] != i) {
    i = parent[i] = parent[parent[i]];
  }
  return i;
}

// 200 disjoint discs of 3 cm radius, 96 outline vertices around a centre:
// outlines that simplify further the larger the allowed error.
Mesh discs() {
  Mesh mesh;
  std::mt19937 rng(11);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  for (uint32_t leaf = 0; leaf < 200; ++leaf) {
    const float cx = unit(rng) - 0.5f, cy = 1.5f + unit(rng), cz = unit(rng) - 0.5f;
    const uint32_t base = static_cast<uint32_t>(mesh.positions.size() / 3);
    mesh.positions.insert(mesh.positions.end(), {cx, cy, cz});
    for (int k = 0; k < 96; ++k) {
      const float angle = 6.2831853f * k / 96;
      mesh.positions.insert(mesh.positions.end(),
                            {cx + 0.03f * std::cos(angle), cy + 0.03f * std::sin(angle), cz});
      mesh.indices.insert(mesh.indices.end(),
                          {base, base + 1 + k, base + 1 + static_cast<uint32_t>((k + 1) % 96)});
    }
    for (int k = 0; k < 97; ++k) {
      mesh.normals.insert(mesh.normals.end(), {0.0f, 0.0f, 1.0f});
      mesh.uvs.insert(mesh.uvs.end(), {0.0f, 0.0f});
    }
  }
  return mesh;
}

// Connected leaf pieces of a cooked tree (vertices joined by exact position).
size_t cookedLeafPieces(const std::filesystem::path &gltf) {
  fastgltf::Parser parser(fastgltf::Extensions::KHR_texture_basisu |
                          fastgltf::Extensions::KHR_materials_diffuse_transmission);
  auto data = fastgltf::GltfDataBuffer::FromPath(gltf);
  REQUIRE(data.error() == fastgltf::Error::None);
  auto loaded = parser.loadGltf(data.get(), gltf.parent_path(), fastgltf::Options::LoadExternalBuffers);
  REQUIRE(loaded.error() == fastgltf::Error::None);
  const fastgltf::Asset &asset = loaded.get();
  size_t pieces = 0;
  for (const fastgltf::Mesh &mesh : asset.meshes) {
    const fastgltf::Primitive &primitive = mesh.primitives[0];
    if (!asset.materials[*primitive.materialIndex].doubleSided) {
      continue;
    }
    const auto &positionAccessor =
        asset.accessors[primitive.findAttribute("POSITION")->accessorIndex];
    std::vector<std::array<float, 3>> positions(positionAccessor.count);
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
        asset, positionAccessor,
        [&](fastgltf::math::fvec3 p, size_t i) { positions[i] = {p.x(), p.y(), p.z()}; });
    const auto &indexAccessor = asset.accessors[*primitive.indicesAccessor];
    std::vector<uint32_t> indices(indexAccessor.count);
    fastgltf::copyFromAccessor<uint32_t>(asset, indexAccessor, indices.data());
    std::map<std::array<float, 3>, size_t> ids;
    std::vector<size_t> parent;
    const auto id = [&](uint32_t v) {
      auto [it, inserted] = ids.emplace(positions[v], parent.size());
      if (inserted) {
        parent.push_back(parent.size());
      }
      return it->second;
    };
    for (size_t i = 0; i < indices.size(); i += 3) {
      const size_t a = id(indices[i]);
      parent[findRoot(parent, id(indices[i + 1]))] = findRoot(parent, a);
      parent[findRoot(parent, id(indices[i + 2]))] = findRoot(parent, a);
    }
    std::set<size_t> roots;
    for (size_t i = 0; i < parent.size(); ++i) {
      roots.insert(findRoot(parent, i));
    }
    pieces += roots.size();
  }
  return pieces;
}

} // namespace

TEST_CASE("cookTree reaches the budget and keeps leaf area and bounds") {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("meridian_tree_cook_test_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  const Mesh meshes[2] = {tube(), leaves()};
  std::vector<std::pair<int, Matrix>> nodes;
  for (int i = 0; i < 10; ++i) {
    nodes.emplace_back(0, placement(0.6f * i, 0.8f + 0.04f * i, 3.0f * i, 0.0f, 0.0f));
  }
  for (int i = 0; i < 50; ++i) {
    nodes.emplace_back(1, placement(0.3f * i, 0.9f + 0.005f * i, 0.6f * i, 0.0f, 1.0f));
  }
  writeSource(dir / "tree.gltf", meshes, nodes);

  Bounds sourceBounds;
  double sourceLeafArea = 0.0;
  for (const auto &[mesh, matrix] : nodes) {
    const Mesh &m = meshes[mesh];
    std::vector<std::array<float, 3>> world;
    for (size_t i = 0; i < m.positions.size(); i += 3) {
      world.push_back(transform(matrix, &m.positions[i]));
      sourceBounds.add(world.back());
    }
    for (size_t i = 0; mesh == 1 && i < m.indices.size(); i += 3) {
      sourceLeafArea +=
          triangleArea(world[m.indices[i]], world[m.indices[i + 1]], world[m.indices[i + 2]]);
    }
  }

  // Leaf quads can't be simplified, so the budget forces pruning.
  const size_t budget = 15000;
  const meridian::cook::TreeCookResult result =
      meridian::cook::cookTree(dir / "tree.gltf", dir / "out", budget, {});
  CHECK(result.sourceTriangles == 10 * 1024 + 50 * 400);
  CHECK(result.triangles <= budget);
  CHECK(result.triangles > budget * 9 / 10);
  CHECK(result.leafAreaBefore == doctest::Approx(sourceLeafArea).epsilon(1e-4));
  // The cook is deterministic: a second run writes the same files.
  meridian::cook::cookTree(dir / "tree.gltf", dir / "again", budget, {});
  CHECK(readTree(dir / "out") == readTree(dir / "again"));
  CHECK(std::abs(result.leafAreaAfter - result.leafAreaBefore) / result.leafAreaBefore <=
        0.05);

  fastgltf::Parser parser(fastgltf::Extensions::KHR_texture_basisu);
  auto data = fastgltf::GltfDataBuffer::FromPath(dir / "out" / "tree.gltf");
  REQUIRE(data.error() == fastgltf::Error::None);
  auto loaded = parser.loadGltf(data.get(), dir / "out", fastgltf::Options::LoadExternalBuffers);
  REQUIRE(loaded.error() == fastgltf::Error::None);
  const fastgltf::Asset &asset = loaded.get();

  Bounds cookedBounds;
  size_t triangles = 0, woodTriangles = 0;
  double leafArea = 0.0;
  bool sawLeaves = false;
  for (const fastgltf::Mesh &mesh : asset.meshes) {
    REQUIRE(mesh.primitives.size() == 1);
    const fastgltf::Primitive &primitive = mesh.primitives[0];
    const auto &positionAccessor =
        asset.accessors[primitive.findAttribute("POSITION")->accessorIndex];
    std::vector<std::array<float, 3>> positions(positionAccessor.count);
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
        asset, positionAccessor, [&](fastgltf::math::fvec3 p, size_t i) {
          positions[i] = {p.x(), p.y(), p.z()};
          cookedBounds.add(positions[i]);
        });
    const auto &indexAccessor = asset.accessors[*primitive.indicesAccessor];
    std::vector<uint32_t> indices(indexAccessor.count);
    fastgltf::copyFromAccessor<uint32_t>(asset, indexAccessor, indices.data());
    triangles += indices.size() / 3;
    if (!asset.materials[*primitive.materialIndex].doubleSided) {
      woodTriangles += indices.size() / 3;
      continue;
    }
    sawLeaves = true;
    for (size_t i = 0; i < indices.size(); i += 3) {
      leafArea += triangleArea(positions[indices[i]], positions[indices[i + 1]],
                               positions[indices[i + 2]]);
    }
    std::vector<std::array<float, 2>> uvs(positions.size());
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
        asset, asset.accessors[primitive.findAttribute("TEXCOORD_0")->accessorIndex],
        [&](fastgltf::math::fvec2 uv, size_t i) { uvs[i] = {uv.x(), uv.y()}; });
    std::vector<size_t> parent(positions.size());
    std::iota(parent.begin(), parent.end(), size_t{0});
    for (size_t i = 0; i < indices.size(); i += 3) {
      for (int k = 1; k < 3; ++k) {
        parent[findRoot(parent, indices[i + k])] = findRoot(parent, indices[i]);
      }
    }
    for (uint32_t index : indices) {
      const auto &uv = uvs[index];
      CHECK(uv == uvs[findRoot(parent, index)]);
      for (float coordinate : uv) {
        const float texel = coordinate * meridian::cook::kLeafPaletteSize - 0.5f;
        CHECK(std::abs(texel - std::round(texel)) < 1e-3f);
      }
    }
  }
  CHECK(sawLeaves);
  // Over budget, wood is simplified too, not only the leaves pruned.
  CHECK(woodTriangles > 0);
  CHECK(woodTriangles < 10 * 1024);
  CHECK(triangles == result.triangles);
  CHECK(leafArea == doctest::Approx(result.leafAreaAfter).epsilon(1e-3));
  for (int c = 0; c < 3; ++c) {
    CHECK(std::abs(cookedBounds.min[c] - sourceBounds.min[c]) <= 1e-3f);
    CHECK(std::abs(cookedBounds.max[c] - sourceBounds.max[c]) <= 1e-3f);
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("cookTree keeps a textured leaf material's UVs through simplification and pruning") {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("meridian_tree_cook_uv_test_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  // Leaves of 4 x 4 quads: flat, with affine UVs, so each simplifies to two
  // triangles; the budget then forces pruning as well.
  const Mesh meshes[2] = {tube(), leaves(4)};
  std::vector<std::pair<int, Matrix>> nodes;
  for (int i = 0; i < 10; ++i) {
    nodes.emplace_back(0, placement(0.6f * i, 0.8f + 0.04f * i, 3.0f * i, 0.0f, 0.0f));
  }
  for (int i = 0; i < 50; ++i) {
    nodes.emplace_back(1, placement(0.3f * i, 0.9f + 0.005f * i, 0.6f * i, 0.0f, 1.0f));
  }
  basisu::image texture(64, 64);
  for (uint32_t y = 0; y < 64; ++y) {
    for (uint32_t x = 0; x < 64; ++x) {
      // Opaque alpha: not traced, so this covers UV-preserving pruning.
      texture(x, y).set(60, 140, 30, 255);
    }
  }
  REQUIRE(basisu::save_png((dir / "leaf.png").string(), texture));
  writeSource(dir / "tree.gltf", meshes, nodes, "leaf.png");

  const size_t budget = 15000;
  const meridian::cook::TreeCookResult result =
      meridian::cook::cookTree(dir / "tree.gltf", dir / "out", budget, {});
  CHECK(result.sourceTriangles == 10 * 1024 + 50 * 200 * 32);
  CHECK(result.triangles <= budget);
  CHECK(std::abs(result.leafAreaAfter - result.leafAreaBefore) / result.leafAreaBefore <=
        0.05);

  fastgltf::Parser parser(fastgltf::Extensions::KHR_texture_basisu |
                          fastgltf::Extensions::KHR_materials_diffuse_transmission);
  auto data = fastgltf::GltfDataBuffer::FromPath(dir / "out" / "tree.gltf");
  REQUIRE(data.error() == fastgltf::Error::None);
  auto loaded = parser.loadGltf(data.get(), dir / "out", fastgltf::Options::LoadExternalBuffers);
  REQUIRE(loaded.error() == fastgltf::Error::None);
  const fastgltf::Asset &asset = loaded.get();

  bool sawLeaves = false;
  for (const fastgltf::Mesh &mesh : asset.meshes) {
    const fastgltf::Primitive &primitive = mesh.primitives[0];
    const fastgltf::Material &material = asset.materials[*primitive.materialIndex];
    if (!material.doubleSided) {
      continue;
    }
    sawLeaves = true;
    CHECK(material.alphaMode == fastgltf::AlphaMode::Opaque);
    REQUIRE(material.pbrData.baseColorTexture);
    REQUIRE(material.diffuseTransmission);
    REQUIRE(material.diffuseTransmission->diffuseTransmissionColorTexture);
    CHECK(material.diffuseTransmission->diffuseTransmissionColorTexture->textureIndex ==
          material.pbrData.baseColorTexture->textureIndex);

    const auto &positionAccessor =
        asset.accessors[primitive.findAttribute("POSITION")->accessorIndex];
    std::vector<std::array<float, 3>> positions(positionAccessor.count);
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
        asset, positionAccessor,
        [&](fastgltf::math::fvec3 p, size_t i) { positions[i] = {p.x(), p.y(), p.z()}; });
    std::vector<std::array<float, 3>> uvs(positions.size());
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
        asset, asset.accessors[primitive.findAttribute("TEXCOORD_0")->accessorIndex],
        [&](fastgltf::math::fvec2 uv, size_t i) { uvs[i] = {uv.x(), uv.y(), 0.0f}; });
    const auto &indexAccessor = asset.accessors[*primitive.indicesAccessor];
    std::vector<uint32_t> indices(indexAccessor.count);
    fastgltf::copyFromAccessor<uint32_t>(asset, indexAccessor, indices.data());

    std::vector<size_t> parent(positions.size());
    std::iota(parent.begin(), parent.end(), size_t{0});
    for (size_t i = 0; i < indices.size(); i += 3) {
      for (int k = 1; k < 3; ++k) {
        parent[findRoot(parent, indices[i + k])] = findRoot(parent, indices[i]);
      }
    }
    // Per leaf: triangles, the ratio of surface to UV area (one affine map
    // per leaf: the same for every triangle), and the UV bounds.
    struct Leaf {
      size_t triangles = 0;
      double ratio = 0.0;
      Bounds uv;
    };
    std::map<size_t, Leaf> pieces;
    for (size_t i = 0; i < indices.size(); i += 3) {
      Leaf &leaf = pieces[findRoot(parent, indices[i])];
      ++leaf.triangles;
      const double ratio = triangleArea(positions[indices[i]], positions[indices[i + 1]],
                                        positions[indices[i + 2]]) /
                           triangleArea(uvs[indices[i]], uvs[indices[i + 1]], uvs[indices[i + 2]]);
      if (leaf.ratio == 0.0) {
        leaf.ratio = ratio;
      }
      CHECK(ratio == doctest::Approx(leaf.ratio).epsilon(1e-3));
      for (int k = 0; k < 3; ++k) {
        leaf.uv.add(uvs[indices[i + k]]);
      }
    }
    // Instances are scaled by 0.9 or more, kept pieces only grow.
    const double sourceRatio = 0.1 * 0.06 / (kLeafUvSize * kLeafUvSize);
    for (const auto &[root, leaf] : pieces) {
      CHECK(leaf.triangles < 32);
      CHECK(leaf.ratio >= sourceRatio * 0.9 * 0.9 * (1.0 - 1e-3));
      // The leaf's outline corners survive, so its UVs span its whole cell.
      const float cellU = std::floor(leaf.uv.min[0] / kLeafUvCell) * kLeafUvCell;
      const float cellV = std::floor(leaf.uv.min[1] / kLeafUvCell) * kLeafUvCell;
      CHECK(leaf.uv.min[0] == doctest::Approx(cellU).epsilon(1e-5));
      CHECK(leaf.uv.min[1] == doctest::Approx(cellV).epsilon(1e-5));
      CHECK(leaf.uv.max[0] == doctest::Approx(cellU + kLeafUvSize).epsilon(1e-5));
      CHECK(leaf.uv.max[1] == doctest::Approx(cellV + kLeafUvSize).epsilon(1e-5));
    }
    CHECK(pieces.size() < 50 * 200);
  }
  CHECK(sawLeaves);
  std::filesystem::remove_all(dir);
}

TEST_CASE("cookTree traces alpha-tested cards into opaque leaves") {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("meridian_tree_cook_trace_test_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  // 200 single-quad cards, each showing its own 16 x 16-texel cell of a
  // 256 x 256 atlas; every cell holds a disk of radius 5 texels inside the
  // card's 14.4-texel UV square.
  const Mesh meshes[2] = {tube(), leaves()};
  const std::vector<std::pair<int, Matrix>> nodes = {{0, placement(0.0f, 1.0f, 0, 0, 0)},
                                                     {1, placement(0.0f, 1.0f, 0, 0, 0)}};
  basisu::image texture(256, 256);
  for (uint32_t y = 0; y < 256; ++y) {
    for (uint32_t x = 0; x < 256; ++x) {
      const float cx = x % 16 + 0.5f - 7.2f, cy = y % 16 + 0.5f - 7.2f;
      texture(x, y).set(60, 140, 30, std::hypot(cx, cy) < 5.0f ? 255 : 0);
    }
  }
  REQUIRE(basisu::save_png((dir / "leaf.png").string(), texture));
  writeSource(dir / "tree.gltf", meshes, nodes, "leaf.png");

  const meridian::cook::TreeCookResult result =
      meridian::cook::cookTree(dir / "tree.gltf", dir / "out", 1000000, {});
  const double cardArea = 0.1 * 0.06, uvArea = 0.9 * 16 * 0.9 * 16;
  CHECK(result.leafAreaBefore ==
        doctest::Approx(200 * cardArea * 3.14159265 * 25 / uvArea).epsilon(0.05));

  fastgltf::Parser parser(fastgltf::Extensions::KHR_texture_basisu |
                          fastgltf::Extensions::KHR_materials_diffuse_transmission);
  auto data = fastgltf::GltfDataBuffer::FromPath(dir / "out" / "tree.gltf");
  REQUIRE(data.error() == fastgltf::Error::None);
  auto loaded = parser.loadGltf(data.get(), dir / "out", fastgltf::Options::LoadExternalBuffers);
  REQUIRE(loaded.error() == fastgltf::Error::None);
  const fastgltf::Asset &asset = loaded.get();

  bool sawLeaves = false;
  for (const fastgltf::Mesh &mesh : asset.meshes) {
    const fastgltf::Primitive &primitive = mesh.primitives[0];
    const fastgltf::Material &material = asset.materials[*primitive.materialIndex];
    if (!material.doubleSided) {
      continue;
    }
    sawLeaves = true;
    CHECK(material.alphaMode == fastgltf::AlphaMode::Opaque);
    CHECK(material.diffuseTransmission);

    const auto &positionAccessor =
        asset.accessors[primitive.findAttribute("POSITION")->accessorIndex];
    std::vector<std::array<float, 3>> positions(positionAccessor.count);
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
        asset, positionAccessor,
        [&](fastgltf::math::fvec3 p, size_t i) { positions[i] = {p.x(), p.y(), p.z()}; });
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
        asset, asset.accessors[primitive.findAttribute("TEXCOORD_0")->accessorIndex],
        [&](fastgltf::math::fvec2 uv, size_t) {
          CHECK(uv.x() >= 0.0f);
          CHECK(uv.x() <= 1.0f);
          CHECK(uv.y() >= 0.0f);
          CHECK(uv.y() <= 1.0f);
        });
    const auto &indexAccessor = asset.accessors[*primitive.indicesAccessor];
    std::vector<uint32_t> indices(indexAccessor.count);
    fastgltf::copyFromAccessor<uint32_t>(asset, indexAccessor, indices.data());
    std::map<std::array<float, 3>, size_t> ids;
    std::vector<size_t> parent;
    const auto id = [&](uint32_t v) {
      auto [it, inserted] = ids.emplace(positions[v], parent.size());
      if (inserted) {
        parent.push_back(parent.size());
      }
      return it->second;
    };
    for (size_t i = 0; i < indices.size(); i += 3) {
      const size_t a = id(indices[i]);
      parent[findRoot(parent, id(indices[i + 1]))] = findRoot(parent, a);
      parent[findRoot(parent, id(indices[i + 2]))] = findRoot(parent, a);
    }
    std::set<size_t> roots;
    for (size_t i = 0; i < parent.size(); ++i) {
      roots.insert(findRoot(parent, i));
    }
    CHECK(roots.size() == 200);
  }
  CHECK(sawLeaves);
  std::filesystem::remove_all(dir);
}

TEST_CASE("cookTree keeps alpha-tested cards that transmit nothing alpha-tested") {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("meridian_tree_cook_decoration_test_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  // The traced-leaf atlas, on cards that transmit no light (bark lichen,
  // knots): they stay cards, masked at the source's cutoff.
  const Mesh meshes[2] = {tube(), leaves()};
  const std::vector<std::pair<int, Matrix>> nodes = {{0, placement(0.0f, 1.0f, 0, 0, 0)},
                                                     {1, placement(0.0f, 1.0f, 0, 0, 0)}};
  basisu::image texture(256, 256);
  for (uint32_t y = 0; y < 256; ++y) {
    for (uint32_t x = 0; x < 256; ++x) {
      const float cx = x % 16 + 0.5f - 7.2f, cy = y % 16 + 0.5f - 7.2f;
      texture(x, y).set(60, 140, 30, std::hypot(cx, cy) < 5.0f ? 255 : 0);
    }
  }
  REQUIRE(basisu::save_png((dir / "leaf.png").string(), texture));
  writeSource(dir / "tree.gltf", meshes, nodes, "leaf.png");
  std::string json;
  {
    std::ifstream file(dir / "tree.gltf");
    json.assign(std::istreambuf_iterator<char>(file), {});
  }
  const auto replace = [&](const std::string &from, const std::string &to) {
    const size_t at = json.find(from);
    REQUIRE(at != std::string::npos);
    json.replace(at, from.size(), to);
  };
  replace("\"alphaCutoff\":0.3", "\"alphaCutoff\":0.3,\"extensions\":{"
                                 "\"KHR_materials_diffuse_transmission\":{"
                                 "\"diffuseTransmissionFactor\":0}}");
  replace("{\"asset\":", "{\"extensionsUsed\":[\"KHR_materials_diffuse_transmission\"],"
                         "\"asset\":");
  std::ofstream(dir / "tree.gltf") << json;

  const meridian::cook::TreeCookResult result =
      meridian::cook::cookTree(dir / "tree.gltf", dir / "out", 1000000, {});

  fastgltf::Parser parser(fastgltf::Extensions::KHR_texture_basisu |
                          fastgltf::Extensions::KHR_materials_diffuse_transmission);
  auto data = fastgltf::GltfDataBuffer::FromPath(dir / "out" / "tree.gltf");
  REQUIRE(data.error() == fastgltf::Error::None);
  auto loaded = parser.loadGltf(data.get(), dir / "out", fastgltf::Options::LoadExternalBuffers);
  REQUIRE(loaded.error() == fastgltf::Error::None);
  const fastgltf::Asset &asset = loaded.get();

  bool sawCards = false;
  for (const fastgltf::Mesh &mesh : asset.meshes) {
    const fastgltf::Primitive &primitive = mesh.primitives[0];
    const fastgltf::Material &material = asset.materials[*primitive.materialIndex];
    if (!material.doubleSided) {
      continue;
    }
    sawCards = true;
    CHECK(material.alphaMode == fastgltf::AlphaMode::Mask);
    CHECK(material.alphaCutoff == doctest::Approx(0.3f));
    CHECK(asset.accessors[*primitive.indicesAccessor].count == 200 * 2 * 3);
  }
  CHECK(sawCards);
  CHECK(result.triangles == result.sourceTriangles);
  std::filesystem::remove_all(dir);
}

TEST_CASE("cookTree coarsens leaf outlines before pruning leaves") {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("meridian_tree_cook_coarsen_test_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const Mesh meshes[2] = {tube(), discs()};
  // Leaves only. A 3 cm disc needs ~9 triangles for a 2 mm outline, ~6 at
  // 4-5 mm: 7 per disc fits only coarser outlines.
  const std::vector<std::pair<int, Matrix>> nodes = {{1, placement(0.0f, 1.0f, 0, 0, 0)}};
  writeSource(dir / "tree.gltf", meshes, nodes);
  const size_t budget = 200 * 7;
  const meridian::cook::TreeCookResult result =
      meridian::cook::cookTree(dir / "tree.gltf", dir / "coarse", budget, {});
  CHECK(result.triangles <= budget);
  CHECK(cookedLeafPieces(dir / "coarse" / "tree.gltf") == 200);
  std::filesystem::remove_all(dir);
}

TEST_CASE("isLeafMaterial: double-sided, alpha-masked or named like foliage") {
  fastgltf::Material material;
  material.name = "M_Bark";
  CHECK(!meridian::cook::isLeafMaterial(material));
  material.name = "MI_Beech_Leaves_01";
  CHECK(meridian::cook::isLeafMaterial(material));
  material.name = "M_Bark";
  material.alphaMode = fastgltf::AlphaMode::Mask;
  CHECK(meridian::cook::isLeafMaterial(material));
  material.alphaMode = fastgltf::AlphaMode::Opaque;
  material.doubleSided = true;
  CHECK(meridian::cook::isLeafMaterial(material));
}

TEST_CASE("coverageMips keeps alpha-test coverage clear of the threshold") {
  using meridian::cook::Usage;
  // Uniform alpha 0.87: every mip must stay clear of 0.5, not land on it
  // (compression error would then drop the whole mip), without saturating.
  basisu::image uniform(64, 64);
  for (uint32_t i = 0; i < uniform.get_total_pixels(); ++i) {
    uniform.get_ptr()[i].set(50, 120, 30, 222);
  }
  for (const basisu::image &mip : meridian::cook::coverageMips(uniform, Usage::Color, 3)) {
    for (uint32_t i = 0; i < mip.get_total_pixels(); ++i) {
      CHECK(mip.get_ptr()[i].a >= 148);
      CHECK(mip.get_ptr()[i].a <= 158);
    }
  }
  // Holes of a quarter of the area: each mip keeps about 3/4 of its texels
  // above 0.5 while the holes are resolved (16 x 16).
  basisu::image holes(64, 64);
  for (uint32_t y = 0; y < 64; ++y) {
    for (uint32_t x = 0; x < 64; ++x) {
      holes(x, y).set(50, 120, 30, (x % 8 < 4 && y % 8 < 4) ? 0 : 255);
    }
  }
  for (const basisu::image &mip : meridian::cook::coverageMips(holes, Usage::Color, 3)) {
    if (mip.get_width() < 16) {
      break;
    }
    size_t above = 0;
    for (uint32_t i = 0; i < mip.get_total_pixels(); ++i) {
      above += mip.get_ptr()[i].a >= 128;
    }
    CHECK(static_cast<double>(above) / mip.get_total_pixels() ==
          doctest::Approx(0.75).epsilon(0.1));
  }
}

TEST_CASE("leaf transmission keeps the front's reflectance and the back's share as KHR") {
  for (const float t : {0.0f, 0.217f, 0.35f, 2.0f}) {
    const meridian::cook::KhrLeafTransmission khr = meridian::cook::khrLeafTransmission(t);
    CHECK((1.0f - khr.factor) * khr.colorScale == doctest::Approx(1.0f));
    CHECK(khr.factor * khr.colorScale == doctest::Approx(t));
  }
}
