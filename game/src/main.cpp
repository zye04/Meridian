#include "overlay.h"
#include "perf_overlay.h"

#include <fstream>
#include <numbers>

#include <meridian/camera_route.h>
#include <meridian/model.h>
#include <meridian/placement.h>
#include "geo_channels.h"
#include <meridian/renderer.h>
#include <meridian/terrain.h>
#include <meridian/texture.h>
#include <meridian/weather.h>
#include <meridian/window.h>
#ifdef MERIDIAN_HAS_DLSS
#include <meridian/dlss.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr float kMoveSpeed = 30.0f;
constexpr float kSprintMultiplier = 5.0f;
constexpr float kMouseSensitivity = 0.002f;
constexpr float kMaxPitch = 1.55f;
constexpr float kWalkSpeed = 1.4f;
constexpr float kWalkSprintSpeed = 5.0f;
constexpr float kEyeHeight = 1.7f;

// A starting look toward the Chlorophyll Engine frames
// (assets/reference/chlorophyll): AgX alone is flat, its inset desaturates
// daylight foliage and the cooked leaf colour leans blue; the sky already
// matches their chroma, so only the greens get more.
const meridian::Grade kStartingGrade{
    .contrast = 1.4f,
    .greens = {.hueShiftDegrees = -10.0f, .saturation = 1.5f}};

// walkOn: the eye follows the ground at eye height, WASD stays horizontal.
void updateFlyCamera(meridian::Camera &camera, const meridian::Window &window,
                     float deltaSeconds, const meridian::Heightfield *walkOn) {
  using meridian::Key;

  meridian::MouseDelta mouse = window.mouseDelta();
  camera.yaw += mouse.x * kMouseSensitivity;
  camera.pitch = std::clamp(camera.pitch - mouse.y * kMouseSensitivity,
                            -kMaxPitch, kMaxPitch);

  // Walking uses the yaw-only heading so pitch never shrinks the vector.
  const meridian::Vec3 forward =
      walkOn ? meridian::Vec3{std::sin(camera.yaw), 0.0f, -std::cos(camera.yaw)}
             : camera.forward();
  meridian::Vec3 move;
  if (window.isKeyDown(Key::W)) {
    move = move + forward;
  }
  if (window.isKeyDown(Key::S)) {
    move = move - forward;
  }
  if (window.isKeyDown(Key::D)) {
    move = move + camera.right();
  }
  if (window.isKeyDown(Key::A)) {
    move = move - camera.right();
  }
  if (!walkOn) {
    if (window.isKeyDown(Key::Space)) {
      move.y += 1.0f;
    }
    if (window.isKeyDown(Key::LeftControl)) {
      move.y -= 1.0f;
    }
  }
  if (meridian::length(move) > 0.0f) {
    const bool sprint = window.isKeyDown(Key::LeftShift);
    float speed = walkOn ? (sprint ? kWalkSprintSpeed : kWalkSpeed)
                         : (sprint ? kMoveSpeed * kSprintMultiplier : kMoveSpeed);
    camera.position =
        camera.position + meridian::normalize(move) * (speed * deltaSeconds);
  }
  if (walkOn) {
    camera.position.y =
        walkOn->heightAt(camera.position.x, camera.position.z) + kEyeHeight;
  }
}

// Loads one cooked ground texture set from assets/terrain/<id>/, its base
// color scaled from its mean (albedo.txt, linear RGB) 85% of the way to
// `target`, so neighbouring layers match and a hint of each one's own tint
// stays. Returns nothing (after saying which file) if any file is missing.
std::optional<meridian::TerrainLayer> loadTerrainLayer(meridian::Renderer &renderer,
                                                       const char *id, float tileSize,
                                                       float heightScale,
                                                       meridian::Vec3 target) {
  const auto directory = std::filesystem::path(MERIDIAN_ASSET_DIR) / "terrain" / id;
  const char *names[] = {"base_color.ktx2", "normal.ktx2", "arm.ktx2", "height.ktx2",
                         "albedo.txt"};
  for (const char *name : names) {
    const auto path = directory / name;
    if (!std::filesystem::exists(path)) {
      std::printf("Missing %s; terrain stays procedural (see "
                  "tools/fetch-sample-assets.sh)\n",
                  path.c_str());
      return std::nullopt;
    }
  }
  auto load = [&](const char *name, meridian::ColorSpace colorSpace) {
    return renderer.uploadTexture(
        meridian::loadKtx2(directory / (std::string(name) + ".ktx2"), colorSpace));
  };
  meridian::TerrainLayer layer;
  layer.baseColor = load("base_color", meridian::ColorSpace::Srgb);
  layer.normal = load("normal", meridian::ColorSpace::Linear);
  layer.arm = load("arm", meridian::ColorSpace::Linear);
  layer.height = load("height", meridian::ColorSpace::Linear);
  layer.tileSize = tileSize;
  layer.heightScale = heightScale;
  float mean[3] = {};
  if (FILE *file = std::fopen((directory / "albedo.txt").c_str(), "r")) {
    const int read = std::fscanf(file, "%f %f %f", &mean[0], &mean[1], &mean[2]);
    std::fclose(file);
    const float wanted[3] = {target.x, target.y, target.z};
    for (int c = 0; c < 3 && read == 3; ++c) {
      layer.albedoScale[c] = std::pow(wanted[c] / std::max(mean[c], 1e-3f), 0.85f);
    }
  }
  return layer;
}

// Loads a model from assets/, or says what's missing and returns nothing.
std::optional<meridian::Model> loadModel(meridian::Renderer &renderer,
                                         const char *relativePath) {
  const auto path = std::filesystem::path(MERIDIAN_ASSET_DIR) / relativePath;
  if (!std::filesystem::exists(path)) {
    std::printf("Missing %s; see tools/fetch-sample-assets.sh\n", relativePath);
    return std::nullopt;
  }
  return meridian::loadGltf(renderer, path);
}

void addModel(meridian::Renderer &renderer, const meridian::Model &model,
              const meridian::Mat4 &placement) {
  for (const meridian::DrawItem &item : model.drawItems) {
    renderer.addInstance(item.mesh, placement * item.transform);
  }
}

// Places models at random on the ground within `radius` of `center`, with a
// fixed seed so runs repeat. Returns how many were placed.
int scatterModels(meridian::Renderer &renderer, const meridian::Heightfield &terrain,
                  meridian::Vec3 center, float radius, int count, bool trees) {
  const char *ids[] = {"rock_moss_set_01", "rock_moss_set_02", "boulder_01",
                       "namaqualand_boulder_04", "tree_stump_01", "tree_stump_02",
                       "dead_tree_trunk_02", "dry_branches_medium_01", "pine_roots",
                       "fern_02", "nettle_plant", "shrub_02", "dandelion_01",
                       "fir_sapling", "pine_sapling_small", "island_tree_01"};
  std::vector<std::string> paths;
  for (const char *id : ids) {
    const std::string name = id;
    if (trees || (name != "fir_sapling" && name != "pine_sapling_small" &&
                  name != "island_tree_01")) {
      paths.push_back(name + "/" + name + "_2k.gltf");
    }
  }
  // Megascans Megaplants, imported by tools/fetch-sample-assets.sh.
  if (trees) {
    for (const char *tree : {"tree_european_beech_01_a", "tree_english_oak_forest_01_a",
                             "tree_norway_maple_01_a", "tree_european_beech_saplings_01_a",
                             "tree_rowan_sapling_a"}) {
      paths.push_back(std::string("source/megascans/") + tree + "/" + tree + ".gltf");
    }
  }
  std::vector<meridian::Model> models;
  for (const std::string &path : paths) {
    if (auto model = loadModel(renderer, path.c_str())) {
      models.push_back(std::move(*model));
    }
  }
  if (models.empty()) {
    return 0;
  }
  std::mt19937 rng(1);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  std::uniform_int_distribution<size_t> pick(0, models.size() - 1);
  uint32_t instances = renderer.instanceCount();
  for (int placed = 0; placed < count; ++placed) {
    const meridian::Model &model = models[pick(rng)];
    if (instances + model.drawItems.size() > meridian::kMaxInstances) {
      std::printf("Instance table full after %d scattered models\n", placed);
      return placed;
    }
    const float angle = 2.0f * 3.14159265f * unit(rng);
    const float distance = radius * std::sqrt(unit(rng));
    const float x = center.x + distance * std::cos(angle);
    const float z = center.z + distance * std::sin(angle);
    const float yaw = 2.0f * 3.14159265f * unit(rng);
    const float scale = 0.8f + 0.4f * unit(rng);
    // Sunk slightly so bases don't float where the ground curves.
    const float y = terrain.heightAt(x, z) - 0.05f;
    meridian::Mat4 placement = meridian::Mat4::identity();
    placement.m[0][0] = scale * std::cos(yaw);
    placement.m[0][2] = -scale * std::sin(yaw);
    placement.m[1][1] = scale;
    placement.m[2][0] = scale * std::sin(yaw);
    placement.m[2][2] = scale * std::cos(yaw);
    placement.m[3][0] = x;
    placement.m[3][1] = y;
    placement.m[3][2] = z;
    addModel(renderer, model, placement);
    instances += static_cast<uint32_t>(model.drawItems.size());
  }
  return count;
}

