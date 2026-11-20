#include "overlay.h"
#include "perf_overlay.h"

#include <fstream>

#include <meridian/model.h>
#include <meridian/placement.h>
#include <meridian/renderer.h>
#include <meridian/terrain.h>
#include <meridian/texture.h>
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
// (assets/reference/chlorophyll): AgX alone is flat and desaturated for
// daylight foliage, and the cooked leaf colour leans blue.
const meridian::Grade kStartingGrade{
    .contrast = 1.4f,
    .saturation = 1.4f,
    .greens = {.hueShiftDegrees = -12.0f, .saturation = 1.15f}};

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

// One mask: the forest inside it, lone trees in its exact complement; the
// terrain material's forest floor follows it too. kPlacementSeed seeds it.
constexpr float kMaskFrequency = 1.0f / 1500.0f, kMaskThreshold = 0.45f, kMaskEdge = 0.08f;
constexpr uint32_t kPlacementSeed = 1;

// Forests, lone trees on meadow slopes and rocks over the whole terrain,
// placed on the GPU at load (placement.h).
void placeScenery(meridian::Renderer &renderer, bool trees) {
  using meridian::PlacementLayer;
  using meridian::Rotation;
  constexpr float kDegrees = 180.0f / 3.14159265f;
  std::vector<meridian::Prototype> prototypes;
  // (prototype, forest weight, lone weight). Only the beech is a mature,
  // broad-crowned tree (26.6 m, 13 m crown); the forest-grown oak is a
  // 4 m wide pole and the maple 8 m wide, so they stay a minority, and the
  // oak never stands alone. Saplings would take canopy slots (3 m tall).
  std::vector<std::pair<uint32_t, float>> forest, lone;
  const std::vector<std::tuple<const char *, float, float>> megaplants =
      trees ? std::vector<std::tuple<const char *, float, float>>{
                  {"tree_european_beech_01_a", 0.7f, 0.75f},
                  {"tree_norway_maple_01_a", 0.2f, 0.25f},
                  {"tree_english_oak_forest_01_a", 0.1f, 0.0f}}
            : std::vector<std::tuple<const char *, float, float>>{};
  for (const auto &[tree, forestWeight, loneWeight] : megaplants) {
    const std::string name = tree;
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

  std::vector<PlacementLayer> layers;
  if (!forest.empty()) {
    layers.push_back({1, forest, 6.0f,
                      {0.0f, 42.0f, -1e9f, 1e9f, 0.15f, kMaskFrequency, kMaskThreshold,
                       kMaskEdge, 1.0f / 120.0f, 0.3f},
                      0.8f * treeScale, 1.2f * treeScale, Rotation::Yaw, 0.3f});
    layers.push_back({2, lone, 40.0f,
                      {0.0f, 25.0f, -1e9f, 1e9f, 0.3f, kMaskFrequency, kMaskThreshold,
                       -kMaskEdge, 0.0f, 0.0f},
                      0.9f * treeScale, 1.3f * treeScale, Rotation::Yaw, 0.3f});
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
  }
  if (layers.empty()) {
    std::printf("Nothing to place (see tools/fetch-sample-assets.sh)\n");
    return;
  }
  renderer.setPlacement(prototypes, layers, kPlacementSeed);
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

meridian::Atmosphere atmosphereFor(Haze haze) {
  switch (haze) {
  case Haze::Summer:
    return summerHaze(0.10f, 0.5f);
  case Haze::SummerHazy:
    return summerHaze(0.2f, 1.5f);
  case Haze::Clean:
    break;
  }
  return {};
}

// Command-line options for deterministic, automated runs, e.g.
//   game --camera 1119 4354 -431 -1.55 -0.35 --sun 0.5 0.55 -0.4 --capture out.png
struct Options {
  std::optional<meridian::Camera> camera;
  std::optional<meridian::Vec3> sunDirection;
  // Grade file (see overlay.h) to load in any run; interactive runs also
  // load the default one.
  std::optional<std::filesystem::path> gradePath;
  bool overlayGrade = false;
  bool overlayPerf = false;
  // Timed run with real frame times and no input; the CSV goes to benchmarkPath.
  float benchmarkSeconds = 0.0f;
  std::filesystem::path benchmarkPath;
  // Scripted runs move the camera at this velocity (m/s), ending at --camera.
  std::optional<meridian::Vec3> cameraVelocity;
  std::optional<std::pair<float, float>> cameraAngularVelocity;
  std::optional<std::filesystem::path> capturePath;
  // Poly Haven models and Megascans trees to scatter around the start.
  int scatter = 0;
  // Forests, lone trees and rocks over the terrain (placeScenery).
  bool placement = true;
  // Trees are hidden by default (--trees): no tree prototypes, no forest
  // or lone-tree layers, no forest floor in the terrain material.
  bool trees = false;
  bool occlusion = true;
  std::optional<meridian::Bloom> bloom;
  // Clean: Bruneton's very clean defaults (for A/B). Unset: summer.
  std::optional<Haze> haze;
  // Largest simplification error on screen in pixels (0: full detail).
  float lodError = 1.0f;
  // DLSS (DLAA + Ray Reconstruction) unless --aa taa; falls back to Super
  // Resolution DLAA, then TAA, when unsupported.
  meridian::AaMethod aa = meridian::AaMethod::Dlss;
  // Unset: DLAA (RR DLAA costs 4.7 ms at 1440p; optimised from the most
  // demanding configuration down, decisions.md).
  std::optional<meridian::DlssMode> dlssMode;
  char dlssPreset = 0; // 0: the DLSS module's per-mode default
  // Ray Reconstruction under DLSS: it removes the ray-traced shadow
  // shimmer, decisions.md.
  bool rayReconstruction = true;
  // TAA's render size over display size, in (0, 1].
  std::optional<float> renderScale;
  // Fullscreen by default: consistent size for GPU timings, and the way the
  // game is played.
  bool fullscreen = true;
  // Frames to render before exiting; 0 runs until the window closes.
  int frames = 0;
};

Options parseOptions(int argc, char **argv) {
  Options options;
  auto number = [&](int &i) {
    if (++i >= argc) {
      throw std::runtime_error("missing value for option");
    }
    return std::stof(argv[i]);
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
    } else if (arg == "--bloom") {
      options.bloom = meridian::Bloom{number(i), number(i)};
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
    } else if (arg == "--trees") {
      options.trees = true;
    } else if (arg == "--scatter") {
      options.scatter = static_cast<int>(number(i));
    } else if (arg == "--placement" && i + 1 < argc &&
               std::string(argv[i + 1]) == "off") {
      options.placement = false;
      ++i;
    } else if (arg == "--frames") {
      options.frames = static_cast<int>(number(i));
    } else {
      throw std::runtime_error("unknown option " + arg +
                               " (use --camera x y z yaw pitch, --sun x y z, "
                               "--camera-velocity x y z, "
                               "--camera-angular-velocity yaw pitch, --scatter n, --placement off, --trees, "
                               "--no-occlusion, "
                               "--haze clean|summer|summer-hazy, --grade file.json, --overlay grade|perf, --benchmark seconds file.csv, "
                               "--lod-error px, --render-scale s, --aa dlss|taa, "
                               "--dlss-mode dlaa|quality|balanced|performance, "
                               "--dlss-preset E|F|J|K|L|M, --rr on|off, "
                               "--windowed, "
                               "--capture file.png|file.pfm, --frames n)");
    }
  }
  // Enough frames for temporal accumulation and exposure to settle.
  if (options.capturePath && options.frames == 0 && options.benchmarkSeconds == 0.0f) {
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
  return options;
}

} // namespace

int main(int argc, char **argv) {
  const Options options = parseOptions(argc, argv);
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
  renderer.setBloom(options.bloom.value_or(meridian::Bloom{}));
  renderer.setLodErrorPixels(options.lodError);
  renderer.setGrade(kStartingGrade);

  // Real elevation (swisstopo swissALTI3D, the Napf), or procedural hills.
  const auto heightPath = std::filesystem::path(MERIDIAN_ASSET_DIR) / "terrain" / "napf.height";
  meridian::Heightfield terrain;
  if (std::filesystem::exists(heightPath)) {
    terrain = meridian::loadHeightfield(heightPath);
  } else {
    std::printf("Missing assets/terrain/napf.height; terrain is procedural (see "
                "tools/fetch-sample-assets.sh)\n");
    terrain = meridian::generateHeightfield({});
  }
  renderer.setTerrain(terrain);
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
      {TerrainRole::Grass,
       {0.085f, 0.12f, 0.024f},
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
    material.forest = {kMaskFrequency, kMaskThreshold, kMaskEdge, kPlacementSeed};
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
    placeScenery(renderer, options.trees);
  }

  meridian::Lighting lighting;
  lighting.atmosphere = atmosphereFor(options.haze.value_or(Haze::Summer));
  if (options.sunDirection) {
    lighting.sunDirection = *options.sunDirection;
  }
  // CLI and defaults are the built-in look; the grade file (interactive runs
  // and --grade) overrides it except for an explicit --sun, --haze or --bloom.
  Look builtIn = lookWithSun(lighting.sunDirection, options.haze.value_or(Haze::Summer));
  builtIn.grade = kStartingGrade;
  builtIn.bloom = options.bloom.value_or(meridian::Bloom{});
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
        lighting.atmosphere = atmosphereFor(look.haze);
      }
      if (options.bloom) {
        look.bloom = *options.bloom;
      } else {
        renderer.setBloom(look.bloom);
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
  bool benchmarkWritten = true;
  Clock::time_point benchmarkStart;

  while (!window.shouldClose() &&
         (interactive || (benchmark ? !benchmarkDone : totalFrames < options.frames))) {
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
    ++totalFrames;
    scriptAngles(options.frames - totalFrames);
    if (options.cameraVelocity) {
      camera.position = meridian::scriptedPosition(
          scriptedEnd, *options.cameraVelocity, kScriptedStep,
          options.frames - totalFrames);
    }
    if (options.capturePath && totalFrames == options.frames) {
      renderer.requestCapture(*options.capturePath);
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
          lighting.atmosphere = atmosphereFor(gradePanel.look().haze);
        }
        if (changes.bloom) {
          renderer.setBloom(gradePanel.look().bloom);
        }
      }
      if (perf.visible()) {
        perf.draw(gradePanel.visible());
      }
    }
    renderer.draw(camera, lighting, interactive || benchmark ? deltaSeconds : kScriptedStep);

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

  if (!interactive) {
    const meridian::CullDrops drops = renderer.droppedCullEntries();
    std::printf("Rendered %d frames, GPU %.2f ms, %u drawn instances, %u clusters, "
                "%u triangles, %u terrain nodes, cells %u, bvh nodes %u, %u dropped by full "
                "cull lists (chunks %u, work items %u, clusters %u, terrain nodes %u, "
                "bvh nodes %u), placed %u, %u validation errors\n",
                totalFrames, renderer.gpuFrameMs(), renderer.drawnInstanceCount(),
                renderer.drawnClusterCount(), renderer.drawnTriangleCount(),
                renderer.drawnTerrainNodeCount(), renderer.drawnCellCount(),
                renderer.visitedBvhNodeCount(), renderer.droppedCullEntryCount(), drops.chunks,
                drops.workItems, drops.clusters, drops.terrainNodes, drops.traversalNodes,
                renderer.placedInstanceCount(),
                renderer.validationErrorCount());
    std::printf("GPU passes:");
    const char *separator = " ";
    for (const meridian::PassTime &pass : renderer.gpuPassTimes()) {
      std::printf("%s%s %.2f", separator, pass.name, pass.ms);
      separator = ", ";
    }
    std::printf("\n");
  }
  return renderer.validationErrorCount() == 0 && benchmarkWritten ? 0 : 1;
}
