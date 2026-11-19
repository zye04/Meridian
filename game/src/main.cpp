#include <meridian/renderer.h>
#include <meridian/terrain.h>
#include <meridian/window.h>

#include <chrono>
#include <string>
#include <vector>

int main() {
  meridian::Window window(1280, 720, "Meridian");
  meridian::Renderer renderer(window);

  meridian::MeshId terrain =
      renderer.uploadMesh(meridian::generateTerrain({}));
  std::vector<meridian::DrawItem> scene = {{terrain}};

  meridian::Camera camera;
  camera.position = {0.0f, 300.0f, 700.0f};
  camera.pitch = -0.35f;

  using Clock = std::chrono::steady_clock;
  auto fpsStart = Clock::now();
  int frames = 0;

  while (!window.shouldClose()) {
    window.pollEvents();
    if (window.isKeyDown(meridian::Key::Escape)) {
      window.close();
    }

    renderer.draw(camera, scene);

    ++frames;
    if (Clock::now() - fpsStart >= std::chrono::seconds(1)) {
      std::string title = "Meridian - " + std::to_string(frames) + " FPS";
      window.setTitle(title.c_str());
      frames = 0;
      fpsStart = Clock::now();
    }
  }
}