// Prototypes from a model in assets/: one with all its meshes, or one per
// draw item (rock sets: each rock on its own; their offsets are dropped).
std::vector<uint32_t> addPrototypes(meridian::Renderer &renderer,
                                    std::vector<meridian::Prototype> &prototypes,
                                    const std::string &path, bool perItem) {
  std::vector<uint32_t> added;
  const auto model = loadModel(renderer, path.c_str());
  if (!model) {
    return added;
  }
  for (const meridian::DrawItem &item : model->drawItems) {
    if (perItem || added.empty()) {
      added.push_back(static_cast<uint32_t>(prototypes.size()));
      prototypes.emplace_back();
    }
    prototypes.back().meshes.push_back(item.mesh);
  }
  return added;
}

constexpr uint32_t kPlacementSeed = 1;
constexpr uint32_t kForestLayerId = 1;

// The forest layer's rule, shared by placeScenery and --score-forest. On
// the Napf the forest is what farming left: slopes too steep to mow or
// graze (from ~14 degrees, closed by ~34) stay wooded, and the sunniest
// slopes are cleared for pasture. Tuned against the relief patch's canopy
// (--score-forest: kappa 0.461, F1 0.829); cluster noise lowered the score.
meridian::DensityRule forestRule() {
  meridian::DensityRule rule{14.0f, 90.0f, -1e9f, 1e9f, 20.0f / 76.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                             0.0f};
  rule.channels[0] = meridian::geo::ramp(meridian::geo::kSunExposure,
                                         meridian::geo::encodeSunExposure(1.5f),
                                         meridian::geo::encodeSunExposure(1.06f));
  return rule;
}

