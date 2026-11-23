#include "leaf_trace.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <tuple>

namespace meridian::cook {

namespace {

double signedArea(const std::array<float, 2> &a, const std::array<float, 2> &b,
                  const std::array<float, 2> &c) {
  return 0.5 * ((static_cast<double>(b[0]) - a[0]) * (static_cast<double>(c[1]) - a[1]) -
                (static_cast<double>(c[0]) - a[0]) * (static_cast<double>(b[1]) - a[1]));
}

// Vertices of the filled marching-squares mesh, one per texel corner and one
// per crossed texel edge. Ids live in two rolling rows, so a 4K atlas needs no
// full-size tables; a crossing is computed from its two texels in a fixed
// order, so both cells sharing an edge get the same vertex.
class Tracer {
public:
  Tracer(const std::vector<uint8_t> &alpha, uint32_t width, uint32_t height, float threshold)
      : alpha_(alpha), width_(width), height_(height), threshold_(threshold),
        corner_{std::vector<uint32_t>(width, kNone), std::vector<uint32_t>(width, kNone)},
        across_{std::vector<uint32_t>(width, kNone), std::vector<uint32_t>(width, kNone)},
        down_(width, kNone) {}

  AtlasMesh run() {
    for (uint32_t y = 0; y + 1 < height_; ++y) {
      std::fill(down_.begin(), down_.end(), kNone);
      for (uint32_t x = 0; x + 1 < width_; ++x) {
        cell(x, y);
      }
      std::swap(corner_[0], corner_[1]);
      std::swap(across_[0], across_[1]);
      std::fill(corner_[1].begin(), corner_[1].end(), kNone);
      std::fill(across_[1].begin(), across_[1].end(), kNone);
    }
    return std::move(mesh_);
  }

private:
  static constexpr uint32_t kNone = UINT32_MAX;

  float value(uint32_t x, uint32_t y) const { return alpha_[size_t(y) * width_ + x]; }
  bool inside(uint32_t x, uint32_t y) const { return value(x, y) >= threshold_; }

  std::array<float, 2> texel(float x, float y) const {
    return {(x + 0.5f) / width_, (y + 0.5f) / height_};
  }

  uint32_t add(std::array<float, 2> uv) {
    mesh_.uv.push_back(uv);
    return static_cast<uint32_t>(mesh_.uv.size() - 1);
  }

  uint32_t cornerId(uint32_t x, uint32_t row, uint32_t y) {
    uint32_t &id = corner_[row][x];
    if (id == kNone) {
      id = add(texel(float(x), float(y)));
    }
    return id;
  }

  // Crossing between texel (x0, y0) and the next one along +x or +y.
  uint32_t crossingId(uint32_t &id, uint32_t x0, uint32_t y0, bool alongX) {
    if (id == kNone) {
      const float a = value(x0, y0);
      const float b = alongX ? value(x0 + 1, y0) : value(x0, y0 + 1);
      const float t = (threshold_ - a) / (b - a);
      id = add(alongX ? texel(x0 + t, float(y0)) : texel(float(x0), y0 + t));
    }
    return id;
  }

  void cell(uint32_t x, uint32_t y) {
    // Corners clockwise in image space: (x, y), (x+1, y), (x+1, y+1), (x, y+1).
    const bool in[4] = {inside(x, y), inside(x + 1, y), inside(x + 1, y + 1), inside(x, y + 1)};
    const int count = in[0] + in[1] + in[2] + in[3];
    if (count == 0) {
      return;
    }
    const auto corner = [&](int k) {
      switch (k) {
      case 0: return cornerId(x, 0, y);
      case 1: return cornerId(x + 1, 0, y);
      case 2: return cornerId(x + 1, 1, y + 1);
      default: return cornerId(x, 1, y + 1);
      }
    };
    // Edge k runs from corner k to corner k + 1.
    const auto edge = [&](int k) {
      switch (k) {
      case 0: return crossingId(across_[0][x], x, y, true);
      case 1: return crossingId(down_[x + 1], x + 1, y, false);
      case 2: return crossingId(across_[1][x], x, y + 1, true);
      default: return crossingId(down_[x], x, y, false);
      }
    };
    const bool saddle = count == 2 && in[0] == in[2];
    const float centre =
        (value(x, y) + value(x + 1, y) + value(x + 1, y + 1) + value(x, y + 1)) / 4.0f;
    if (saddle && centre < threshold_) {
      // Two separate corners: a triangle each.
      for (int k = 0; k < 4; ++k) {
        if (in[k]) {
          triangle(corner(k), edge(k), edge((k + 3) % 4));
        }
      }
      return;
    }
    uint32_t polygon[8];
    int n = 0;
    for (int k = 0; k < 4; ++k) {
      if (in[k]) {
        polygon[n++] = corner(k);
      }
      if (in[k] != in[(k + 1) % 4]) {
        polygon[n++] = edge(k);
      }
    }
    for (int i = 1; i + 1 < n; ++i) {
      triangle(polygon[0], polygon[i], polygon[i + 1]);
    }
  }

