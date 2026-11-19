#include <meridian/window.h>

#include <GLFW/glfw3.h>

#include <stdexcept>

namespace meridian {

Window::Window(int width, int height, const char *title) {
  if (!glfwInit()) {
    throw std::runtime_error("glfwInit failed");
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

  handle_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
  if (!handle_) {
    glfwTerminate();
    throw std::runtime_error("glfwCreateWindow failed");
  }
}

Window::~Window() {
  glfwDestroyWindow(handle_);
  glfwTerminate();
}

bool Window::shouldClose() const { return glfwWindowShouldClose(handle_); }

void Window::pollEvents() { glfwPollEvents(); }

} // namespace meridian

// GLFW's Wayland decorations (libdecor) pull in dbus and pango, which leak
// small one-time allocations we can't fix. Hide them from LeakSanitizer.
extern "C" const char *__lsan_default_suppressions() {
  return "leak:libdbus-1.so\n"
         "leak:libpango\n";
}