// Forests, lone trees on meadow slopes and rocks over the whole terrain,
// placed on the GPU at load (placement.h). With farField, distant forest
// cells are drawn as voxels (far field) instead of their trees.
void placeScenery(meridian::Renderer &renderer, bool trees, bool farField, bool streamed,
                  bool geoChannels) {
  using meridian::PlacementLayer;
  using meridian::Rotation;
  constexpr float kDegrees = 180.0f / 3.14159265f;
  std::vector<meridian::Prototype> prototypes;
  // (prototype, forest weight, lone weight). Only the beech is a mature,
  // broad-crowned tree (26.6 m, 13 m crown); the forest-grown oak is a
  // 4 m wide pole and the maple 8 m wide, so they stay a minority, and the
  // oak never stands alone. Saplings would take canopy slots (3 m tall).
  std::vector<std::pair<uint32_t, float>> forest, lone, saplings;
  // Megascans Trees cooked from Unreal (docs/assets.md) replace the
  // Megaplants when present: beech forests with hornbeam and some alder;
  // alder, hornbeam and the open-grown beech alone; all three species'
  // saplings under the canopy. Every cooked variant of a kind
  // (<species>/SM_<pack>_<kind>_NN) takes an equal share of its species'
  // weight.
  const auto addUnrealTrees = [&](const char *species, const char *pack, const char *kind,
                                  float weight, std::vector<std::pair<uint32_t, float>> &into) {
    std::vector<uint32_t> ids;
    for (int variant = 1; variant <= 20; ++variant) {
      char name[96];
      std::snprintf(name, sizeof(name), "SM_%s_%s_%02d", pack, kind, variant);
      const std::string path =
          std::string("source/megascans/") + species + "/" + name + "/" + name + ".gltf";
      if (std::filesystem::exists(std::filesystem::path(MERIDIAN_ASSET_DIR) / path)) {
        const std::vector<uint32_t> added = addPrototypes(renderer, prototypes, path, false);
        ids.insert(ids.end(), added.begin(), added.end());
      }
    }
    for (uint32_t id : ids) {
      into.emplace_back(id, weight / static_cast<float>(ids.size()));
    }
  };
  if (trees) {
    addUnrealTrees("europeanbeech", "EuropeanBeech", "Forest", 0.6f, forest);
    addUnrealTrees("europeanhornbeam", "EuropeanHornbeam", "Forest", 0.25f, forest);
    addUnrealTrees("blackalder", "BlackAlder", "Forest", 0.15f, forest);
    addUnrealTrees("blackalder", "BlackAlder", "Field", 0.5f, lone);
    addUnrealTrees("europeanhornbeam", "EuropeanHornbeam", "Field", 0.3f, lone);
    addUnrealTrees("europeanbeech", "EuropeanBeech", "Field", 0.2f, lone);
    addUnrealTrees("europeanbeech", "EuropeanBeech", "Sapling", 1.0f, saplings);
    addUnrealTrees("europeanhornbeam", "EuropeanHornbeam", "Sapling", 1.0f, saplings);
    addUnrealTrees("blackalder", "BlackAlder", "Sapling", 1.0f, saplings);
    if (lone.empty()) {
      lone = forest;
    }
    std::printf("Megascans Trees: %zu forest, %zu lone, %zu sapling prototypes\n", forest.size(),
                lone.size(), saplings.size());
  }
  const bool unrealTrees = !forest.empty();
  const std::vector<std::tuple<const char *, float, float>> megaplants =
      trees && !unrealTrees ? std::vector<std::tuple<const char *, float, float>>{
                  {"tree_european_beech_01_a", 0.7f, 0.75f},
                  {"tree_norway_maple_01_a", 0.2f, 0.25f},
                  {"tree_english_oak_forest_01_a", 0.1f, 0.0f}}
            : std::vector<std::tuple<const char *, float, float>>{};
  for (const auto &[tree, forestWeight, loneWeight] : megaplants) {
    std::string name = tree;
    // The Megascans-textured cook (tools/usd2gltf/texture_tree.py) when there is one.
    if (std::filesystem::exists(std::filesystem::path(MERIDIAN_ASSET_DIR) / "source/megascans" /
                                (name + "_tex") / (name + "_tex.gltf"))) {
      name += "_tex";
    }
    for (uint32_t id : addPrototypes(renderer, prototypes,
                                     "source/megascans/" + name + "/" + name + ".gltf", false)) {
      forest.emplace_back(id, forestWeight);
      if (loneWeight > 0.0f) {
        lone.emplace_back(id, loneWeight);
      }
    }
  }
  // Megaplants trees are 15-27 m tall; the Poly Haven stand-in is 5 m.
  float treeScale = 1.0f;
  if (trees && lone.empty()) {
    std::printf("Warning: no Megaplants trees (see tools/fetch-sample-assets.sh); placing "
                "island_tree_01 scaled to canopy height instead\n");
    forest.clear();
    for (uint32_t id :
         addPrototypes(renderer, prototypes, "island_tree_01/island_tree_01_2k.gltf", false)) {
      forest.emplace_back(id, 1.0f);
      lone.emplace_back(id, 1.0f);
    }
    treeScale = 4.4f;
  }
  std::vector<std::pair<uint32_t, float>> rocks;
  for (const auto &[path, perItem, weight] :
       {std::tuple{"source/megascans/massive_sandstone_cliff_vmmdcj2/vmmdcj2_tier_1.gltf",
                   false, 0.5f},
        {"boulder_01/boulder_01_2k.gltf", false, 1.0f},
        {"namaqualand_boulder_04/namaqualand_boulder_04_2k.gltf", false, 1.0f},
        {"rock_moss_set_01/rock_moss_set_01_2k.gltf", true, 0.3f},
        {"rock_moss_set_02/rock_moss_set_02_2k.gltf", true, 0.3f}}) {
    for (uint32_t id : addPrototypes(renderer, prototypes, path, perItem)) {
      rocks.emplace_back(id, weight);
    }
  }

  // Meadow accents: every variant of a Megascans plant is one prototype
  // (equal weights); `skip` drops variants by index (heavy modelled ones).
  const auto plant = [&](const char *id, std::initializer_list<size_t> skip = {}) {
    std::vector<std::pair<uint32_t, float>> variants;
    std::string path;
    const auto dir = std::filesystem::path(MERIDIAN_ASSET_DIR) / id;
    if (std::filesystem::is_directory(dir)) {
      for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.ends_with("_tier_1_nonUE.gltf") ||
            (path.empty() && name.ends_with("_tier_1.gltf"))) {
          path = std::string(id) + "/" + name;
        }
      }
    }
    if (path.empty()) {
      std::printf("Missing %s plant; see tools/fetch-sample-assets.sh\n", id);
      return variants;
    }
    size_t index = 0;
    for (uint32_t proto : addPrototypes(renderer, prototypes, path, true)) {
      if (std::find(skip.begin(), skip.end(), index++) == skip.end()) {
        variants.emplace_back(proto, 1.0f);
      }
    }
    return variants;
  };
  const auto poppy = plant("field_poppy_vmcobd0ja");
  const auto hawkbit = plant("yellow_flower_xchjef3ia");
  const auto goldenrod = plant("goldenrods_xg1lbfxia", {0, 1, 3});
  const auto clover = plant("clovers_rbdjz");
  const auto fern = plant("common_fern_rhdso");
  // Grass: a short, dense mat (the low Megascans clumps) and taller tufts
  // with seed heads standing out of it.
  const auto gather = [&](std::initializer_list<const char *> ids) {
    std::vector<std::pair<uint32_t, float>> all;
    for (const char *id : ids) {
      const auto variants = plant(id);
      all.insert(all.end(), variants.begin(), variants.end());
    }
    return all;
  };
  const auto grassMat = gather({"wild_grass_vlkhcbxia", "grass_clumps_qmbr2"});
  const auto grassTufts = gather({"rough_meadow_grass_skfjj", "wild_grass_sjliz"});
  // Graded toward target-meadow.png's fresh green and cream-yellow heads
  // (linear albedo multipliers, measured against its mean colour).
  const auto tint = [&](const std::vector<std::pair<uint32_t, float>> &variants,
                        meridian::Vec3 factor) {
    for (const auto &[prototype, weight] : variants) {
      prototypes[prototype].tint = factor;
    }
  };
  tint(grassMat, {1.15f, 1.9f, 0.95f});
  tint(grassTufts, {1.15f, 1.9f, 0.95f});
  tint(hawkbit, {1.1f, 1.0f, 0.6f});

  // Causal layers: each reads the ground's geo channels (geo_channels.h)
  // and, where one layer grows in another's shade or gaps, that layer's
  // density (PlacementLayer::parent). Noise only varies density inside the
  // extent the channels give (clusters, gaps), never decides it.
  namespace geo = meridian::geo;
  using meridian::ChannelRamp;
  std::vector<PlacementLayer> layers;
  std::optional<PlacementLayer> forestLayer; // a copy: layers grows
  if (!forest.empty()) {
    layers.push_back({kForestLayerId, forest, 6.0f, forestRule(), 0.8f * treeScale,
                      1.2f * treeScale, Rotation::Yaw, 0.3f});
    layers.back().farField = farField;
    // Field trees stand on the open pasture the forest leaves.
    PlacementLayer loneTrees{2, lone, 40.0f,
                             {0.0f, 30.0f, -1e9f, 1e9f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
                             0.9f * treeScale, 1.3f * treeScale, Rotation::Yaw, 0.3f};
    loneTrees.farField = farField;
    loneTrees.parent = meridian::parentRamp(layers.front(), 0.5f, 0.1f);
    layers.push_back(loneTrees);
    forestLayer = layers.front();
  }
  // Under-canopy layers follow the forest's density; without trees they go.
  const auto underForest = [&](PlacementLayer layer, float lo, float hi) {
    if (forestLayer) {
      layer.parent = meridian::parentRamp(*forestLayer, lo, hi);
      layers.push_back(layer);
    }
  };
  if (!saplings.empty()) {
    // Saplings in the gaps of the closed forest, where light reaches the floor.
    underForest({12, saplings, 15.0f,
                 {0.0f, 90.0f, -1e9f, 1e9f, 0.15f, 0.0f, 0.0f, 0.0f, 1.0f / 40.0f, 0.5f}, 0.8f,
                 1.2f, Rotation::Yaw, 0.1f, 150.0f},
                0.4f, 0.8f);
  }
  // Rocks where the ground material turns to rock: the density ramps up
  // over the same slopes as the material's blend (kRockNormalY).
  const float rockStart = std::acos(meridian::kRockNormalY[1]) * kDegrees;
  const float rockFull = std::acos(meridian::kRockNormalY[0]) * kDegrees;
  if (!rocks.empty()) {
    layers.push_back({3, rocks, 12.0f,
                      {rockStart, 90.0f, -1e9f, 1e9f, (rockFull - rockStart) / (90.0f - rockStart),
                       0.0f, 0.0f, 0.0f, 1.0f / 80.0f, 0.6f},
                      0.5f, 1.5f, Rotation::Full, 0.3f, 1500.0f});
    // Scree: smaller rocks that fell from the steep ground above, densest at
    // its foot and thinning out along the runout.
    PlacementLayer scree{13, rocks, 8.0f,
                         {0.0f, 45.0f, -1e9f, 1e9f, 0.2f, 0.0f, 0.0f, 0.0f, 1.0f / 30.0f, 0.5f},
                         0.25f, 0.7f, Rotation::Full, 0.15f, 600.0f};
    scree.rule.channels[0] = geo::ramp(geo::kTalus, geo::encodeTalus(0.15f), geo::encodeTalus(0.6f));
    layers.push_back(scree);
  }
  // Meadow plants by how dry and sunny the ground is: poppies, goldenrod
  // and hawkbit on warm, well-drained ground; clover where it stays moist.
  // The grass stops where the forest closes. Patches come from the mask at
  // a layer-specific frequency (gaps between drifts), clumping from the
  // layer's cluster noise. Flat to moderate slopes only.
  const ChannelRamp sunny =
      geo::ramp(geo::kSunExposure, geo::encodeSunExposure(0.85f), geo::encodeSunExposure(1.05f));
  const ChannelRamp dry = geo::ramp(geo::kWetness, geo::encodeWetness(7.0f), geo::encodeWetness(4.5f));
  const ChannelRamp moist =
      geo::ramp(geo::kWetness, geo::encodeWetness(4.0f), geo::encodeWetness(6.5f));
  struct Patch {
    uint32_t id;
    const std::vector<std::pair<uint32_t, float>> &variants;
    float footprint, slopeMax, patchFrequency, patchThreshold, clusterAmount;
    float scaleMin, scaleMax, drawDistance;
    float streamFullDensity;
    std::array<ChannelRamp, 2> ground;
    bool open; // not under the closed forest
  };
  // Streamed (streamFullDensity > 0, see PlacementLayer::streamFullDensity):
  // the grass mat everywhere grass grows, so no ground shows, seed-head
  // tufts and hawkbit in clumped patches, sparse clover; poppies and
  // goldenrod stay static, sparse accents.
  for (const Patch &patch :
       {Patch{4, poppy, 4.0f, 25.0f, 1.0f / 37.0f, 0.70f, 0.5f, 0.8f, 1.2f, 200.0f, 0.0f, {sunny, dry}, true},
        Patch{5, hawkbit, 0.3f, 28.0f, 1.0f / 15.0f, 0.50f, 1.0f, 2.0f, 2.8f, 120.0f, 30.0f, {sunny, {}}, true},
        Patch{6, goldenrod, 5.0f, 25.0f, 1.0f / 43.0f, 0.70f, 0.5f, 0.8f, 1.2f, 200.0f, 0.0f, {sunny, dry}, true},
        Patch{7, clover, 1.0f, 25.0f, 1.0f / 23.0f, 0.72f, 0.5f, 0.8f, 1.2f, 40.0f, 12.0f, {moist, {}}, true},
        Patch{8, grassMat, 0.1414f, 35.0f, 0.0f, 0.0f, 0.2f, 1.0f, 1.5f, 120.0f, 30.0f, {}, true},
        Patch{11, grassTufts, 0.5f, 35.0f, 1.0f / 15.0f, 0.52f, 0.8f, 0.45f, 0.7f, 100.0f, 25.0f, {}, true}}) {
    if (!patch.variants.empty() && (streamed || patch.streamFullDensity == 0.0f)) {
      PlacementLayer layer{patch.id, patch.variants, patch.footprint,
                           {0.0f, patch.slopeMax, -1e9f, 1e9f, 0.4f, patch.patchFrequency,
                            patch.patchThreshold, 0.08f, 1.0f / 6.0f, patch.clusterAmount},
                           patch.scaleMin, patch.scaleMax, Rotation::Yaw, 0.02f, patch.drawDistance,
                           patch.streamFullDensity};
      layer.rule.channels[0] = patch.ground[0];
      layer.rule.channels[1] = patch.ground[1];
      if (patch.open && forestLayer) {
        layer.parent = meridian::parentRamp(*forestLayer, 0.6f, 0.2f);
      }
      layers.push_back(layer);
    }
  }
  // Ferns where the ground stays moist and shaded; with trees, mostly under
  // the canopy's edge.
  if (!fern.empty()) {
    PlacementLayer ferns{9, fern, 12.0f,
                         {0.0f, 45.0f, -1e9f, 1e9f, 0.3f, 0.0f, 0.0f, 0.0f, 1.0f / 15.0f, 1.0f},
                         0.8f, 1.2f, Rotation::Yaw, 0.02f, 150.0f};
    ferns.rule.channels[0] = moist;
    ferns.rule.channels[1] =
        geo::ramp(geo::kSunExposure, geo::encodeSunExposure(1.0f), geo::encodeSunExposure(0.75f));
    if (forestLayer) {
      ferns.parent = meridian::parentRamp(*forestLayer, 0.2f, 0.6f);
    }
    layers.push_back(ferns);
  }
  if (!geoChannels) {
    // Without the ground's channels only the layers that read none remain.
    // When a parent goes, a layer that grows under it (rising ramp:
    // saplings) goes too; one that avoids it (inverted: grass, field trees)
    // stays without it.
    std::printf("No geo channels for the terrain: causal placement layers skipped\n");
    const auto reads = [](const meridian::DensityRule &rule) {
      return std::any_of(rule.channels.begin(), rule.channels.end(),
                         [](const ChannelRamp &ramp) { return ramp.channel >= 0; });
    };
    std::vector<PlacementLayer> kept;
    for (PlacementLayer layer : layers) {
      if (reads(layer.rule)) {
        continue;
      }
      if (layer.parent && std::none_of(kept.begin(), kept.end(), [&](const PlacementLayer &k) {
            return k.id == layer.parent->layerId;
          })) {
        if (layer.parent->lo < layer.parent->hi) {
          continue;
        }
        layer.parent.reset();
      }
      kept.push_back(std::move(layer));
    }
    layers = std::move(kept);
  }
  if (layers.empty()) {
    std::printf("Nothing to place (see tools/fetch-sample-assets.sh)\n");
    return;
  }
  renderer.setPlacement(prototypes, layers, kPlacementSeed,
                     std::filesystem::path(MERIDIAN_ASSET_DIR) / "placement-cache");
}

