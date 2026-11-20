#include <meridian/model.h>
#include <meridian/placement.h>
#include <meridian/renderer.h>
#include <meridian/terrain.h>
#include <meridian/texture.h>
#include <meridian/window.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
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

// Loads one cooked Poly Haven ground texture set from assets/terrain/<id>/.
// Returns nothing (after saying which file) if any map is missing.
std::optional<meridian::TerrainLayer>
loadTerrainLayer(meridian::Renderer &renderer, const char *id, float tileSize) {
  const auto directory = std::filesystem::path(MERIDIAN_ASSET_DIR) / "terrain" / id;
  const char *names[] = {"base_color", "normal", "arm", "height"};
  for (const char *name : names) {
    const auto path = directory / (std::string(name) + ".ktx2");
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

// Forests, lone trees on meadow slopes and rocks over the whole terrain,
// placed on the GPU at load (placement.h).
void placeScenery(meridian::Renderer &renderer) {
  using meridian::PlacementLayer;
  using meridian::Rotation;
  constexpr float kDegrees = 180.0f / 3.14159265f;
  std::vector<meridian::Prototype> prototypes;
  // (prototype, weight) per tree; the saplings make the forest's understory.
  std::vector<std::pair<uint32_t, float>> forest, lone;
  for (const auto &[tree, weight] :
       {std::pair{"tree_european_beech_01_a", 0.4f}, {"tree_english_oak_forest_01_a", 0.25f},
        {"tree_norway_maple_01_a", 0.2f}, {"tree_european_beech_saplings_01_a", 0.1f},
        {"tree_rowan_sapling_a", 0.05f}}) {
    const std::string name = tree;
    for (uint32_t id : addPrototypes(renderer, prototypes,
                                     "source/megascans/" + name + "/" + name + ".gltf", false)) {
      forest.emplace_back(id, weight);
      if (name.find("sapling") == std::string::npos) {
        lone.emplace_back(id, weight);
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
  // One mask: the forest inside it, lone trees in its exact complement.
  constexpr float kMaskFrequency = 1.0f / 1500.0f, kMaskThreshold = 0.45f, kMaskEdge = 0.08f;
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
  renderer.setPlacement(prototypes, layers, 1);
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
                               "--camera-velocity x y z, --scatter n, --placement off, "
                               "--no-occlusion, "
                               "--haze clean|summer|summer-hazy, "
                               "--lod-error px, "
                               "--windowed, "
                               "--capture file.png|file.pfm, --frames n)");
    }
  }
  // Enough frames for temporal accumulation and exposure to settle.
  if (options.capturePath && options.frames == 0) {
    options.frames = 120;
  }
  if (options.cameraVelocity && options.frames == 0) {
    throw std::runtime_error("--camera-velocity needs --capture or --frames");
  }
  return options;
}

} // namespace

int main(int argc, char **argv) {
  const Options options = parseOptions(argc, argv);
  // Scripted runs ignore the mouse and keyboard so results are repeatable.
  const bool interactive = options.frames == 0;

  meridian::Window window(1280, 720, "Meridian", options.fullscreen);
  meridian::Renderer renderer(window);
  window.setCursorCaptured(interactive);
  renderer.setOcclusionCulling(options.occlusion);
  renderer.setLodErrorPixels(options.lodError);

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
  // Tile sizes are the scans' real-world sizes.
  const auto soil = loadTerrainLayer(renderer, "forest_ground_04", 3.15f);
  auto needles = soil ? loadTerrainLayer(renderer, "forest_leaves_04", 1.5f)
                      : std::nullopt;
  // Its AO map follows the darkness already in its base color (correlation
  // 0.55), so at full strength the gaps between needles count twice.
  if (needles) {
    needles->occlusionStrength = 0.5f;
  }
  const auto rock = needles ? loadTerrainLayer(renderer, "rock_face_03", 2.7f)
                            : std::nullopt;
  if (rock) {
    renderer.setTerrainMaterial({*soil, *needles, *rock});
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
  if (options.cameraVelocity) {
    camera.position = camera.position - *options.cameraVelocity *
                                            (kScriptedStep * options.frames);
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
    if (options.cameraVelocity) {
      camera.position = camera.position + *options.cameraVelocity * kScriptedStep;
    }
    if (options.capturePath && totalFrames == options.frames) {
      renderer.requestCapture(*options.capturePath);
    }
    renderer.draw(camera, lighting, interactive ? deltaSeconds : kScriptedStep);

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

  if (!interactive) {
    const meridian::CullDrops drops = renderer.droppedCullEntries();
    std::printf("Rendered %d frames, GPU %.2f ms, %u drawn instances, %u clusters, "
                "%u triangles, %u terrain nodes, cells %u, %u dropped by full cull lists "
                "(chunks %u, work items %u, clusters %u, terrain nodes %u), "
                "placed %u, %u validation errors\n",
                totalFrames, renderer.gpuFrameMs(), renderer.drawnInstanceCount(),
                renderer.drawnClusterCount(), renderer.drawnTriangleCount(),
                renderer.drawnTerrainNodeCount(), renderer.drawnCellCount(),
                renderer.droppedCullEntryCount(), drops.chunks, drops.workItems, drops.clusters,
                drops.terrainNodes, renderer.placedInstanceCount(),
                renderer.validationErrorCount());
    std::printf("GPU passes:");
    const char *separator = " ";
    for (const meridian::PassTime &pass : renderer.gpuPassTimes()) {
      std::printf("%s%s %.2f", separator, pass.name, pass.ms);
      separator = ", ";
    }
    std::printf("\n");
  }
  return renderer.validationErrorCount() == 0 ? 0 : 1;
}