  void triangle(uint32_t a, uint32_t b, uint32_t c) {
    if (signedArea(mesh_.uv[a], mesh_.uv[b], mesh_.uv[c]) != 0.0) {
      mesh_.indices.insert(mesh_.indices.end(), {a, b, c});
    }
  }

  const std::vector<uint8_t> &alpha_;
  uint32_t width_, height_;
  float threshold_;
  // [0]: row y, [1]: row y + 1.
  std::vector<uint32_t> corner_[2], across_[2];
  std::vector<uint32_t> down_;
  AtlasMesh mesh_;
};

uint32_t root(std::vector<uint32_t> &parent, uint32_t i) {
  while (parent[i] != i) {
    i = parent[i] = parent[parent[i]];
  }
  return i;
}

} // namespace

AtlasMesh traceAtlas(const std::vector<uint8_t> &alpha, uint32_t width, uint32_t height,
                     float cutoff, float toleranceTexels, float minAreaTexels) {
  AtlasMesh traced = Tracer(alpha, width, height, cutoff * 255.0f).run();
  if (traced.indices.empty()) {
    return traced;
  }

  std::vector<float> positions;
  positions.reserve(traced.uv.size() * 3);
  for (const auto &uv : traced.uv) {
    positions.insert(positions.end(), {uv[0], uv[1], 0.0f});
  }
  std::vector<uint32_t> indices(traced.indices.size());
  const float error = toleranceTexels / static_cast<float>(std::max(width, height));
  indices.resize(meshopt_simplify(indices.data(), traced.indices.data(), traced.indices.size(),
                                  positions.data(), traced.uv.size(), sizeof(float) * 3, 0,
                                  error, meshopt_SimplifyErrorAbsolute, nullptr));

  // Connected parts (by shared vertices) under the minimum area are specks.
  std::vector<uint32_t> parent(traced.uv.size());
  std::iota(parent.begin(), parent.end(), 0u);
  for (size_t t = 0; t < indices.size(); t += 3) {
    parent[root(parent, indices[t + 1])] = root(parent, indices[t]);
    parent[root(parent, indices[t + 2])] = root(parent, indices[t]);
  }
  std::vector<double> area(traced.uv.size(), 0.0);
  for (size_t t = 0; t < indices.size(); t += 3) {
    area[root(parent, indices[t])] += std::abs(signedArea(
        traced.uv[indices[t]], traced.uv[indices[t + 1]], traced.uv[indices[t + 2]]));
  }
  const double minArea = minAreaTexels / (static_cast<double>(width) * height);

  AtlasMesh mesh;
  std::vector<uint32_t> remap(traced.uv.size(), UINT32_MAX);
  for (size_t t = 0; t < indices.size(); t += 3) {
    if (area[root(parent, indices[t])] < minArea) {
      continue;
    }
    for (int k = 0; k < 3; ++k) {
      uint32_t &id = remap[indices[t + k]];
      if (id == UINT32_MAX) {
        id = static_cast<uint32_t>(mesh.uv.size());
        mesh.uv.push_back(traced.uv[indices[t + k]]);
      }
      mesh.indices.push_back(id);
    }
  }
  return mesh;
}

double atlasArea(const AtlasMesh &mesh) {
  double area = 0.0;
  for (size_t t = 0; t < mesh.indices.size(); t += 3) {
    area += std::abs(
        signedArea(mesh.uv[mesh.indices[t]], mesh.uv[mesh.indices[t + 1]], mesh.uv[mesh.indices[t + 2]]));
  }
  return area;
}

namespace {

using Uv = std::array<double, 2>;

double cross(const Uv &a, const Uv &b) { return a[0] * b[1] - a[1] * b[0]; }
Uv minus(const Uv &a, const Uv &b) { return {a[0] - b[0], a[1] - b[1]}; }

bool before(const CardVertex &a, const CardVertex &b) {
  return std::tie(a.position, a.uv[0], a.uv[1]) < std::tie(b.position, b.uv[0], b.uv[1]);
}

// A point of a clipped polygon: an atlas vertex, a point on card edge `edge`
// at `t` along it (in its canonical order), or card corner `corner`.
struct Point {
  enum Kind { Atlas, Edge, Corner } kind;
  uint32_t id = 0; // atlas vertex, edge or corner
  double t = 0.0;
  Uv uv;           // in the tile's frame, for the clipping tests
};

// A polygon side: part of atlas edge (a, b) in canonical order, or of a card
// edge (a = UINT32_MAX).
struct Side {
  uint32_t a, b;
};

struct CardTriangle {
  const CardVertex *v[3];
  Uv uv[3];   // shifted into the tile
  // Edge k runs between corners k and k+1; from/to in canonical order.
  int from[3], to[3];
};

CardVertex lerp(const CardVertex &a, const CardVertex &b, double t) {
  CardVertex out;
  const float f = static_cast<float>(t);
  for (int c = 0; c < 3; ++c) {
    out.position[c] = a.position[c] + f * (b.position[c] - a.position[c]);
    out.normal[c] = a.normal[c] + f * (b.normal[c] - a.normal[c]);
  }
  for (int c = 0; c < 2; ++c) {
    out.uv[c] = a.uv[c] + f * (b.uv[c] - a.uv[c]);
  }
  return out;
}

void normalize(std::array<float, 3> &n) {
  const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (length > 0.0f) {
    for (float &c : n) {
      c /= length;
    }
  }
}

} // namespace

void clipToCards(const AtlasMesh &atlas, const std::vector<CardVertex> &vertices,
                 const std::vector<uint32_t> &indices, std::vector<CardVertex> &outVertices,
                 std::vector<uint32_t> &outIndices) {
  constexpr int kGrid = 256;
  std::vector<std::vector<uint32_t>> grid(kGrid * kGrid);
  const auto cellOf = [](double x) { return std::clamp(int(std::floor(x * kGrid)), 0, kGrid - 1); };
  for (uint32_t t = 0; t < atlas.indices.size() / 3; ++t) {
    double lo[2] = {1e9, 1e9}, hi[2] = {-1e9, -1e9};
    for (int k = 0; k < 3; ++k) {
      for (int c = 0; c < 2; ++c) {
        lo[c] = std::min<double>(lo[c], atlas.uv[atlas.indices[3 * t + k]][c]);
        hi[c] = std::max<double>(hi[c], atlas.uv[atlas.indices[3 * t + k]][c]);
      }
    }
    for (int y = cellOf(lo[1]); y <= cellOf(hi[1]); ++y) {
      for (int x = cellOf(lo[0]); x <= cellOf(hi[0]); ++x) {
        grid[y * kGrid + x].push_back(t);
      }
    }
  }
  std::vector<uint64_t> seen(atlas.indices.size() / 3, UINT64_MAX);
  uint64_t stamp = 0;
  std::vector<Point> polygon, clipped;
  std::vector<Side> sides, clippedSides;

  for (size_t c = 0; c + 2 < indices.size(); c += 3) {
    CardTriangle card;
    for (int k = 0; k < 3; ++k) {
      card.v[k] = &vertices[indices[c + k]];
    }
    for (int k = 0; k < 3; ++k) {
      const bool forward = !before(*card.v[(k + 1) % 3], *card.v[k]);
      card.from[k] = forward ? k : (k + 1) % 3;
      card.to[k] = forward ? (k + 1) % 3 : k;
    }
    double lo[2] = {1e9, 1e9}, hi[2] = {-1e9, -1e9};
    for (int k = 0; k < 3; ++k) {
      for (int a = 0; a < 2; ++a) {
        lo[a] = std::min<double>(lo[a], card.v[k]->uv[a]);
        hi[a] = std::max<double>(hi[a], card.v[k]->uv[a]);
      }
    }
    const Uv original[3] = {{card.v[0]->uv[0], card.v[0]->uv[1]},
                            {card.v[1]->uv[0], card.v[1]->uv[1]},
                            {card.v[2]->uv[0], card.v[2]->uv[1]}};
    const double area = cross(minus(original[1], original[0]), minus(original[2], original[0]));
    if (std::abs(area) < 1e-14) {
      continue;
    }
    // Inside a card edge: on the same side as the opposite corner.
    const double orientation = area > 0.0 ? 1.0 : -1.0;
    for (int ty = int(std::floor(lo[1])); ty <= int(std::ceil(hi[1])) - 1; ++ty) {
      for (int tx = int(std::floor(lo[0])); tx <= int(std::ceil(hi[0])) - 1; ++tx) {
        for (int k = 0; k < 3; ++k) {
          card.uv[k] = {original[k][0] - tx, original[k][1] - ty};
        }
        const auto inside = [&](int k, const Uv &p) {
          return orientation * cross(minus(card.uv[(k + 1) % 3], card.uv[k]),
                                     minus(p, card.uv[k])) >= 0.0;
        };
        ++stamp;
        for (int y = cellOf(lo[1] - ty); y <= cellOf(hi[1] - ty); ++y) {
          for (int x = cellOf(lo[0] - tx); x <= cellOf(hi[0] - tx); ++x) {
            for (uint32_t t : grid[y * kGrid + x]) {
              if (seen[t] == stamp) {
                continue;
              }
              seen[t] = stamp;
              polygon.clear();
              sides.clear();
              for (int k = 0; k < 3; ++k) {
                const uint32_t a = atlas.indices[3 * t + k], b = atlas.indices[3 * t + (k + 1) % 3];
                polygon.push_back({Point::Atlas, a, 0.0, {atlas.uv[a][0], atlas.uv[a][1]}});
                sides.push_back({std::min(a, b), std::max(a, b)});
              }
              // Sutherland-Hodgman against the card's three edges.
              for (int k = 0; k < 3 && !polygon.empty(); ++k) {
                clipped.clear();
                clippedSides.clear();
                const Uv &e0 = card.uv[card.from[k]], &e1 = card.uv[card.to[k]];
                for (size_t i = 0; i < polygon.size(); ++i) {
                  const Point &p = polygon[i], &q = polygon[(i + 1) % polygon.size()];
                  const bool pIn = inside(k, p.uv), qIn = inside(k, q.uv);
                  if (pIn) {
                    clipped.push_back(p);
                    clippedSides.push_back(sides[i]);
                  }
                  if (pIn == qIn) {
                    continue;
                  }
                  Point cut;
                  if (sides[i].a == UINT32_MAX) {
                    // Along another card edge: the crossing is their shared corner.
                    const int other = static_cast<int>(sides[i].b);
                    const int corner = (other + 1) % 3 == k ? k : (k + 1) % 3;
                    cut = {Point::Corner, static_cast<uint32_t>(corner), 0.0, card.uv[corner]};
                  } else {
                    const Uv a0 = {atlas.uv[sides[i].a][0], atlas.uv[sides[i].a][1]};
                    const Uv a1 = {atlas.uv[sides[i].b][0], atlas.uv[sides[i].b][1]};
                    const Uv d = minus(a1, a0), e = minus(e1, e0);
                    const double t = std::clamp(cross(minus(a0, e0), d) / cross(e, d), 0.0, 1.0);
                    cut = {Point::Edge, static_cast<uint32_t>(k), t,
                           {e0[0] + t * e[0], e0[1] + t * e[1]}};
                  }
                  clipped.push_back(cut);
                  // Leaving the card, the next side runs along card edge k.
                  clippedSides.push_back(pIn ? Side{UINT32_MAX, static_cast<uint32_t>(k)} : sides[i]);
                }
                std::swap(polygon, clipped);
                std::swap(sides, clippedSides);
              }
              if (polygon.size() < 3) {
                continue;
              }
              const uint32_t first = static_cast<uint32_t>(outVertices.size());
              for (const Point &p : polygon) {
                CardVertex v;
                if (p.kind == Point::Corner) {
                  v = *card.v[p.id];
                } else if (p.kind == Point::Edge) {
                  v = lerp(*card.v[card.from[p.id]], *card.v[card.to[p.id]], p.t);
                } else {
                  double w[3];
                  for (int k = 0; k < 3; ++k) {
                    w[k] = cross(minus(card.uv[(k + 2) % 3], card.uv[(k + 1) % 3]),
                                 minus(p.uv, card.uv[(k + 1) % 3])) / area;
                  }
                  v = {};
                  for (int k = 0; k < 3; ++k) {
                    for (int a = 0; a < 3; ++a) {
                      v.position[a] += static_cast<float>(w[k] * card.v[k]->position[a]);
                      v.normal[a] += static_cast<float>(w[k] * card.v[k]->normal[a]);
                    }
                  }
                  v.uv[0] = static_cast<float>(p.uv[0] + tx);
                  v.uv[1] = static_cast<float>(p.uv[1] + ty);
                }
                normalize(v.normal);
                outVertices.push_back(v);
              }
              // The atlas winds every leaf one way in UV; a card whose UVs are
              // mirrored must take them the other way to keep its own winding,
              // which double-sided shading reads as the side its normal faces.
              double polygonArea = 0.0;
              for (size_t i = 0; i < polygon.size(); ++i) {
                polygonArea += cross(polygon[i].uv, polygon[(i + 1) % polygon.size()].uv);
              }
              const bool flip = (polygonArea > 0.0) != (orientation > 0.0);
              for (uint32_t i = 1; i + 1 < polygon.size(); ++i) {
                if (flip) {
                  outIndices.insert(outIndices.end(), {first, first + i + 1, first + i});
                } else {
                  outIndices.insert(outIndices.end(), {first, first + i, first + i + 1});
                }
              }
            }
          }
        }
      }
    }
  }
}

} // namespace meridian::cook