// Summer aerosol over the Swiss Plateau and the Napf, from the boundary
// layer's optical depth at 550 nm and its scale height in km (derivation and
// sources in docs/decisions.md, "Summer haze").
meridian::Atmosphere summerHaze(float opticalDepth, float scaleHeight) {
  meridian::Atmosphere atmosphere;
  // Extinction on the Plateau floor (0.5 km); the density reference is sea
  // level.
  const float extinction =
      opticalDepth / scaleHeight * std::exp(0.5f / scaleHeight);
  // Continental aerosol's Angstrom exponents (1.42 above 500 nm, 1.11
  // below) at the 680/550/440 nm the RGB coefficients stand for.
  atmosphere.mieExtinction = meridian::Vec3{0.740f, 1.0f, 1.281f} * extinction;
  atmosphere.mieScattering = atmosphere.mieExtinction * 0.925f;
  atmosphere.mieScaleHeight = scaleHeight;
  atmosphere.miePhaseG = 0.7f;
  return atmosphere;
}

// Summer haze as a boundary layer over the valley floor: near well mixed
// below a sharp-ish top, plus a thin free-troposphere background above it,
// so views from above the layer look through little haze while long paths
// in the valleys stay hazy.
meridian::Atmosphere summerLayerHaze(float floorKm) {
  constexpr float kLayerDepthKm = 1.2f;      // top above the valley floor
  constexpr float kLayerFadeKm = 0.25f;      // smoothstep width around the top
  constexpr float kScaleHeightKm = 2.0f;     // inside the layer
  constexpr float kLayerAod = 0.03f;         // AOD(550), floor to top
  constexpr float kBackgroundAod = 0.01f;    // AOD(550) above the top
  constexpr float kBackgroundHeightKm = 2.0f;
  const float top = floorKm + kLayerDepthKm;
  // Sea-level extinction from the layer's optical depth, integrated
  // numerically (midpoint) over the profile from the floor to its fade end.
  const float end = top + 0.5f * kLayerFadeKm;
  constexpr int kSteps = 2000;
  const float dh = (end - floorKm) / kSteps;
  float shape = 0.0f;
  for (int i = 0; i < kSteps; ++i) {
    const float h = floorKm + (i + 0.5f) * dh;
    const float t = std::clamp((h - (top - 0.5f * kLayerFadeKm)) / kLayerFadeKm, 0.0f, 1.0f);
    shape += std::exp(-h / kScaleHeightKm) * (1.0f - t * t * (3.0f - 2.0f * t)) * dh;
  }
  const float extinction = kLayerAod / shape;
  meridian::Atmosphere atmosphere = summerHaze(kLayerAod, kScaleHeightKm);
  atmosphere.mieExtinction = meridian::Vec3{0.740f, 1.0f, 1.281f} * extinction;
  atmosphere.mieScattering = atmosphere.mieExtinction * 0.925f;
  atmosphere.mieLayerTop = top;
  atmosphere.mieLayerFade = kLayerFadeKm;
  atmosphere.mieBackgroundScaleHeight = kBackgroundHeightKm;
  atmosphere.mieBackground = kBackgroundAod / (extinction * kBackgroundHeightKm *
                                               std::exp(-top / kBackgroundHeightKm));
  std::printf("Haze layer: floor %.2f km, top %.2f km, sea-level extinction %.3f/km (550 nm), "
              "AOD %.2f + %.2f above, background %.3f of the layer\n",
              floorKm, top, extinction, kLayerAod, kBackgroundAod, atmosphere.mieBackground);
  return atmosphere;
}

// The 5th percentile of the terrain heights, km above sea level.
float valleyFloorKm(const meridian::Heightfield &terrain) {
  std::vector<uint64_t> histogram(65536, 0);
  for (const uint16_t v : terrain.samples) {
    ++histogram[v];
  }
  const uint64_t target = terrain.samples.size() / 20;
  uint64_t seen = 0;
  size_t bin = 0;
  for (; bin < histogram.size() && seen + histogram[bin] <= target; ++bin) {
    seen += histogram[bin];
  }
  return (terrain.minHeight + terrain.heightScale * static_cast<float>(bin)) * 0.001f;
}

meridian::Atmosphere atmosphereFor(Haze haze, float valleyFloorKm) {
  switch (haze) {
  case Haze::Summer:
    return summerLayerHaze(valleyFloorKm);
  case Haze::SummerHazy:
    return summerHaze(0.2f, 1.5f);
  case Haze::Clean:
    break;
  }
  return {};
}

// "pom=0,micro=0,contact=0": comma-separated name=value over the defaults.
// Names: pom, pom-depth, pom-shadow, terrain-detail, micro, micro-strength, contact,
// contact-length, bloom (on/off are 0/1).
meridian::Effects parseEffects(const std::string &text) {
  meridian::Effects e;
  size_t begin = 0;
  while (begin < text.size()) {
    size_t end = text.find(',', begin);
    end = end == std::string::npos ? text.size() : end;
    const std::string item = text.substr(begin, end - begin);
    begin = end + 1;
    const size_t eq = item.find('=');
    if (eq == std::string::npos) {
      throw std::runtime_error("--effects takes name=value,... (got " + item + ")");
    }
    const std::string name = item.substr(0, eq);
    const float value = std::stof(item.substr(eq + 1));
    if (name == "pom") e.parallax = value != 0.0f;
    else if (name == "pom-depth") e.parallaxDepth = value;
    else if (name == "pom-shadow") e.parallaxSelfShadow = value != 0.0f;
    else if (name == "terrain-detail") e.terrainDetail = value != 0.0f;
    else if (name == "micro") e.microShadows = value != 0.0f;
    else if (name == "micro-strength") e.microShadowStrength = value;
    else if (name == "contact") e.contactShadows = value != 0.0f;
    else if (name == "contact-length") e.contactLength = value;
    else if (name == "bloom") e.bloom = value != 0.0f;
    else if (name == "cloud-outline") e.cloudOutline = value;
    else if (name == "cutout-bias") e.cutoutMipBias = value;
    else if (name == "taa-stable") e.stableTaa = value != 0.0f;
    else throw std::runtime_error("--effects: unknown effect " + name);
  }
  return e;
}

// Command-line options for deterministic, automated runs, e.g.
//   game --camera 1119 4354 -431 -1.55 -0.35 --sun 0.5 0.55 -0.4 --capture out.png
struct Options {
  std::optional<meridian::Camera> camera;
  std::optional<meridian::Vec3> sunDirection;
  // Grade file (see overlay.h) to load in any run; interactive runs also
  // load the default one.
  std::optional<std::filesystem::path> gradePath;
  // --terrain FILE: the heightfield (default assets/terrain/napf.height).
  std::optional<std::filesystem::path> terrainPath;
  // --score-forest FILE: score the forest rule against canopy truth, exit.
  std::optional<std::filesystem::path> scoreForestPath;
  bool overlayGrade = false;
  bool overlayPerf = false;
  // Timed run with real frame times and no input; the CSV goes to benchmarkPath.
  float benchmarkSeconds = 0.0f;
  std::filesystem::path benchmarkPath;
  // --route FILE: the benchmark's camera follows these keys (camera_route.h).
  std::vector<meridian::CameraKey> route;
  // Scripted runs move the camera at this velocity (m/s), ending at --camera.
  std::optional<meridian::Vec3> cameraVelocity;
  std::optional<std::pair<float, float>> cameraAngularVelocity;
  // Also saves <capture stem>_NNN.png every this many frames (sequences).
  int captureSequence = 0;
  std::optional<std::filesystem::path> capturePath;
  // The linear diffuse albedo of the --capture frame (.pfm), for the albedo audit.
  std::optional<std::filesystem::path> albedoCapturePath;
  // Poly Haven models and Megascans trees to scatter around the start.
  int scatter = 0;
  // Forests, lone trees and rocks over the terrain (placeScenery).
  bool placement = true;
  // The streamed meadow layers (grass mat, tufts, hawkbit, clover); not in the
  // TLAS, so --reference views compare without them.
  bool streamed = true;
  // --reference: path traced samples per pixel after the --frames warm-up.
  uint32_t referenceSamples = 0;
  meridian::ReferenceMode referenceMode = meridian::ReferenceMode::Radiance;
  // Trees are hidden by default (--trees): no tree prototypes, no forest
  // or lone-tree layers, no forest floor in the terrain material.
  bool trees = false;
  // Distant forest cells as voxels; off draws every tree (for A/B).
  bool farField = true;
  bool voxelFarShadows = true;
  bool gi = true;
  // Trees past the mid switch as prototype voxels; off keeps their clusters (A/B).
  bool voxelLod = true;
  bool occlusion = true;
  // Raster cascades; rt (one TLAS ray per pixel) is an A/B.
  meridian::ShadowMode shadows = meridian::ShadowMode::Raster;
  std::optional<meridian::Bloom> bloom;
  // Horizontal field of view; unset = the grade file's, else 70.
  std::optional<float> fovDegrees;
  std::optional<meridian::Effects> effects;
  // Clean: Bruneton's very clean defaults (for A/B). Unset: summer.
  std::optional<Haze> haze;
  // Largest simplification error on screen in pixels (0: full detail).
  float lodError = 1.0f;
  // Native TAA (decisions.md); --aa dlss is an A/B that falls back to TAA
  // when unsupported.
  meridian::AaMethod aa = meridian::AaMethod::Dlss;
  // Unset: DLAA (RR DLAA costs 4.7 ms at 1440p; optimised from the most
  // demanding configuration down, decisions.md).
  std::optional<meridian::DlssMode> dlssMode;
  char dlssPreset = 0; // 0: the DLSS module's per-mode default
  // Ray Reconstruction under DLSS, an A/B option; Super Resolution DLAA
  // is the default (decisions.md).
  bool rayReconstruction = false;
  // TAA's render size over display size, in (0, 1].
  std::optional<float> renderScale;
  // Fullscreen by default: consistent size for GPU timings, and the way the
  // game is played.
  bool fullscreen = true;
  // Frames to render before exiting; 0 runs until the window closes.
  int frames = 0;
  std::string weather = "fair";
  // Game seconds per real second (time-lapse).
  float timeScale = 1.0f;
  // Blends from the start weather to overcast over this many game seconds.
  std::optional<float> weatherFront;
  bool clouds = true;
  // Saves the weather map after the last frame (debug).
  std::optional<std::filesystem::path> weatherMapPath;
};

