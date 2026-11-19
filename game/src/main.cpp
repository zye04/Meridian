#include <meridian/renderer.h>
#include <meridian/window.h>

#include <chrono>
#include <string>

int main() {
  meridian::Window window(1280, 720, "Meridian");
  meridian::Renderer renderer(window);

  using Clock = std::chrono::steady_clock;
  auto fpsStart = Clock::now();
  int frames = 0;

  while (!window.shouldClose()) {
    window.pollEvents();
    if (window.isKeyDown(meridian::Key::Escape)) {
      window.close();
    }

    renderer.draw();

    ++frames;
    if (Clock::now() - fpsStart >= std::chrono::seconds(1)) {
      std::string title = "Meridian - " + std::to_string(frames) + " FPS";
      window.setTitle(title.c_str());
      frames = 0;
      fpsStart = Clock::now();
    }
  }
}
