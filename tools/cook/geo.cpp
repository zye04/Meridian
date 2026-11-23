#include "geo.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <queue>

namespace meridian::cook {

namespace {

constexpr int kDi[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kDj[8] = {0, 1, 1, 1, 0, -1, -1, -1};

bool inside(const Grid &g, int i, int j) {
  return i >= 0 && j >= 0 && i < static_cast<int>(g.width) && j < static_cast<int>(g.height);
}

float slopeTan(const Grid &h, uint32_t i, uint32_t j) {
  const uint32_t i0 = i > 0 ? i - 1 : i, i1 = std::min(i + 1, h.width - 1);
  const uint32_t j0 = j > 0 ? j - 1 : j, j1 = std::min(j + 1, h.height - 1);
  const auto at = [&](uint32_t a, uint32_t b) { return h.v[size_t{b} * h.width + a]; };
  const float dx = (at(i1, j) - at(i0, j)) / (static_cast<float>(i1 - i0) * h.spacing);
  const float dz = (at(i, j1) - at(i, j0)) / (static_cast<float>(j1 - j0) * h.spacing);
  return std::hypot(dx, dz);
}

} // namespace

Grid fillDepressions(const Grid &heights) {
  Grid out = heights;
  const int w = static_cast<int>(out.width), h = static_cast<int>(out.height);
  using Entry = std::pair<float, uint32_t>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
  std::vector<uint8_t> closed(out.v.size(), 0);
  for (int j = 0; j < h; ++j) {
    for (int i = 0; i < w; ++i) {
      if (i == 0 || j == 0 || i == w - 1 || j == h - 1) {
        const uint32_t k = static_cast<uint32_t>(j * w + i);
        closed[k] = 1;
        open.emplace(out.v[k], k);
      }
    }
  }
  while (!open.empty()) {
    const uint32_t c = open.top().second;
    open.pop();
    const int ci = static_cast<int>(c % out.width), cj = static_cast<int>(c / out.width);
    for (int d = 0; d < 8; ++d) {
      const int ni = ci + kDi[d], nj = cj + kDj[d];
      if (!inside(out, ni, nj)) {
        continue;
      }
      const uint32_t n = static_cast<uint32_t>(nj * w + ni);
      if (closed[n]) {
        continue;
      }
      closed[n] = 1;
      out.v[n] = std::max(out.v[n], std::nextafter(out.v[c], INFINITY));
      open.emplace(out.v[n], n);
    }
  }
  return out;
}

Grid smoothGrid(const Grid &grid, float radiusMeters) {
  const int r = static_cast<int>(std::lround(radiusMeters / grid.spacing));
  if (r <= 0) {
    return grid;
  }
  const uint32_t w = grid.width, h = grid.height;
  std::vector<double> sat(size_t{w + 1} * (h + 1), 0.0);
  for (uint32_t j = 0; j < h; ++j) {
    double row = 0.0;
    for (uint32_t i = 0; i < w; ++i) {
      row += grid.v[size_t{j} * w + i];
      sat[size_t{j + 1} * (w + 1) + i + 1] = sat[size_t{j} * (w + 1) + i + 1] + row;
    }
  }
  Grid out = grid;
  for (uint32_t j = 0; j < h; ++j) {
    const uint32_t j0 = static_cast<uint32_t>(std::max<int>(0, static_cast<int>(j) - r));
    const uint32_t j1 = std::min(j + r + 1, h);
    for (uint32_t i = 0; i < w; ++i) {
      const uint32_t i0 = static_cast<uint32_t>(std::max<int>(0, static_cast<int>(i) - r));
      const uint32_t i1 = std::min(i + r + 1, w);
      const double sum = sat[size_t{j1} * (w + 1) + i1] - sat[size_t{j0} * (w + 1) + i1] -
                         sat[size_t{j1} * (w + 1) + i0] + sat[size_t{j0} * (w + 1) + i0];
      out.v[size_t{j} * w + i] = static_cast<float>(sum / ((i1 - i0) * (j1 - j0)));
    }
  }
  return out;
}

Grid specificCatchmentArea(const Grid &filled) {
  constexpr float kExponent = 1.1f;
  // Contour length per direction in cells (Quinn et al. 1991).
  constexpr float kContour[8] = {0.5f, 0.354f, 0.5f, 0.354f, 0.5f, 0.354f, 0.5f, 0.354f};
  const float s = filled.spacing;
  std::vector<double> area(filled.v.size(), double{s} * s);
  std::vector<uint32_t> order(filled.v.size());
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(),
            [&](uint32_t a, uint32_t b) { return filled.v[a] > filled.v[b]; });
  const int w = static_cast<int>(filled.width);
  for (uint32_t c : order) {
    const int ci = static_cast<int>(c % filled.width), cj = static_cast<int>(c / filled.width);
    float weights[8] = {};
    float total = 0.0f;
    for (int d = 0; d < 8; ++d) {
      const int ni = ci + kDi[d], nj = cj + kDj[d];
      if (!inside(filled, ni, nj)) {
        continue;
      }
      const float drop = filled.v[c] - filled.v[size_t(nj * w + ni)];
      if (drop > 0.0f) {
        const float distance = (d % 2 ? 1.41421356f : 1.0f) * s;
        weights[d] = std::pow(drop / distance, kExponent) * kContour[d];
        total += weights[d];
      }
    }
    if (total <= 0.0f) {
      continue;
    }
    for (int d = 0; d < 8; ++d) {
      if (weights[d] > 0.0f) {
        area[size_t((cj + kDj[d]) * w + ci + kDi[d])] += area[c] * (weights[d] / total);
      }
    }
  }
  Grid out{filled.width, filled.height, s, std::vector<float>(filled.v.size())};
  for (size_t k = 0; k < area.size(); ++k) {
    out.v[k] = static_cast<float>(area[k] / s);
  }
  return out;
}

Grid wetnessIndex(const Grid &sca, const Grid &heights) {
  const float epsilon = std::tan(0.001f * 3.14159265f / 180.0f);
  Grid out = sca;
  for (uint32_t j = 0; j < sca.height; ++j) {
    for (uint32_t i = 0; i < sca.width; ++i) {
      const size_t k = size_t{j} * sca.width + i;
      out.v[k] = std::log(std::max(sca.v[k], 1e-6f) / (slopeTan(heights, i, j) + epsilon));
    }
  }
  return out;
}

} // namespace meridian::cook
