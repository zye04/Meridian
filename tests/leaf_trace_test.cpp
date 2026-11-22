#include <doctest/doctest.h>

#include "leaf_trace.h"

#include <cmath>
#include <numbers>

using meridian::cook::AtlasMesh;
using meridian::cook::atlasArea;
using meridian::cook::traceAtlas;

namespace {

constexpr uint32_t kSize = 256;

// Alpha 255 where inner <= distance from (cx, cy) < outer, else 0.
std::vector<uint8_t> ring(float cx, float cy, float inner, float outer) {
  std::vector<uint8_t> alpha(kSize * kSize, 0);
  for (uint32_t y = 0; y < kSize; ++y) {
    for (uint32_t x = 0; x < kSize; ++x) {
      const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
      if (d >= inner && d < outer) {
        alpha[y * kSize + x] = 255;
      }
    }
  }
  return alpha;
}

bool contains(const AtlasMesh &mesh, size_t t, float u, float v) {
  const auto &a = mesh.uv[mesh.indices[t]], &b = mesh.uv[mesh.indices[t + 1]],
             &c = mesh.uv[mesh.indices[t + 2]];
  const auto side = [&](const std::array<float, 2> &p, const std::array<float, 2> &q) {
    return (q[0] - p[0]) * (v - p[1]) - (q[1] - p[1]) * (u - p[0]);
  };
  const float s0 = side(a, b), s1 = side(b, c), s2 = side(c, a);
  return (s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0);
}

double signedArea(const AtlasMesh &mesh, size_t t) {
  const auto &a = mesh.uv[mesh.indices[t]], &b = mesh.uv[mesh.indices[t + 1]],
             &c = mesh.uv[mesh.indices[t + 2]];
  return 0.5 * ((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]));
}

} // namespace

TEST_CASE("traceAtlas fills an opaque disk") {
  const AtlasMesh mesh = traceAtlas(ring(128, 128, 0, 80), kSize, kSize, 0.5f);
  const double pi = std::numbers::pi;
  CHECK(atlasArea(mesh) * kSize * kSize == doctest::Approx(pi * 80 * 80).epsilon(0.02));
  CHECK(mesh.indices.size() / 3 < 400);
  for (size_t t = 0; t < mesh.indices.size(); t += 3) {
    CHECK(std::abs(signedArea(mesh, t)) > 0.0);
  }
}

TEST_CASE("traceAtlas keeps holes") {
  const AtlasMesh mesh = traceAtlas(ring(128, 128, 40, 90), kSize, kSize, 0.5f);
  const double pi = std::numbers::pi;
  CHECK(atlasArea(mesh) * kSize * kSize ==
        doctest::Approx(pi * (90 * 90 - 40 * 40)).epsilon(0.02));
  for (size_t t = 0; t < mesh.indices.size(); t += 3) {
    CHECK_FALSE(contains(mesh, t, 0.5f, 0.5f));
  }
}

TEST_CASE("traceAtlas drops specks") {
  std::vector<uint8_t> alpha = ring(128, 128, 0, 80);
  for (uint32_t y = 9; y < 12; ++y) {
    for (uint32_t x = 9; x < 12; ++x) {
      alpha[y * kSize + x] = 255;
    }
  }
  const AtlasMesh mesh = traceAtlas(alpha, kSize, kSize, 0.5f);
  const AtlasMesh disk = traceAtlas(ring(128, 128, 0, 80), kSize, kSize, 0.5f);
  CHECK(atlasArea(mesh) == doctest::Approx(atlasArea(disk)).epsilon(0.02));
  for (const auto &uv : mesh.uv) {
    CHECK(std::hypot(uv[0] * kSize - 10.5f, uv[1] * kSize - 10.5f) > 6.0f);
  }
}

TEST_CASE("traceAtlas of transparent alpha is empty") {
  const AtlasMesh mesh = traceAtlas(std::vector<uint8_t>(kSize * kSize, 0), kSize, kSize, 0.5f);
  CHECK(mesh.indices.empty());
}
