#pragma once

struct GLFWwindow;

namespace meridian {

enum class Key { Escape, W, A, S, D, F, Space, LeftShift, LeftControl };

struct MouseDelta {
  float x = 0.0f;
  float y = 0.0f;
};

class Window {
public:
  // Fullscreen covers the primary monitor at its current mode (the
  // constructor waits for the compositor to apply it); width and
  // height then only apply if the window is later made windowed.
  Window(int width, int height, const char *title, bool fullscreen = false);
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

  // The size to render at: the monitor mode when fullscreen (Wayland
  // reports the new window's tiled size until its first frame is
  // presented, which would make the renderer build its targets twice),
  // else the framebuffer.
  void framebufferSize(int &width, int &height) const;

  GLFWwindow *handle() const { return handle_; }

private:
  GLFWwindow *handle_ = nullptr;
  int fullscreenWidth_ = 0;
  int fullscreenHeight_ = 0;
  double lastCursorX_ = 0.0;
  double lastCursorY_ = 0.0;
  bool hasLastCursor_ = false;
  MouseDelta mouseDelta_;
};

} // namespace meridian
