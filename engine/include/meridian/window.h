#pragma once

struct GLFWwindow;

namespace meridian {

enum class Key { Escape, W, A, S, D, Space, LeftShift, LeftControl };

struct MouseDelta {
  float x = 0.0f;
  float y = 0.0f;
};

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

  // Hides the cursor and locks it to the window for mouse-look.
  void setCursorCaptured(bool captured);
  // Mouse movement since the previous pollEvents().
  MouseDelta mouseDelta() const { return mouseDelta_; }

  GLFWwindow *handle() const { return handle_; }

private:
  GLFWwindow *handle_ = nullptr;
  double lastCursorX_ = 0.0;
  double lastCursorY_ = 0.0;
  bool hasLastCursor_ = false;
  MouseDelta mouseDelta_;
};

} // namespace meridian