Options parseOptions(int argc, char **argv) {
  Options options;
  auto number = [&](int &i) {
    if (++i >= argc) {
      throw std::runtime_error("missing value for option");
    }
    return std::stof(argv[i]);
  };
  auto text = [&](int &i) {
    if (++i >= argc) {
      throw std::runtime_error("missing value for " + std::string(argv[i - 1]));
    }
    return std::string(argv[i]);
  };
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--camera") {
      meridian::Camera camera;
      camera.position = {number(i), number(i), number(i)};
      camera.yaw = number(i);
      camera.pitch = number(i);
      options.camera = camera;
    } else if (arg == "--sun") {
      options.sunDirection =
          meridian::normalize({number(i), number(i), number(i)});
    } else if (arg == "--camera-velocity") {
      options.cameraVelocity = {number(i), number(i), number(i)};
    } else if (arg == "--camera-angular-velocity") {
      const float yawRate = number(i);
      options.cameraAngularVelocity = {yawRate, number(i)};
    } else if (arg == "--grade" && i + 1 < argc) {
      options.gradePath = argv[++i];
    } else if (arg == "--overlay" && i + 1 < argc) {
      const std::string_view panel = argv[++i];
      if (panel != "grade" && panel != "perf") {
        throw std::runtime_error("--overlay takes grade or perf");
      }
      (panel == "grade" ? options.overlayGrade : options.overlayPerf) = true;
    } else if (arg == "--benchmark" && i + 2 < argc) {
      options.benchmarkSeconds = std::stof(argv[++i]);
      if (!(options.benchmarkSeconds > 0.0f)) {
        throw std::runtime_error("--benchmark takes seconds > 0 and a file.csv");
      }
      options.benchmarkPath = argv[++i];
    } else if (arg == "--route" && i + 1 < argc) {
      std::ifstream file(argv[++i]);
      if (!file) {
        throw std::runtime_error(std::string("Can't read route ") + argv[i]);
      }
      options.route = meridian::parseRoute(file);
    } else if (arg == "--camera-yaw-rate") {
      options.cameraAngularVelocity = {number(i), 0.0f};
    } else if (arg == "--capture-sequence") {
      options.captureSequence = static_cast<int>(number(i));
    } else if (arg == "--capture-albedo" && i + 1 < argc) {
      options.albedoCapturePath = argv[++i];
    } else if (arg == "--capture" && i + 1 < argc) {
      options.capturePath = argv[++i];
    } else if (arg == "--windowed") {
      options.fullscreen = false;
    } else if (arg == "--haze" && i + 1 < argc) {
      const std::string haze = argv[++i];
      if (haze == "clean") {
        options.haze = Haze::Clean;
      } else if (haze == "summer") {
        options.haze = Haze::Summer;
      } else if (haze == "summer-hazy") {
        options.haze = Haze::SummerHazy;
      } else {
        throw std::runtime_error("--haze takes clean, summer or summer-hazy");
      }
    } else if (arg == "--fov") {
      const float fov = number(i);
      if (!(fov > 30.0f && fov < 150.0f)) {
        throw std::runtime_error("--fov takes degrees (horizontal) between 30 and 150");
      }
      options.fovDegrees = fov;
    } else if (arg == "--bloom") {
      options.bloom = meridian::Bloom{number(i), number(i)};
    } else if (arg == "--effects" && i + 1 < argc) {
      options.effects = parseEffects(argv[++i]);
    } else if (arg == "--shadows") {
      const std::string mode = argv[++i];
      if (mode == "raster") {
        options.shadows = meridian::ShadowMode::Raster;
      } else if (mode == "rt") {
        options.shadows = meridian::ShadowMode::Rt;
      } else {
        throw std::runtime_error("--shadows takes raster or rt");
      }
    } else if (arg == "--no-occlusion") {
      options.occlusion = false;
    } else if (arg == "--lod-error") {
      options.lodError = number(i);
    } else if (arg == "--render-scale") {
      const float scale = number(i);
      if (!(scale > 0.0f && scale <= 1.0f)) {
        throw std::runtime_error("--render-scale takes a value in (0, 1]");
      }
      options.renderScale = scale;
    } else if (arg == "--aa" && i + 1 < argc) {
      const std::string aa = argv[++i];
      if (aa == "dlss") {
        options.aa = meridian::AaMethod::Dlss;
      } else if (aa == "taa") {
        options.aa = meridian::AaMethod::Taa;
      } else {
        throw std::runtime_error("--aa takes dlss or taa");
      }
    } else if (arg == "--dlss-mode" && i + 1 < argc) {
      const std::string mode = argv[++i];
      if (mode == "dlaa") {
        options.dlssMode = meridian::DlssMode::Dlaa;
      } else if (mode == "quality") {
        options.dlssMode = meridian::DlssMode::Quality;
      } else if (mode == "balanced") {
        options.dlssMode = meridian::DlssMode::Balanced;
      } else if (mode == "performance") {
        options.dlssMode = meridian::DlssMode::Performance;
      } else {
        throw std::runtime_error("--dlss-mode takes dlaa, quality, balanced or performance");
      }
    } else if (arg == "--dlss-preset" && i + 1 < argc) {
      const std::string preset = argv[++i];
      // The presets NGX 310.9.16 names (nvsdk_ngx_defs.h); the other
      // letters are removed or reserved.
      if (preset.size() != 1 || std::string_view("EFJKLM").find(preset[0]) ==
                                    std::string_view::npos) {
        throw std::runtime_error("--dlss-preset takes E, F, J, K, L or M");
      }
      options.dlssPreset = preset[0];
    } else if (arg == "--rr" && i + 1 < argc) {
      const std::string rr = argv[++i];
      if (rr != "on" && rr != "off") {
        throw std::runtime_error("--rr takes on or off");
      }
      options.rayReconstruction = rr == "on";
    } else if (arg == "--terrain" && i + 1 < argc) {
      options.terrainPath = argv[++i];
    } else if (arg == "--score-forest" && i + 1 < argc) {
      options.scoreForestPath = argv[++i];
    } else if (arg == "--trees") {
      options.trees = true;
    } else if (arg == "--scatter") {
      options.scatter = static_cast<int>(number(i));
    } else if (arg == "--placement" && i + 1 < argc &&
               std::string(argv[i + 1]) == "off") {
      options.placement = false;
      ++i;
    } else if (arg == "--reference") {
      const float samples = number(i);
      if (samples < 1.0f) {
        throw std::runtime_error("--reference takes a sample count >= 1");
      }
      options.referenceSamples = static_cast<uint32_t>(samples);
    } else if (arg == "--reference-mode") {
      const std::string value = text(i);
      using meridian::ReferenceMode;
      const std::pair<const char *, ReferenceMode> modes[] = {
          {"radiance", ReferenceMode::Radiance}, {"albedo", ReferenceMode::Albedo},
          {"white", ReferenceMode::FurnaceWhite}, {"leaf", ReferenceMode::FurnaceLeaf},
          {"sun", ReferenceMode::FurnaceSun}};
      const auto *found = std::find_if(std::begin(modes), std::end(modes),
                                       [&](const auto &m) { return value == m.first; });
      if (found == std::end(modes)) {
        throw std::runtime_error("--reference-mode takes radiance, albedo, white, leaf or sun");
      }
      options.referenceMode = found->second;
    } else if (arg == "--streamed") {
      const std::string value = text(i);
      if (value != "on" && value != "off") {
        throw std::runtime_error("--streamed takes on or off");
      }
      options.streamed = value == "on";
    } else if (arg == "--far-field" && i + 1 < argc &&
               std::string(argv[i + 1]) == "off") {
      options.farField = false;
      ++i;
    } else if (arg == "--far-shadows" && i + 1 < argc) {
      const std::string mode = argv[++i];
      if (mode != "tlas" && mode != "voxel") {
        throw std::runtime_error("--far-shadows takes tlas or voxel");
      }
      options.voxelFarShadows = mode == "voxel";
    } else if (arg == "--gi" && i + 1 < argc) {
      const std::string mode = argv[++i];
      if (mode != "off" && mode != "probes") {
        throw std::runtime_error("--gi takes off or probes");
      }
      options.gi = mode == "probes";
    } else if (arg == "--voxel-lod" && i + 1 < argc) {
      const std::string mode = argv[++i];
      if (mode != "on" && mode != "off") {
        throw std::runtime_error("--voxel-lod takes on or off");
      }
      options.voxelLod = mode == "on";
    } else if (arg == "--frames") {
      options.frames = static_cast<int>(number(i));
    } else if (arg == "--weather") {
      options.weather = text(i);
      // Throws on an unknown name, before anything loads.
      meridian::weatherPreset(options.weather);
    } else if (arg == "--time-scale") {
      options.timeScale = number(i);
    } else if (arg == "--weather-front") {
      options.weatherFront = number(i);
    } else if (arg == "--clouds") {
      const std::string value = text(i);
      if (value != "on" && value != "off") {
        throw std::runtime_error("--clouds takes on or off");
      }
      options.clouds = value == "on";
    } else if (arg == "--dump-weather-map") {
      options.weatherMapPath = text(i);
    } else {
      throw std::runtime_error("unknown option " + arg +
                               " (use --camera x y z yaw pitch, --sun x y z, "
                               "--camera-velocity x y z, "
                               "--camera-angular-velocity yaw pitch, --camera-yaw-rate rad/s, "
                               "--scatter n, --placement off, --streamed on|off, --reference spp, "
                               "--reference-mode radiance|albedo|white|leaf|sun, --trees, --no-occlusion, "
                                "--far-field off, --far-shadows tlas|voxel, --gi off|probes, --voxel-lod on|off, "
                               "--haze clean|summer|summer-hazy, --grade file.json, --overlay grade|perf, --benchmark seconds file.csv, --route file.route, "
                               "--lod-error px, --render-scale s, --aa dlss|taa, "
                               "--dlss-mode dlaa|quality|balanced|performance, "
                               "--dlss-preset E|F|J|K|L|M, --rr on|off, "
                               "--windowed, "
                               "--weather clear|fair|broken|overcast, --time-scale x, "
                               "--weather-front seconds, --clouds on|off, --dump-weather-map file.png, "
                               "--capture file.png|file.pfm, --capture-albedo file.pfm, --capture-sequence n, "
                               "--frames n)");
    }
  }
  // Enough frames for temporal accumulation and exposure to settle.
  if ((options.capturePath || options.weatherMapPath) && options.frames == 0 &&
      options.benchmarkSeconds == 0.0f) {
    options.frames = 120;
  }
  if ((options.cameraVelocity || options.cameraAngularVelocity) &&
      options.frames == 0) {
    throw std::runtime_error(
        "--camera-velocity and --camera-angular-velocity need --capture or "
        "--frames");
  }
  if (options.benchmarkSeconds > 0.0f &&
      (options.capturePath || options.cameraVelocity || options.cameraAngularVelocity)) {
    throw std::runtime_error(
        "--benchmark can't be combined with --capture, --camera-velocity or "
        "--camera-angular-velocity");
  }
  if (!options.route.empty() && options.benchmarkSeconds == 0.0f) {
    throw std::runtime_error("--route needs --benchmark");
  }
  if (options.captureSequence > 0 && !options.capturePath) {
    throw std::runtime_error("--capture-sequence needs --capture");
  }
  if (options.albedoCapturePath && !options.capturePath) {
    throw std::runtime_error("--capture-albedo needs --capture");
  }
  if (options.referenceSamples > 0 &&
      (!options.capturePath || options.benchmarkSeconds > 0.0f || options.captureSequence > 0 ||
       options.cameraVelocity || options.cameraAngularVelocity || options.albedoCapturePath)) {
    throw std::runtime_error("--reference needs --capture, without --benchmark, "
                             "--capture-sequence, --capture-albedo or camera velocities "
                             "(the reference's albedo: --reference-mode albedo)");
  }
  // The reference traces display pixels, while the haze froxels it reads are
  // indexed in render pixels.
  if (options.referenceSamples > 0 &&
      ((options.dlssMode && *options.dlssMode != meridian::DlssMode::Dlaa) ||
       (options.renderScale && *options.renderScale < 1.0f))) {
    throw std::runtime_error("--reference needs native rendering (DLAA or --render-scale 1)");
  }
  return options;
}

} // namespace

