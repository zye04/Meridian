#include <meridian/model.h>
#include <meridian/placement.h>
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
                  meridian::Vec3 center, float radius, int count) {
  const char *ids[] = {"rock_moss_set_01", "rock_moss_set_02", "boulder_01",
                       "namaqualand_boulder_04", "tree_stump_01", "tree_stump_02",
                       "dead_tree_trunk_02", "dry_branches_medium_01", "pine_roots",
                       "fern_02", "nettle_plant", "shrub_02", "dandelion_01",
                       "fir_sapling", "pine_sapling_small", "island_tree_01"};
  std::vector<std::string> paths;
  for (const char *id : ids) {
    paths.push_back(std::string(id) + "/" + id + "_2k.gltf");
  }
  // Megascans Megaplants, imported by tools/fetch-sample-assets.sh.
  for (const char *tree : {"tree_european_beech_01_a", "tree_english_oak_forest_01_a",
                           "tree_norway_maple_01_a", "tree_european_beech_saplings_01_a",
                           "tree_rowan_sapling_a"}) {
    paths.push_back(std::string("source/megascans/") + tree + "/" + tree + ".gltf");
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
void placeScenery(meridian::Renderer &renderer) {
  using meridian::PlacementLayer;
  using meridian::Rotation;
  constexpr float kDegrees = 180.0f / 3.14159265f;
  std::vector<meridian::Prototype> prototypes;
  // (prototype, forest weight, lone weight). Only the beech is a mature,
  // broad-crowned tree (26.6 m, 13 m crown); the forest-grown oak is a
  // 4 m wide pole and the maple 8 m wide, so they stay a minority, and the
  // oak never stands alone. Saplings would take canopy slots (3 m tall).
  std::vector<std::pair<uint32_t, float>> forest, lone;
  for (const auto &[tree, forestWeight, loneWeight] :
       {std::tuple{"tree_european_beech_01_a", 0.7f, 0.75f},
        {"tree_norway_maple_01_a", 0.2f, 0.25f}, {"tree_english_oak_forest_01_a", 0.1f, 0.0f}}) {
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
  if (lone.empty()) {
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

// Command-line options for deterministic, automated runs, e.g.
//   game --camera 1119 4354 -431 -1.55 -0.35 --sun 0.5 0.55 -0.4 --capture out.png
struct Options {
  std::optional<meridian::Camera> camera;
  std::optional<meridian::Vec3> sunDirection;
  // Scripted runs move the camera at this velocity (m/s), ending at --camera.
  std::optional<meridian::Vec3> cameraVelocity;
  std::optional<std::pair<float, float>> cameraAngularVelocity;
  // Also saves <capture stem>_NNN.png every this many frames (sequences).
  int captureSequence = 0;
  std::optional<std::filesystem::path> capturePath;
  // Poly Haven models and Megascans trees to scatter around the start.
  int scatter = 0;
  // Forests, lone trees and rocks over the terrain (placeScenery).
  bool placement = true;
  bool occlusion = true;
  // Clean: Bruneton's very clean defaults (for A/B).
  enum class Haze { Clean, Summer, SummerHazy } haze = Haze::Summer;
  // Largest simplification error on screen in pixels (0: full detail).
  float lodError = 1.0f;
  // TAA unless --aa dlss; DLSS falls back to TAA when unsupported.
  meridian::AaMethod aa = meridian::AaMethod::Taa;
  // Unset: Quality under Ray Reconstruction (RR DLAA costs 4.7 ms at
  // 1440p, over its 3 ms budget), else DLAA.
  std::optional<meridian::DlssMode> dlssMode;
  char dlssPreset = 0; // 0: the DLSS module's per-mode default
  // Ray Reconstruction under DLSS (opt-in: it fails the motion gates,
  // decisions.md).
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
    } else if (arg == "--camera-yaw-rate") {
      options.cameraAngularVelocity = {number(i), 0.0f};
    } else if (arg == "--capture-sequence") {
      options.captureSequence = static_cast<int>(number(i));
    } else if (arg == "--capture" && i + 1 < argc) {
      options.capturePath = argv[++i];
    } else if (arg == "--windowed") {
      options.fullscreen = false;
    } else if (arg == "--haze" && i + 1 < argc) {
      const std::string haze = argv[++i];
      if (haze == "clean") {
        options.haze = Options::Haze::Clean;
      } else if (haze == "summer") {
        options.haze = Options::Haze::Summer;
      } else if (haze == "summer-hazy") {
        options.haze = Options::Haze::SummerHazy;
      } else {
        throw std::runtime_error("--haze takes clean, summer or summer-hazy");
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
    } else if (arg == "--scatter") {
      options.scatter = static_cast<int>(number(i));
    } else if (arg == "--placement" && i + 1 < argc &&
               std::string(argv[i + 1]) == "off") {
      options.placement = false;
      ++i;
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
                               "--scatter n, --placement off, --no-occlusion, "
                               "--haze clean|summer|summer-hazy, "
                               "--lod-error px, --render-scale s, --aa dlss|taa, "
                               "--dlss-mode dlaa|quality|balanced|performance, "
                               "--dlss-preset E|F|J|K|L|M, --rr on|off, "
                               "--windowed, "
                               "--weather clear|fair|broken|overcast, --time-scale x, "
                               "--weather-front seconds, --clouds on|off, --dump-weather-map file.png, "
                               "--capture file.png|file.pfm, --capture-sequence n, --frames n)");
    }
  }
  // Enough frames for temporal accumulation and exposure to settle.
  if ((options.capturePath || options.weatherMapPath) && options.frames == 0) {
    options.frames = 120;
  }
  if ((options.cameraVelocity || options.cameraAngularVelocity) &&
      options.frames == 0) {
    throw std::runtime_error(
        "--camera-velocity and --camera-angular-velocity need --capture or "
        "--frames");
  }
  if (options.captureSequence > 0 && !options.capturePath) {
    throw std::runtime_error("--capture-sequence needs --capture");
  }
  return options;
}

} // namespace

int main(int argc, char **argv) {
  const Options options = parseOptions(argc, argv);
  // Scripted runs ignore the mouse and keyboard so results are repeatable.
  const bool interactive = options.frames == 0;

  meridian::Window window(1280, 720, "Meridian", options.fullscreen);
#ifdef MERIDIAN_HAS_DLSS
  std::printf("%s\n", meridian::dlss::attribution());
  // NGX finds its snippets next to the binary (copied there by the build).
  const auto upscalerProvider = meridian::dlss::createProvider(
      std::filesystem::read_symlink("/proc/self/exe").parent_path());
  meridian::Renderer renderer(window, upscalerProvider.get());
#else
  meridian::Renderer renderer(window);
#endif
  window.setCursorCaptured(interactive);
  renderer.setOcclusionCulling(options.occlusion);
  renderer.setLodErrorPixels(options.lodError);
  // A starting look toward the Chlorophyll Engine frames
  // (assets/reference/chlorophyll): AgX alone is flat and desaturated for
  // daylight foliage, and the cooked leaf colour leans blue.
  renderer.setGrade({.contrast = 1.4f,
                     .saturation = 1.4f,
                     .greens = {.hueShiftDegrees = -12.0f, .saturation = 1.15f}});
  meridian::AaSettings aa{.method = options.aa,
                          .mode = options.dlssMode.value_or(options.rayReconstruction
                                                                ? meridian::DlssMode::Quality
                                                                : meridian::DlssMode::Dlaa),
                          .preset = options.dlssPreset,
                          .rayReconstruction = options.aa == meridian::AaMethod::Dlss &&
                                               options.rayReconstruction,
                          .renderScale = options.renderScale.value_or(1.0f)};
  // The Renderer logs its own fallback when feature creation fails.
  if (aa.method == meridian::AaMethod::Dlss) {
#ifdef MERIDIAN_HAS_DLSS
    if (aa.rayReconstruction && !upscalerProvider->supports(aa)) {
      std::printf("DLSS Ray Reconstruction unavailable; using Super Resolution\n");
      aa.rayReconstruction = false;
    }
    if (!aa.rayReconstruction && !options.dlssMode) {
      aa.mode = meridian::DlssMode::Dlaa;
    }
    if (!upscalerProvider->supports(aa)) {
      std::printf("DLSS unavailable: NGX reports no Super Resolution; using TAA\n");
    }
#else
    std::printf("DLSS unavailable: built with MERIDIAN_DLSS=OFF; using TAA\n");
#endif
  }
  if (aa.method != meridian::AaMethod::Taa || options.renderScale) {
    renderer.setAntiAliasing(aa);
  }
  renderer.setCloudsEnabled(options.clouds);

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
  // got its role): per role, the variants with their real-world tile sizes,
  // and the mean linear color they are matched to.
  using meridian::TerrainRole;
  struct Role {
    TerrainRole role;
    meridian::Vec3 target;
    std::vector<std::pair<const char *, float>> variants;
  };
  const Role roles[] = {
      {TerrainRole::Grass,
       {0.085f, 0.12f, 0.024f},
       {{"uncut_grass_oilpt20", 2.0f}, {"wild_grass_pjwgw0", 1.0f}, {"lawn_grass_sbykrmp0", 0.41f}}},
      {TerrainRole::Clover, {0.06f, 0.111f, 0.028f}, {{"clover_vlzlbjon", 1.5f}}},
      {TerrainRole::Mud, {0.07f, 0.046f, 0.021f}, {{"soil_mud_olsetpp0", 0.88f}}},
      {TerrainRole::Soil,
       {0.13f, 0.095f, 0.05f},
       {{"soil_dirt_pjusf0", 1.0f}, {"soil_dirt_peioh0", 1.0f}}},
      {TerrainRole::ForestPath, {0.08f, 0.06f, 0.035f}, {{"forest_path_ugsnfawlw", 2.0f}}},
      {TerrainRole::ForestFloor,
       {0.05f, 0.05f, 0.016f},
       {{"forest_floor_sfjmafua", 2.0f}, {"forest_floor_xdfddec", 2.0f}}},
      {TerrainRole::Gravel, {0.13f, 0.1f, 0.07f}, {{"gravel_on_soil_rmsigwp0", 0.39f}}},
      {TerrainRole::Scree, {0.1f, 0.065f, 0.04f}, {{"gravel_on_soil_rmrk3sp0", 0.39f}}},
      {TerrainRole::Rock,
       {0.13f, 0.108f, 0.085f},
       {{"rock_cliff_tg1mcgbl", 2.0f}, {"layered_rock_vl0fdhdo", 3.0f}}},
  };
  meridian::TerrainMaterial material;
  material.forest = {kMaskFrequency, kMaskThreshold, kMaskEdge, kPlacementSeed};
  bool layersLoaded = true;
  for (const Role &role : roles) {
    for (const auto &[id, tileSize] : role.variants) {
      auto layer = layersLoaded ? loadTerrainLayer(renderer, id, tileSize, role.target)
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
    scatterModels(renderer, terrain, start.position, 150.0f, options.scatter);
  }
  if (options.placement) {
    placeScenery(renderer);
  }

  meridian::Weather weather(1, meridian::weatherPreset(options.weather));
  if (options.weatherFront) {
    weather.setTarget(meridian::weatherPreset("overcast"), *options.weatherFront);
  }
  double gameSeconds = 0.0;

  meridian::Lighting lighting;
  if (options.haze == Options::Haze::Summer) {
    lighting.atmosphere = summerHaze(0.10f, 0.5f);
  } else if (options.haze == Options::Haze::SummerHazy) {
    lighting.atmosphere = summerHaze(0.2f, 1.5f);
  }
  if (options.sunDirection) {
    lighting.sunDirection = *options.sunDirection;
  }
  meridian::Camera camera = start;
  camera.position = ground(0.0f, 0.0f) + meridian::Vec3{0.0f, 1.7f, 0.0f};
  if (options.camera) {
    camera = *options.camera;
  }
  // Scripted runs (motion and exposure) use a fixed step so they repeat
  // exactly.
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

  while (!window.shouldClose() &&
         (interactive || totalFrames < options.frames)) {
    auto now = Clock::now();
    // Clamped so a long stall (loading, dragging the window) doesn't
    // teleport the camera.
    float deltaSeconds =
        std::min(std::chrono::duration<float>(now - lastFrame).count(), 0.1f);
    lastFrame = now;

    window.pollEvents();
    if (window.isKeyDown(meridian::Key::Escape)) {
      window.close();
    }

    if (interactive) {
      const bool fDown = window.isKeyDown(meridian::Key::F);
      if (fDown && !fWasDown) {
        walking = !walking;
      }
      fWasDown = fDown;
      updateFlyCamera(camera, window, deltaSeconds,
                      walking ? &terrain : nullptr);
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
    } else if (options.captureSequence > 0 && totalFrames % options.captureSequence == 0) {
      char suffix[24];
      std::snprintf(suffix, sizeof(suffix), "_%03d.png",
                    totalFrames / options.captureSequence - 1);
      std::filesystem::path path = *options.capturePath;
      renderer.requestCapture(path.replace_filename(path.stem().string() + suffix));
    }
    const float frameSeconds = interactive ? deltaSeconds : kScriptedStep;
    gameSeconds += static_cast<double>(frameSeconds) * options.timeScale;
    weather.update(gameSeconds);
    renderer.setWeather(weather);
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

  if (options.weatherMapPath) {
    renderer.dumpWeatherMap(*options.weatherMapPath);
  }
  if (!interactive) {
    renderer.printAmbient();
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
  return renderer.validationErrorCount() == 0 ? 0 : 1;
}
