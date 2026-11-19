#include <meridian/window.h>

#include <GLFW/glfw3.h>

#include <stdexcept>

namespace meridian {

namespace {

int toGlfwKey(Key key) {
  switch (key) {
  case Key::Escape:
    return GLFW_KEY_ESCAPE;
  case Key::W:
    return GLFW_KEY_W;
  case Key::A:
    return GLFW_KEY_A;
  case Key::S:
    return GLFW_KEY_S;
  case Key::D:
    return GLFW_KEY_D;
  case Key::Space:
    return GLFW_KEY_SPACE;
  case Key::LeftShift:
    return GLFW_KEY_LEFT_SHIFT;
  case Key::LeftControl:
    return GLFW_KEY_LEFT_CONTROL;
  }
  return GLFW_KEY_UNKNOWN;
}

} // namespace

Window::Window(int width, int height, const char *title, bool fullscreen) {
  if (!glfwInit()) {
    throw std::runtime_error("glfwInit failed");
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

  GLFWmonitor *monitor = fullscreen ? glfwGetPrimaryMonitor() : nullptr;
  if (const GLFWvidmode *mode = monitor ? glfwGetVideoMode(monitor) : nullptr) {
    width = mode->width;
    height = mode->height;
  }
  handle_ = glfwCreateWindow(width, height, title, monitor, nullptr);
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

void Window::pollEvents() {
  glfwPollEvents();

  double x = 0.0;
  double y = 0.0;
  glfwGetCursorPos(handle_, &x, &y);
  mouseDelta_ = {};
  if (hasLastCursor_) {
    mouseDelta_.x = static_cast<float>(x - lastCursorX_);
    mouseDelta_.y = static_cast<float>(y - lastCursorY_);
  }
  lastCursorX_ = x;
  lastCursorY_ = y;
  hasLastCursor_ = true;
}

void Window::close() { glfwSetWindowShouldClose(handle_, GLFW_TRUE); }

void Window::setTitle(const char *title) { glfwSetWindowTitle(handle_, title); }

bool Window::isKeyDown(Key key) const {
  return glfwGetKey(handle_, toGlfwKey(key)) == GLFW_PRESS;
}

void Window::setCursorCaptured(bool captured) {
  glfwSetInputMode(handle_, GLFW_CURSOR,
                   captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
  // Raw motion skips OS pointer acceleration, which mouse-look should not have.
  if (glfwRawMouseMotionSupported()) {
    glfwSetInputMode(handle_, GLFW_RAW_MOUSE_MOTION,
                     captured ? GLFW_TRUE : GLFW_FALSE);
  }
  // The cursor position jumps when the mode changes; don't count that as
  // movement.
  hasLastCursor_ = false;
}

} // namespace meridian

// GLFW's Wayland decorations (libdecor) pull in dbus and pango, which leak
// small one-time allocations we can't fix. Hide them from LeakSanitizer.
extern "C" const char *__lsan_default_suppressions() {
  return "leak:libdbus-1.so\n"
         "leak:libpango\n";
}
