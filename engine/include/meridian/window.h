#pragma once

struct GLFWwindow;

namespace meridian {

enum class Key { Escape };

class Window {
public:
  Window(int width, int height, const char *title);
  ~Window();

  Window(const Window &) = delete;
  Window &operator=(const Window &) = delete;

  bool shouldClose() const;
  void pollEvents();
  void close();
  void setTitle(const char *title);
  bool isKeyDown(Key key) const;

  GLFWwindow *handle() const { return handle_; }

private:
  GLFWwindow *handle_ = nullptr;
};

} // namespace meridian
