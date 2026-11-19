#include <meridian/model.h>
#include <meridian/renderer.h>
#include <meridian/terrain.h>
#include <meridian/texture.h>
#include <meridian/window.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr float kMoveSpeed = 30.0f;
constexpr float kSprintMultiplier = 5.0f;
constexpr float kMouseSensitivity = 0.002f;
constexpr float kMaxPitch = 1.55f;

void updateFlyCamera(meridian::Camera &camera, const meridian::Window &window,
                     float deltaSeconds) {
  using meridian::Key;

  meridian::MouseDelta mouse = window.mouseDelta();
  camera.yaw += mouse.x * kMouseSensitivity;
  camera.pitch = std::clamp(camera.pitch - mouse.y * kMouseSensitivity,
                            -kMaxPitch, kMaxPitch);

  meridian::Vec3 move;
  if (window.isKeyDown(Key::W)) {
    move = move + camera.forward();
  }
  if (window.isKeyDown(Key::S)) {
    move = move - camera.forward();
  }
  if (window.isKeyDown(Key::D)) {
    move = move + camera.right();
  }
  if (window.isKeyDown(Key::A)) {
    move = move - camera.right();
  }
  if (window.isKeyDown(Key::Space)) {
    move.y += 1.0f;
  }
  if (window.isKeyDown(Key::LeftControl)) {
    move.y -= 1.0f;
  }
  if (meridian::length(move) == 0.0f) {
    return;
  }

  float speed = kMoveSpeed;
  if (window.isKeyDown(Key::LeftShift)) {
    speed *= kSprintMultiplier;
  }
  camera.position =
      camera.position + meridian::normalize(move) * (speed * deltaSeconds);
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
int scatterModels(meridian::Renderer &renderer,
                  const meridian::TerrainSettings &terrain, meridian::Vec3 center,
                  float radius, int count) {
  const char *ids[] = {"rock_moss_set_01", "rock_moss_set_02", "boulder_01",
                       "namaqualand_boulder_04", "tree_stump_01", "tree_stump_02",
                       "dead_tree_trunk_02", "dry_branches_medium_01", "pine_roots",
                       "fern_02", "nettle_plant", "shrub_02", "dandelion_01",
                       "fir_sapling", "pine_sapling_small"};
  std::vector<meridian::Model> models;
  for (const char *id : ids) {
    const std::string path = std::string(id) + "/" + id + "_2k.gltf";
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
    const float y = meridian::terrainHeight(terrain, x, z) - 0.05f;
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

// Command-line options for deterministic, automated runs, e.g.
//   game --camera 0 300 700 0 -0.35 --sun 0.5 0.55 -0.4 --capture out.png
struct Options {
  std::optional<meridian::Camera> camera;
  std::optional<meridian::Vec3> sunDirection;
  // Scripted runs move the camera at this velocity (m/s), ending at --camera.
  std::optional<meridian::Vec3> cameraVelocity;
  std::optional<std::filesystem::path> capturePath;
  // Poly Haven models to scatter on the ground around the start.
  int scatter = 0;
  bool occlusion = true;
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
    } else if (arg == "--no-occlusion") {
      options.occlusion = false;
    } else if (arg == "--scatter") {
      options.scatter = static_cast<int>(number(i));
    } else if (arg == "--frames") {
      options.frames = static_cast<int>(number(i));
    } else {
      throw std::runtime_error("unknown option " + arg +
                               " (use --camera x y z yaw pitch, --sun x y z, "
                               "--camera-velocity x y z, --scatter n, --no-occlusion, "
                               "--windowed, "
                               "--capture file.png, --frames n)");
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

  const meridian::TerrainSettings terrainSettings;
  meridian::MaterialData terrainMaterial;
  terrainMaterial.terrain = true;
  meridian::MeshId terrain =
      renderer.uploadMesh(meridian::generateTerrain(terrainSettings),
                          renderer.createMaterial(terrainMaterial));
  renderer.addInstance(terrain, meridian::Mat4::identity());
  // Tile sizes are the scans' real-world sizes.
  const auto soil = loadTerrainLayer(renderer, "forest_ground_04", 3.15f);
  const auto needles = soil ? loadTerrainLayer(renderer, "forest_leaves_04", 1.5f)
                            : std::nullopt;
  const auto rock = needles ? loadTerrainLayer(renderer, "rock_face_03", 2.7f)
                            : std::nullopt;
  if (rock) {
    renderer.setTerrainMaterial({*soil, *needles, *rock});
  }

  // Sample assets from tools/fetch-sample-assets.sh, on the ground a few
  // meters in front of the start.
  const meridian::Vec3 start = {0.0f, 0.0f, 700.0f};
  auto ground = [&](float x, float z) {
    return meridian::Vec3{x, meridian::terrainHeight(terrainSettings, x, z), z};
  };
  if (auto helmet = loadModel(renderer, "FlightHelmet/FlightHelmet.gltf")) {
    addModel(renderer, *helmet, meridian::translation(ground(0.0f, 697.0f)));
  }
  if (auto alpha = loadModel(renderer, "AlphaBlendModeTest/AlphaBlendModeTest.gltf")) {
    addModel(renderer, *alpha, meridian::translation(ground(-3.0f, 693.0f)));
  }
  if (options.scatter > 0) {
    scatterModels(renderer, terrainSettings, start, 150.0f, options.scatter);
  }

  meridian::Lighting lighting;
  if (options.sunDirection) {
    lighting.sunDirection = *options.sunDirection;
  }
  meridian::Camera camera;
  camera.position = ground(start.x, start.z) + meridian::Vec3{0.0f, 1.7f, 0.0f};
  camera.pitch = -0.1f;
  if (options.camera) {
    camera = *options.camera;
  }
  // Scripted motion uses a fixed step so runs repeat exactly.
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
      updateFlyCamera(camera, window, deltaSeconds);
    }
    ++totalFrames;
    if (options.cameraVelocity) {
      camera.position = camera.position + *options.cameraVelocity * kScriptedStep;
    }
    if (options.capturePath && totalFrames == options.frames) {
      renderer.requestCapture(*options.capturePath);
    }
    renderer.draw(camera, lighting);

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
    std::printf("Rendered %d frames, GPU %.2f ms, %u drawn instances, "
                "%u validation errors\n",
                totalFrames, renderer.gpuFrameMs(), renderer.drawnInstanceCount(),
                renderer.validationErrorCount());
  }
  return renderer.validationErrorCount() == 0 ? 0 : 1;
}
