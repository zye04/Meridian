#include <meridian/model.h>
#include <meridian/renderer.h>
#include <meridian/terrain.h>
#include <meridian/window.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
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

// Command-line options for deterministic, automated runs, e.g.
//   game --camera 0 300 700 0 -0.35 --sun 0.5 0.55 -0.4 --capture out.png
struct Options {
  std::optional<meridian::Camera> camera;
  std::optional<meridian::Vec3> sunDirection;
  std::optional<std::filesystem::path> capturePath;
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
    } else if (arg == "--capture" && i + 1 < argc) {
      options.capturePath = argv[++i];
    } else if (arg == "--frames") {
      options.frames = static_cast<int>(number(i));
    } else {
      throw std::runtime_error("unknown option " + arg +
                               " (use --camera x y z yaw pitch, --sun x y z, "
                               "--capture file.png, --frames n)");
    }
  }
  // Enough frames for temporal accumulation and exposure to settle.
  if (options.capturePath && options.frames == 0) {
    options.frames = 120;
  }
  return options;
}

} // namespace

int main(int argc, char **argv) {
  const Options options = parseOptions(argc, argv);
  // Scripted runs ignore the mouse and keyboard so results are repeatable.
  const bool interactive = options.frames == 0;

  meridian::Window window(1280, 720, "Meridian");
  meridian::Renderer renderer(window);
  window.setCursorCaptured(interactive);

  meridian::MaterialData terrainMaterial;
  terrainMaterial.proceduralTerrain = true;
  meridian::MeshId terrain =
      renderer.uploadMesh(meridian::generateTerrain({}),
                          renderer.createMaterial(terrainMaterial));
  std::vector<meridian::DrawItem> scene = {{terrain}};

  // Sample assets from tools/fetch-sample-assets.sh (AlphaBlendModeTest must
  // be cooked first), placed a few meters in front of the starting camera.
  struct SampleModel {
    const char *path;
    meridian::Vec3 position;
  };
  const SampleModel samples[] = {
      {"FlightHelmet/FlightHelmet.gltf", {0.0f, 298.4f, 697.0f}},
      {"AlphaBlendModeTest/AlphaBlendModeTest.gltf", {-3.0f, 298.4f, 695.0f}},
  };
  for (const SampleModel &sample : samples) {
    const auto path = std::filesystem::path(MERIDIAN_ASSET_DIR) / sample.path;
    if (!std::filesystem::exists(path)) {
      std::printf("Missing %s; see tools/fetch-sample-assets.sh\n", sample.path);
      continue;
    }
    meridian::Model model = meridian::loadGltf(renderer, path);
    const meridian::Mat4 placement = meridian::translation(sample.position);
    for (meridian::DrawItem item : model.drawItems) {
      item.transform = placement * item.transform;
      scene.push_back(item);
    }
  }

  meridian::Lighting lighting;
  if (options.sunDirection) {
    lighting.sunDirection = *options.sunDirection;
  }
  meridian::Camera camera;
  camera.position = {0.0f, 300.0f, 700.0f};
  camera.pitch = -0.35f;
  if (options.camera) {
    camera = *options.camera;
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
    if (options.capturePath && totalFrames == options.frames) {
      renderer.requestCapture(*options.capturePath);
    }
    renderer.draw(camera, lighting, scene);

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
    std::printf("Rendered %d frames, GPU %.2f ms, %u validation errors\n",
                totalFrames, renderer.gpuFrameMs(),
                renderer.validationErrorCount());
  }
  return renderer.validationErrorCount() == 0 ? 0 : 1;
}
