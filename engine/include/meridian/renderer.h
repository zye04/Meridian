#pragma once

#include <memory>

namespace meridian {

class Window;

class Renderer {
public:
  explicit Renderer(Window &window);
  ~Renderer();

  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace meridian
