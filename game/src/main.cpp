#include <meridian/renderer.h>
#include <meridian/terrain.h>
#include <meridian/window.h>

#include <algorithm>
#include <chrono>
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

} // namespace

int main() {
  meridian::Window window(1280, 720, "Meridian");
  meridian::Renderer renderer(window);
  window.setCursorCaptured(true);

  meridian::MeshId terrain =
      renderer.uploadMesh(meridian::generateTerrain({}));
  std::vector<meridian::DrawItem> scene = {{terrain}};

  meridian::Camera camera;
  camera.position = {0.0f, 300.0f, 700.0f};
  camera.pitch = -0.35f;

  using Clock = std::chrono::steady_clock;
  auto lastFrame = Clock::now();
  auto fpsStart = lastFrame;
  int frames = 0;

  while (!window.shouldClose()) {
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

    updateFlyCamera(camera, window, deltaSeconds);
    renderer.draw(camera, scene);

    ++frames;
    if (now - fpsStart >= std::chrono::seconds(1)) {
      std::string title = "Meridian - " + std::to_string(frames) + " FPS";
      window.setTitle(title.c_str());
      frames = 0;
      fpsStart = now;
    }
  }
}
