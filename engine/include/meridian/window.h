#pragma once

struct GLFWwindow;

namespace meridian {

class Window {
public:
  Window(int width, int height, const char *title);
  ~Window();

  Window(const Window &) = delete;
  Window &operator=(const Window &) = delete;

  bool shouldClose() const;
  void pollEvents();

  GLFWwindow *handle() const { return handle_; }

private:
  GLFWwindow *handle_ = nullptr;
};

} // namespace meridian