// The forest rule against a canopy truth (tools/canopy-truth.py): a cell is
// forest when the rule's mean density over a 4x4 grid of points in it is at
// least one half. Prints precision, recall and F1 of the forest class, and
// Cohen's kappa: agreement beyond chance, the one to judge by when the truth
// is mostly forest (calling everything forest scores F1 0.82 on the relief
// patch, kappa 0).
int scoreForest(const Options &options) {
  const auto heightPath = options.terrainPath.value_or(
      std::filesystem::path(MERIDIAN_ASSET_DIR) / "terrain" / "napf.height");
  const meridian::Heightfield field = meridian::loadHeightfield(heightPath);
  if (!std::filesystem::exists(meridian::geoPathFor(heightPath))) {
    std::fprintf(stderr, "--score-forest needs %s (meridian-cook --geo)\n",
                 meridian::geoPathFor(heightPath).c_str());
    return 1;
  }
  const meridian::GeoChannels geo = meridian::loadGeoChannels(meridian::geoPathFor(heightPath));
  if (!meridian::geoMatches(geo, field)) {
    std::fprintf(stderr, "%s does not match the heightfield\n",
                 meridian::geoPathFor(heightPath).c_str());
    return 1;
  }
  std::ifstream file(*options.scoreForestPath, std::ios::binary);
  uint32_t header[3] = {};
  file.read(reinterpret_cast<char *>(header), sizeof(header));
  const auto [columns, rows, cell] = header;
  std::vector<uint8_t> truth(size_t{columns} * rows);
  file.read(reinterpret_cast<char *>(truth.data()), static_cast<std::streamsize>(truth.size()));
  if (!file || columns * cell != field.width || rows * cell != field.height) {
    std::fprintf(stderr, "%s does not match the heightfield\n",
                 options.scoreForestPath->c_str());
    return 1;
  }
  meridian::PlacementLayer layer{};
  layer.id = kForestLayerId;
  layer.rule = forestRule();
  const auto toFixed = [&](float sample, uint32_t size) {
    const float meters = (sample - 0.5f * static_cast<float>(size - 1)) * field.spacing;
    return static_cast<int64_t>(std::llround(meters * 65536.0));
  };
  uint64_t truePositive = 0, falsePositive = 0, falseNegative = 0, truthForest = 0;
  for (uint32_t row = 0; row < rows; ++row) {
    for (uint32_t column = 0; column < columns; ++column) {
      uint32_t sum = 0;
      for (int b = 0; b < 4; ++b) {
        for (int a = 0; a < 4; ++a) {
          const float sx = static_cast<float>(column * cell) + (a + 0.5f) * cell / 4.0f - 0.5f;
          const float sz = static_cast<float>(row * cell) + (b + 0.5f) * cell / 4.0f - 0.5f;
          sum += meridian::densityAt(field, &geo, layer, kPlacementSeed,
                                     toFixed(sx, field.width), toFixed(sz, field.height));
        }
      }
      const bool ours = sum / 16 >= 32768;
      const bool real = truth[size_t{row} * columns + column] != 0;
      truthForest += real;
      truePositive += ours && real;
      falsePositive += ours && !real;
      falseNegative += !ours && real;
    }
  }
  const double precision = truePositive / std::max(1.0, double(truePositive + falsePositive));
  const double recall = truePositive / std::max(1.0, double(truePositive + falseNegative));
  const double f1 = precision + recall > 0.0 ? 2.0 * precision * recall / (precision + recall) : 0.0;
  const double n = double(columns) * rows;
  const double observed = (n - falsePositive - falseNegative) / n;
  const double ours = (truePositive + falsePositive) / n, real = truthForest / n;
  const double chance = ours * real + (1.0 - ours) * (1.0 - real);
  const double kappa = chance < 1.0 ? (observed - chance) / (1.0 - chance) : 0.0;
  std::printf("Forest score: kappa %.3f, precision %.3f, recall %.3f, F1 %.3f (cells %u, truth "
              "forest %.1f%%, ours %.1f%%)\n",
              kappa, precision, recall, f1, columns * rows, 100.0 * real, 100.0 * ours);
  return 0;
}

