#include <doctest/doctest.h>

#include "coverage.h"
#include "leaf_trace.h"

#include <cmath>
#include <map>
#include <numbers>
#include <set>

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

namespace {

using meridian::cook::CardVertex;
using meridian::cook::clipToCards;

// The unit square in xz (y up), split along its 0-2 diagonal.
struct Card {
  std::vector<CardVertex> vertices;
  std::vector<uint32_t> indices{0, 1, 2, 0, 2, 3};
};

Card squareCard(float u0 = 0.0f, float v0 = 0.0f, float uvSize = 1.0f) {
  Card card;
  const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  for (const auto &c : corners) {
    card.vertices.push_back(
        {{c[0], 0.0f, c[1]}, {0.0f, 1.0f, 0.0f}, {u0 + c[0] * uvSize, v0 + c[1] * uvSize}});
  }
  return card;
}

struct Clipped {
  std::vector<CardVertex> vertices;
  std::vector<uint32_t> indices;
};

Clipped clip(const AtlasMesh &atlas, const Card &card) {
  Clipped out;
  clipToCards(atlas, card.vertices, card.indices, out.vertices, out.indices);
  return out;
}

double area3(const Clipped &c) {
  double sum = 0.0;
  for (size_t t = 0; t < c.indices.size(); t += 3) {
    const auto &a = c.vertices[c.indices[t]].position, &b = c.vertices[c.indices[t + 1]].position,
               &d = c.vertices[c.indices[t + 2]].position;
    const double e[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const double f[3] = {d[0] - a[0], d[1] - a[1], d[2] - a[2]};
    sum += 0.5 * std::hypot(e[1] * f[2] - e[2] * f[1], e[2] * f[0] - e[0] * f[2],
                            e[0] * f[1] - e[1] * f[0]);
  }
  return sum;
}

// Connected parts, joining vertices whose positions are bit-identical.
size_t components(const Clipped &c) {
  std::map<std::array<float, 3>, size_t> ids;
  std::vector<size_t> parent;
  const auto id = [&](uint32_t v) {
    auto [it, inserted] = ids.emplace(c.vertices[v].position, parent.size());
    if (inserted) {
      parent.push_back(parent.size());
    }
    return it->second;
  };
  const auto find = [&](size_t i) {
    while (parent[i] != i) {
      i = parent[i] = parent[parent[i]];
    }
    return i;
  };
  for (size_t t = 0; t < c.indices.size(); t += 3) {
    const size_t a = id(c.indices[t]);
    parent[find(id(c.indices[t + 1]))] = find(a);
    parent[find(id(c.indices[t + 2]))] = find(a);
  }
  std::set<size_t> roots;
  for (size_t i = 0; i < parent.size(); ++i) {
    roots.insert(find(i));
  }
  return roots.size();
}

AtlasMesh disk() { return traceAtlas(ring(128, 128, 0, 80), kSize, kSize, 0.5f); }

} // namespace

TEST_CASE("clipToCards maps the atlas onto a flat card") {
  const AtlasMesh atlas = disk();
  const Clipped out = clip(atlas, squareCard());
  CHECK(area3(out) == doctest::Approx(atlasArea(atlas)).epsilon(1e-4));
  for (const CardVertex &v : out.vertices) {
    CHECK(v.position[1] == 0.0f);
    CHECK(v.normal[1] == doctest::Approx(1.0f));
  }
}

TEST_CASE("clipToCards keeps a leaf across the card's diagonal in one piece") {
  CHECK(components(clip(disk(), squareCard())) == 1);
}

TEST_CASE("clipToCards follows a bent card") {
  // Vertex 3 rotated 30 degrees about the 0-2 diagonal, out of the plane.
  Card card = squareCard();
  const float s = std::sin(std::numbers::pi_v<float> / 6), c = std::cos(std::numbers::pi_v<float> / 6);
  // Its offset from the diagonal is (-0.5, 0, 0.5); rotate that about the axis.
  card.vertices[3].position = {0.5f - 0.5f * c, std::sqrt(0.5f) * s, 0.5f + 0.5f * c};
  const Clipped out = clip(disk(), card);
  const auto plane = [&](uint32_t a, uint32_t b, uint32_t d, const std::array<float, 3> &p) {
    const auto &pa = card.vertices[a].position, &pb = card.vertices[b].position,
               &pd = card.vertices[d].position;
    const double e[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
    const double f[3] = {pd[0] - pa[0], pd[1] - pa[1], pd[2] - pa[2]};
    double n[3] = {e[1] * f[2] - e[2] * f[1], e[2] * f[0] - e[0] * f[2], e[0] * f[1] - e[1] * f[0]};
    const double len = std::hypot(n[0], n[1], n[2]);
    return std::abs(((p[0] - pa[0]) * n[0] + (p[1] - pa[1]) * n[1] + (p[2] - pa[2]) * n[2]) / len);
  };
  for (size_t t = 0; t < out.indices.size(); t += 3) {
    bool onA = true, onB = true;
    for (int k = 0; k < 3; ++k) {
      const auto &p = out.vertices[out.indices[t + k]].position;
      onA = onA && plane(0, 1, 2, p) < 1e-5;
      onB = onB && plane(0, 2, 3, p) < 1e-5;
    }
    CHECK((onA || onB));
  }
  CHECK(components(out) == 1);
}

TEST_CASE("clipToCards wraps UVs outside [0,1]") {
  const AtlasMesh atlas = disk();
  const Clipped out = clip(atlas, squareCard(-1.0f, 0.0f));
  CHECK(area3(out) == doctest::Approx(atlasArea(atlas)).epsilon(1e-4));
  for (const CardVertex &v : out.vertices) {
    CHECK(v.uv[0] >= -1.0f);
    CHECK(v.uv[0] <= 0.0f);
  }
}

TEST_CASE("clipToCards clips to the card") {
  // A full atlas spans the texel centres: [0.5, 255.5] / 256. The card shows
  // the UV quarter [0, 0.5]^2 on 1 m^2, 4 m^2 per UV unit squared.
  const AtlasMesh atlas = traceAtlas(std::vector<uint8_t>(kSize * kSize, 255), kSize, kSize, 0.5f);
  const Clipped out = clip(atlas, squareCard(0.0f, 0.0f, 0.5f));
  const double side = 0.5 - 0.5 / kSize;
  CHECK(area3(out) == doctest::Approx(side * side * 4.0).epsilon(1e-4));
}

TEST_CASE("projectedCoverage of half a square") {
  using meridian::cook::projectedCoverage;
  const std::vector<std::array<float, 3>> positions = {
      {0, 0, 0}, {0.5f, 0, 0}, {0.5f, 1, 0}, {0, 1, 0}, {1, 0, 0}};
  CHECK(projectedCoverage(positions, {0, 1, 2, 0, 2, 3}, 0, 0, 1, 1, 256) ==
        doctest::Approx(0.5).epsilon(0.01));
  CHECK(projectedCoverage(positions, {0, 4, 3}, 0, 0, 1, 1, 256) ==
        doctest::Approx(0.5).epsilon(0.02));
}
