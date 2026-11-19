#include <meridian/renderer.h>
#include <meridian/window.h>

int main() {
  meridian::Window window(1280, 720, "Meridian");
  meridian::Renderer renderer(window);

  while (!window.shouldClose()) {
    window.pollEvents();
    renderer.draw();
  }
}