int main(int argc, char **argv) {
  const Options options = parseOptions(argc, argv);
  if (options.scoreForestPath) {
    return scoreForest(options);
  }
  // Scripted runs ignore the mouse and keyboard so results are repeatable.
  const bool benchmark = options.benchmarkSeconds > 0.0f;
  const bool interactive = options.frames == 0 && !benchmark;

  if (benchmark && !std::ofstream(options.benchmarkPath, std::ios::app)) {
    std::fprintf(stderr, "Can't write %s\n", options.benchmarkPath.c_str());
    return 1;
  }

  meridian::Window window(1280, 720, "Meridian", options.fullscreen);
#ifdef MERIDIAN_HAS_DLSS
  std::printf("%s\n", meridian::dlss::attribution());
  // NGX finds its snippets next to the binary (copied there by the build).
  const auto upscalerProvider = meridian::dlss::createProvider(
      std::filesystem::read_symlink("/proc/self/exe").parent_path());
#endif
  meridian::AaSettings aa{.method = options.aa,
                          .mode = options.dlssMode.value_or(meridian::DlssMode::Dlaa),
                          .preset = options.dlssPreset,
                          .rayReconstruction = options.aa == meridian::AaMethod::Dlss &&
                                               options.rayReconstruction,
                          .renderScale = options.renderScale.value_or(1.0f)};
#ifndef MERIDIAN_HAS_DLSS
  if (aa.method == meridian::AaMethod::Dlss) {
    std::printf("DLSS unavailable: built with MERIDIAN_DLSS=OFF; using TAA\n");
  }
#endif
#ifdef MERIDIAN_HAS_DLSS
  meridian::Renderer renderer(window, upscalerProvider.get(), aa);
#else
  meridian::Renderer renderer(window, nullptr, aa);
#endif
  window.setCursorCaptured(interactive);
  renderer.setOcclusionCulling(options.occlusion);
  renderer.setShadowMode(options.shadows);
  renderer.setBloom(options.bloom.value_or(meridian::Bloom{}));
  renderer.setEffects(options.effects.value_or(meridian::Effects{}));
  renderer.setVoxelFarShadows(options.voxelFarShadows);
  renderer.setGiMode(options.gi ? meridian::GiMode::Probes : meridian::GiMode::Off);
  if (options.albedoCapturePath) {
    renderer.enableAlbedoCapture();
  }
  renderer.setVoxelLod(options.voxelLod);
  renderer.setLodErrorPixels(options.lodError);
  renderer.setGrade(kStartingGrade);
  renderer.setCloudsEnabled(options.clouds);

  // Real elevation (swisstopo swissALTI3D, the Napf), or procedural hills.
  const auto heightPath = options.terrainPath.value_or(
      std::filesystem::path(MERIDIAN_ASSET_DIR) / "terrain" / "napf.height");
  meridian::Heightfield terrain;
  if (options.terrainPath || std::filesystem::exists(heightPath)) {
    terrain = meridian::loadHeightfield(heightPath);
  } else {
    std::printf("Missing assets/terrain/napf.height; terrain is procedural (see "
                "tools/fetch-sample-assets.sh)\n");
    terrain = meridian::generateHeightfield({});
  }
  renderer.setTerrain(terrain);
  // Its geo channels (meridian-cook --geo), which the causal placement
  // layers read.
  bool geoChannels = false;
  if (const auto geoPath = meridian::geoPathFor(heightPath); std::filesystem::exists(geoPath)) {
    try {
      const meridian::GeoChannels geo = meridian::loadGeoChannels(geoPath);
      if (meridian::geoMatches(geo, terrain)) {
        renderer.setGeoChannels(geo);
        geoChannels = true;
      } else {
        std::printf("%s does not match the heightfield; ignored\n", geoPath.c_str());
      }
    } catch (const std::exception &error) {
      std::printf("%s; ignored\n", error.what());
    }
  }
  const float floorKm = valleyFloorKm(terrain);
  // Megascans surfaces (docs/assets.md has what each one shows and why it
  // got its role): per role, the variants with their real-world tile sizes
  // and parallax depths (m: each surface's Megascans JSON displacementScale,
  // cm, / 100), and the mean linear color they are matched to.
  using meridian::TerrainRole;
  struct Role {
    TerrainRole role;
    meridian::Vec3 target;
    struct Variant {
      const char *id;
      float tileSize;
      float depth;
    };
    std::vector<Variant> variants;
  };
  const Role roles[] = {
      // Matched to the streamed grass mat's mean as rendered, so the mat
      // thins into a terrain of the same brightness (decisions.md).
      {TerrainRole::Grass,
       {0.037f, 0.058f, 0.0104f},
       {{"uncut_grass_oilpt20", 2.0f, 0.0177f},
        {"wild_grass_pjwgw0", 1.0f, 0.0219f},
        {"lawn_grass_sbykrmp0", 0.41f, 0.0107f}}},
      {TerrainRole::Clover, {0.06f, 0.111f, 0.028f}, {{"clover_vlzlbjon", 1.5f, 0.0267f}}},
      {TerrainRole::Mud, {0.07f, 0.046f, 0.021f}, {{"soil_mud_olsetpp0", 0.88f, 0.0251f}}},
      {TerrainRole::Soil,
       {0.13f, 0.095f, 0.05f},
       {{"soil_dirt_pjusf0", 1.0f, 0.0391f}, {"soil_dirt_peioh0", 1.0f, 0.0572f}}},
      {TerrainRole::ForestPath, {0.08f, 0.06f, 0.035f}, {{"forest_path_ugsnfawlw", 2.0f, 0.0355f}}},
      {TerrainRole::ForestFloor,
       {0.05f, 0.05f, 0.016f},
       {{"forest_floor_sfjmafua", 2.0f, 0.0292f}, {"forest_floor_xdfddec", 2.0f, 0.0372f}}},
      {TerrainRole::Gravel, {0.13f, 0.1f, 0.07f}, {{"gravel_on_soil_rmsigwp0", 0.39f, 0.0149f}}},
      {TerrainRole::Scree, {0.1f, 0.065f, 0.04f}, {{"gravel_on_soil_rmrk3sp0", 0.39f, 0.0156f}}},
      {TerrainRole::Rock,
       {0.13f, 0.108f, 0.085f},
       {{"rock_cliff_tg1mcgbl", 2.0f, 0.0891f}, {"layered_rock_vl0fdhdo", 3.0f, 0.1495f}}},
  };
  meridian::TerrainMaterial material;
  if (options.trees) {
    material.forestLayerId = kForestLayerId;
  }
  bool layersLoaded = true;
  for (const Role &role : roles) {
    for (const auto &[id, tileSize, depth] : role.variants) {
      auto layer = layersLoaded ? loadTerrainLayer(renderer, id, tileSize, depth, role.target)
                                : std::nullopt;
      layersLoaded = layer.has_value();
      if (layer) {
        material.layers[static_cast<size_t>(role.role)].push_back(*layer);
      }
    }
  }
  if (layersLoaded) {
    renderer.setTerrainMaterial(material);
  }

  // The start: a gentle ridge top (slope under 15 degrees within 20 m)
  // 1.5 km east of the Napf summit, looking at it. Sample assets from
  // tools/fetch-sample-assets.sh stand on the ground a few meters ahead.
  meridian::Camera start;
  start.position = {1119.0f, 0.0f, -431.0f};
  start.yaw = -1.55f;
  start.pitch = -0.1f;
  auto ground = [&](float forward, float right) {
    const meridian::Vec3 p =
        start.position + start.forward() * forward + start.right() * right;
    return meridian::Vec3{p.x, terrain.heightAt(p.x, p.z), p.z};
  };
  if (auto helmet = loadModel(renderer, "FlightHelmet/FlightHelmet.gltf")) {
    addModel(renderer, *helmet, meridian::translation(ground(3.0f, 0.0f)));
  }
  if (auto alpha = loadModel(renderer, "AlphaBlendModeTest/AlphaBlendModeTest.gltf")) {
    addModel(renderer, *alpha, meridian::translation(ground(7.0f, -3.0f)));
  }
  if (options.scatter > 0) {
    scatterModels(renderer, terrain, start.position, 150.0f, options.scatter, options.trees);
  }
  if (options.placement) {
    placeScenery(renderer, options.trees, options.farField, options.streamed, geoChannels);
  }

  meridian::Weather weather(1, meridian::weatherPreset(options.weather));
  if (options.weatherFront) {
    weather.setTarget(meridian::weatherPreset("overcast"), *options.weatherFront);
  }
  double gameSeconds = 0.0;

  meridian::Lighting lighting;
  lighting.atmosphere = atmosphereFor(options.haze.value_or(Haze::Summer), floorKm);
  if (options.sunDirection) {
    lighting.sunDirection = *options.sunDirection;
  }
  // CLI and defaults are the built-in look; the grade file (interactive runs
  // and --grade) overrides it except for an explicit --sun, --haze or --bloom.
  Look builtIn = lookWithSun(lighting.sunDirection, options.haze.value_or(Haze::Summer));
  builtIn.grade = kStartingGrade;
  builtIn.fovDegrees = options.fovDegrees.value_or(builtIn.fovDegrees);
  builtIn.bloom = options.bloom.value_or(meridian::Bloom{});
  builtIn.effects = options.effects.value_or(meridian::Effects{});
  Look look = builtIn;
  const std::filesystem::path gradePath = options.gradePath.value_or(defaultLookPath());
  if (options.gradePath || interactive) {
    if (auto loaded = loadLook(gradePath, builtIn)) {
      look = *loaded;
      if (options.sunDirection) {
        look.sunAzimuthDegrees = builtIn.sunAzimuthDegrees;
        look.sunElevationDegrees = builtIn.sunElevationDegrees;
      } else {
        lighting.sunDirection = sunDirection(look.sunAzimuthDegrees, look.sunElevationDegrees);
      }
      if (options.haze) {
        look.haze = *options.haze;
      } else {
        lighting.atmosphere = atmosphereFor(look.haze, floorKm);
      }
      if (options.fovDegrees) {
        look.fovDegrees = *options.fovDegrees;
      }
      if (options.bloom) {
        look.bloom = *options.bloom;
      } else {
        renderer.setBloom(look.bloom);
      }
      if (options.effects) {
        look.effects = *options.effects;
      } else {
        renderer.setEffects(look.effects);
      }
      renderer.setGrade(look.grade);
    } else if (options.gradePath) {
      throw std::runtime_error("cannot read grade file " + options.gradePath->string());
    }
  }
  GradePanel gradePanel(builtIn, look, gradePath);
  if (options.overlayGrade) {
    gradePanel.setVisible(window, true);
  }
  PerfOverlay perf;
  perf.setVisible(options.overlayPerf);
  meridian::Camera camera = start;
  camera.position = ground(0.0f, 0.0f) + meridian::Vec3{0.0f, 1.7f, 0.0f};
  if (options.camera) {
    camera = *options.camera;
  }
  // Scripted runs (motion and exposure) use a fixed step so they repeat
  // exactly.
  constexpr int kBenchmarkWarmupFrames = 60;
  constexpr float kScriptedStep = 1.0f / 60.0f;
  const meridian::Vec3 scriptedEnd = camera.position;
  const float scriptedYaw = camera.yaw;
  const float scriptedPitch = camera.pitch;
  auto scriptAngles = [&](int framesLeft) {
    if (options.cameraAngularVelocity) {
      camera.yaw = meridian::scriptedAngle(
          scriptedYaw, options.cameraAngularVelocity->first, kScriptedStep,
          framesLeft);
      camera.pitch = meridian::scriptedAngle(
          scriptedPitch, options.cameraAngularVelocity->second, kScriptedStep,
          framesLeft);
    }
  };
  scriptAngles(options.frames);
  if (options.cameraVelocity) {
    camera.position = meridian::scriptedPosition(
        scriptedEnd, *options.cameraVelocity, kScriptedStep, options.frames);
  }

  using Clock = std::chrono::steady_clock;
  auto lastFrame = Clock::now();
  auto fpsStart = lastFrame;
  int frames = 0;
  float gpuMsSum = 0.0f;
  int totalFrames = 0;
  bool walking = false;
  bool fWasDown = false;
  bool benchmarkDone = false;
  Clock::time_point referenceStart;
  bool benchmarkWritten = true;
  Clock::time_point benchmarkStart;

  while (!window.shouldClose() &&
         (interactive || (benchmark ? !benchmarkDone
                                   : totalFrames < options.frames + static_cast<int>(
                                                                     options.referenceSamples)))) {
    auto now = Clock::now();
    const float frameMs = std::chrono::duration<float, std::milli>(now - lastFrame).count();
    // Clamped so a long stall (loading, dragging the window) doesn't
    // teleport the camera.
    float deltaSeconds =
        std::min(std::chrono::duration<float>(now - lastFrame).count(), 0.1f);
    lastFrame = now;

    window.pollEvents();
    if (window.isKeyDown(meridian::Key::Escape)) {
      window.close();
    }
    if (totalFrames > 0) { // the first frame's time is loading
      perf.addFrame(frameMs, renderer);
      // Measure after pipelines and exposure have settled.
      if (benchmark && !perf.recording() && totalFrames == kBenchmarkWarmupFrames) {
        perf.startRecording();
        benchmarkStart = now;
      } else if (benchmark && perf.recording() &&
                 std::chrono::duration<float>(now - benchmarkStart).count() >=
                     options.benchmarkSeconds) {
        benchmarkWritten = perf.stopRecording(options.benchmarkPath);
        benchmarkDone = true;
      }
    }

    if (interactive) {
      gradePanel.handleToggle(window);
      perf.handleKeys(window);
      const bool fDown = window.isKeyDown(meridian::Key::F);
      if (fDown && !fWasDown && !gradePanel.visible()) {
        walking = !walking;
      }
      fWasDown = fDown;
      if (!gradePanel.visible()) {
        updateFlyCamera(camera, window, deltaSeconds,
                        walking ? &terrain : nullptr);
      }
    }
    if (benchmark && !options.route.empty()) {
      const float t = perf.recording()
                          ? std::chrono::duration<float>(now - benchmarkStart).count()
                          : 0.0f;
      const meridian::CameraPose pose = meridian::routePose(options.route, t);
      camera.position = pose.position;
      camera.yaw = pose.yaw;
      camera.pitch = pose.pitch;
    }
    ++totalFrames;
    scriptAngles(options.frames - totalFrames);
    if (options.cameraVelocity) {
      camera.position = meridian::scriptedPosition(
          scriptedEnd, *options.cameraVelocity, kScriptedStep,
          options.frames - totalFrames);
    }
    if (options.referenceSamples > 0 && totalFrames == options.frames + 1) {
      renderer.beginReference(options.referenceMode);
      referenceStart = Clock::now();
    }
    if (options.capturePath &&
        totalFrames == options.frames + static_cast<int>(options.referenceSamples)) {
      renderer.requestCapture(*options.capturePath);
      if (options.albedoCapturePath) {
        renderer.requestCapture(*options.albedoCapturePath, meridian::CaptureSource::DiffuseAlbedo);
      }
    } else if (options.captureSequence > 0 && totalFrames % options.captureSequence == 0) {
      char suffix[24];
      std::snprintf(suffix, sizeof(suffix), "_%03d.png",
                    totalFrames / options.captureSequence - 1);
      std::filesystem::path path = *options.capturePath;
      renderer.requestCapture(path.replace_filename(path.stem().string() + suffix));
    }
    if (gradePanel.visible() || perf.visible()) {
      renderer.beginOverlay();
      if (gradePanel.visible()) {
        const GradePanel::Changes changes = gradePanel.draw();
        if (changes.grade) {
          renderer.setGrade(gradePanel.grade());
        }
        if (changes.lighting) {
          lighting.sunDirection = sunDirection(gradePanel.look().sunAzimuthDegrees,
                                               gradePanel.look().sunElevationDegrees);
          lighting.atmosphere = atmosphereFor(gradePanel.look().haze, floorKm);
        }
        if (changes.effects) {
          renderer.setEffects(gradePanel.effects());
        }
        if (changes.bloom) {
          renderer.setBloom(gradePanel.look().bloom);
        }
      }
      if (perf.visible()) {
        perf.draw(gradePanel.visible());
      }
    }
    // Game time (wind, clouds, weather) stands still while the reference accumulates.
    const float frameSeconds = interactive || benchmark ? deltaSeconds
                               : totalFrames > options.frames ? 0.0f
                                                              : kScriptedStep;
    gameSeconds += static_cast<double>(frameSeconds) * options.timeScale;
    weather.update(gameSeconds);
    renderer.setWeather(weather);
    {
      int width = 0, height = 0;
      window.framebufferSize(width, height);
      constexpr float kRadians = std::numbers::pi_v<float> / 180.0f;
      camera.fovY = meridian::verticalFov(gradePanel.look().fovDegrees * kRadians,
                                          static_cast<float>(width) / static_cast<float>(height));
    }
    renderer.draw(camera, lighting, frameSeconds);

    ++frames;
    gpuMsSum += renderer.gpuFrameMs();
    if (now - fpsStart >= std::chrono::seconds(1)) {
      char gpuMs[16];
      std::snprintf(gpuMs, sizeof(gpuMs), "%.2f", gpuMsSum / frames);
      std::string title = "Meridian - " + std::to_string(frames) + " FPS, GPU " +
                          gpuMs + " ms";
      window.setTitle(title.c_str());
      frames = 0;
      gpuMsSum = 0.0f;
      fpsStart = now;
    }
  }

  if (perf.recording()) { // Esc or window close mid-recording
    benchmarkWritten = perf.stopRecording(benchmark ? options.benchmarkPath : std::filesystem::path{});
  }

  if (options.weatherMapPath) {
    renderer.dumpWeatherMap(*options.weatherMapPath);
  }
  if (options.referenceSamples > 0) {
    std::printf("Reference: %u spp, total %.1f s, noise %.2f%%\n", renderer.referenceSamples(),
                std::chrono::duration<double>(Clock::now() - referenceStart).count(),
                100.0 * renderer.referenceNoise());
  }
  if (!interactive) {
    renderer.printAmbient();
    std::printf("sun shadows: %u cascade renders, max %.2f ms\n", renderer.sunShadowRenderCount(),
                renderer.sunShadowMaxMs());
    const meridian::CullDrops drops = renderer.droppedCullEntries();
    std::printf("Rendered %d frames, GPU %.2f ms, %u drawn instances, %u clusters, "
                "%u triangles, %u terrain nodes, cells %u, far cells %u, voxel trees %u (bricks %u, voxels %u), bvh nodes %u, "
                "%u dropped by full "
                "cull lists (chunks %u, work items %u, clusters %u, terrain nodes %u, "
                "bvh nodes %u), placed %u, %u validation errors\n",
                totalFrames, renderer.gpuFrameMs(), renderer.drawnInstanceCount(),
                renderer.drawnClusterCount(), renderer.drawnTriangleCount(),
                renderer.drawnTerrainNodeCount(), renderer.drawnCellCount(),
                renderer.drawnFarCellCount(), renderer.drawnVoxelTreeCount(),
                renderer.drawnVoxelBrickCount(), renderer.drawnVoxelQuadCount(),
                renderer.visitedBvhNodeCount(), renderer.droppedCullEntryCount(), drops.chunks,
                drops.workItems, drops.clusters, drops.terrainNodes, drops.traversalNodes,
                renderer.placedInstanceCount(),
                renderer.validationErrorCount());
    if (renderer.drawnVoxelQuadCount() > 0) {
      std::printf("voxel quads %u: area %.0f px = %.2f x the render's pixels, %.1f%% under 1 px\n",
                  renderer.drawnVoxelQuadCount(), renderer.drawnVoxelQuadArea(),
                  renderer.drawnVoxelQuadComplexity(),
                  100.0 * renderer.drawnSubpixelVoxelQuads() / renderer.drawnVoxelQuadCount());
    }
    const meridian::Renderer::StreamStats stream = renderer.streamStats();
    std::printf("streamed: %u layers, window %ux%u, ring %u slots, %u live\n", stream.layers,
                stream.window, stream.window, stream.ringInstances, stream.live);
    std::printf("GPU passes:");
    const char *separator = " ";
    for (const meridian::PassTime &pass : renderer.gpuPassTimes()) {
      std::printf("%s%s %.2f", separator, pass.name, pass.ms);
      separator = ", ";
    }
    std::printf("\n");
    // Wind as reported in meteorology: where it comes from, clockwise from
    // north (-z).
    const meridian::WeatherState here = weather.stateAt(camera.position);
    const meridian::Vec3 toward = here.wind.direction;
    const float from = std::fmod(
        std::atan2(-toward.x, toward.z) * 180.0f / 3.14159265f + 360.0f, 360.0f);
    std::printf("Weather at the camera: coverage %.2f, type %.2f, wind %.1f m/s from "
                "%.0f deg, gustiness %.2f (game time %.0f s)\n",
                here.coverage, here.type, here.wind.speed10m, from, here.wind.gustiness,
                gameSeconds);
  }
  return renderer.validationErrorCount() == 0 && benchmarkWritten ? 0 : 1;
}
