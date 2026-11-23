#include "geo.h"
#include "geo_channels.h"

#include <meridian/terrain.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
#include <thread>
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

namespace meridian::cook {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kAzimuths = 16;

// Normal of the surface from central differences: (-dh/dx, 1, -dh/dz).
void surfaceNormal(const Grid &h, uint32_t i, uint32_t j, float n[3]) {
  const uint32_t i0 = i > 0 ? i - 1 : i, i1 = std::min(i + 1, h.width - 1);
  const uint32_t j0 = j > 0 ? j - 1 : j, j1 = std::min(j + 1, h.height - 1);
  const auto at = [&](uint32_t a, uint32_t b) { return h.v[size_t{b} * h.width + a]; };
  const float dx = (at(i1, j) - at(i0, j)) / (static_cast<float>(i1 - i0) * h.spacing);
  const float dz = (at(i, j1) - at(i, j0)) / (static_cast<float>(j1 - j0) * h.spacing);
  const float len = std::sqrt(dx * dx + 1.0f + dz * dz);
  n[0] = -dx / len;
  n[1] = 1.0f / len;
  n[2] = -dz / len;
}

// Sun directions (x east, y up, z south) above the flat horizon, every hour
// of the 21st of each month.
std::vector<std::array<float, 3>> sunDirections(float latitudeDegrees) {
  std::vector<std::array<float, 3>> out;
  const float phi = latitudeDegrees * kPi / 180.0f;
  for (int month = 0; month < 12; ++month) {
    const float day = 21.0f + 30.44f * static_cast<float>(month);
    const float declination = -23.44f * kPi / 180.0f * std::cos(2.0f * kPi * (day + 10.0f) / 365.0f);
    for (int hour = 0; hour < 24; ++hour) {
      const float hourAngle = (static_cast<float>(hour) + 0.5f - 12.0f) * kPi / 12.0f;
      // East-north-up from the local hour angle (Duffie and Beckman).
      const float up = std::sin(phi) * std::sin(declination) +
                       std::cos(phi) * std::cos(declination) * std::cos(hourAngle);
      if (up <= 0.0f) {
        continue;
      }
      const float east = -std::cos(declination) * std::sin(hourAngle);
      const float north = std::cos(phi) * std::sin(declination) -
                          std::sin(phi) * std::cos(declination) * std::cos(hourAngle);
      out.push_back({east, up, -north});
    }
  }
  return out;
}

template <typename F> void parallelRows(uint32_t rows, F &&f) {
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  std::atomic<uint32_t> next{0};
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
      for (uint32_t j = next++; j < rows; j = next++) {
        f(j);
      }
    });
  }
  for (auto &thread : pool) {
    thread.join();
  }
}

} // namespace

Grid topographicPosition(const Grid &heights, float radiusMeters) {
  const Grid mean = smoothGrid(heights, radiusMeters);
  Grid out = heights;
  for (size_t k = 0; k < out.v.size(); ++k) {
    out.v[k] -= mean.v[k];
  }
  return out;
}

Grid sunExposure(const Grid &heights, float latitudeDegrees) {
  const auto suns = sunDirections(latitudeDegrees);
  float flat = 0.0f;
  for (const auto &s : suns) {
    flat += s[1];
  }
  Grid out{heights.width, heights.height, heights.spacing,
           std::vector<float>(heights.v.size(), 0.0f)};
  if (flat <= 0.0f) {
    return out;
  }
  // Each sun's horizon azimuth bin.
  std::vector<int> bins;
  for (const auto &s : suns) {
    const float azimuth = std::atan2(s[0], s[2]); // from south toward east
    bins.push_back(static_cast<int>(std::lround(azimuth / (2.0f * kPi) * kAzimuths) + kAzimuths) %
                   kAzimuths);
  }
  constexpr float kMaxDistance = 2000.0f;
  parallelRows(heights.height, [&](uint32_t j) {
    for (uint32_t i = 0; i < heights.width; ++i) {
      const float h0 = heights.v[size_t{j} * heights.width + i];
      float horizon[kAzimuths];
      for (int a = 0; a < kAzimuths; ++a) {
        const float angle = 2.0f * kPi * static_cast<float>(a) / kAzimuths;
        const float dx = std::sin(angle), dz = std::cos(angle);
        float best = 0.0f;
        for (float d = heights.spacing; d <= kMaxDistance; d = std::max(d * 1.08f, d + heights.spacing)) {
          const int si = static_cast<int>(std::lround(i + dx * d / heights.spacing));
          const int sj = static_cast<int>(std::lround(j + dz * d / heights.spacing));
          if (!inside(heights, si, sj)) {
            break;
          }
          best = std::max(best, (heights.v[size_t(sj) * heights.width + si] - h0) / d);
        }
        horizon[a] = best;
      }
      float n[3];
      surfaceNormal(heights, i, j, n);
      float sum = 0.0f;
      for (size_t k = 0; k < suns.size(); ++k) {
        const auto &s = suns[k];
        const float tanElevation = s[1] / std::max(std::hypot(s[0], s[2]), 1e-6f);
        if (tanElevation > horizon[bins[k]]) {
          sum += std::max(0.0f, n[0] * s[0] + n[1] * s[1] + n[2] * s[2]);
        }
      }
      out.v[size_t{j} * heights.width + i] = sum / flat;
    }
  });
  return out;
}

Grid talus(const Grid &heights) {
  const float sourceTan = std::tan(45.0f * kPi / 180.0f);
  const float reachTan = std::tan(32.0f * kPi / 180.0f);
  Grid out{heights.width, heights.height, heights.spacing,
           std::vector<float>(heights.v.size(), 0.0f)};
  const int w = static_cast<int>(heights.width);
  std::vector<uint32_t> path;
  std::vector<float> along;
  for (uint32_t j = 0; j < heights.height; ++j) {
    for (uint32_t i = 0; i < heights.width; ++i) {
      if (slopeTan(heights, i, j) <= sourceTan) {
        continue;
      }
      const float top = heights.v[size_t{j} * heights.width + i];
      path.clear();
      along.clear();
      int ci = static_cast<int>(i), cj = static_cast<int>(j);
      float distance = 0.0f;
      for (;;) {
        path.push_back(static_cast<uint32_t>(cj * w + ci));
        along.push_back(distance);
        int best = -1;
        float lowest = heights.v[size_t(cj * w + ci)];
        for (int d = 0; d < 8; ++d) {
          const int ni = ci + kDi[d], nj = cj + kDj[d];
          if (inside(heights, ni, nj) && heights.v[size_t(nj * w + ni)] < lowest) {
            lowest = heights.v[size_t(nj * w + ni)];
            best = d;
          }
        }
        if (best < 0) {
          break;
        }
        const float step = (best % 2 ? 1.41421356f : 1.0f) * heights.spacing;
        const float horizontal =
            std::hypot(static_cast<float>(ci + kDi[best] - static_cast<int>(i)),
                       static_cast<float>(cj + kDj[best] - static_cast<int>(j))) *
            heights.spacing;
        if (top - lowest < reachTan * horizontal) {
          break;
        }
        ci += kDi[best];
        cj += kDj[best];
        distance += step;
      }
      // The deposit starts at the foot: the first cell no longer steep.
      size_t foot = 0;
      while (foot < path.size() &&
             slopeTan(heights, path[foot] % heights.width, path[foot] / heights.width) > sourceTan) {
        ++foot;
      }
      if (foot == path.size()) {
        continue;
      }
      const float length = std::max(distance - along[foot], heights.spacing);
      for (size_t k = foot; k < path.size(); ++k) {
        out.v[path[k]] = std::max(out.v[path[k]], 1.0f - (along[k] - along[foot]) / length);
      }
    }
  }
  return out;
}

GeoChannels cookGeo(const Heightfield &field, uint32_t ratio, float latitudeDegrees) {
  if (ratio == 0 || field.width % ratio != 0 || field.height % ratio != 0) {
    throw std::runtime_error("geo: the ratio must divide the heightfield's width and height");
  }
  Grid full{field.width, field.height, field.spacing, std::vector<float>(field.samples.size())};
  for (size_t k = 0; k < full.v.size(); ++k) {
    full.v[k] = field.minHeight + field.heightScale * static_cast<float>(field.samples[k]);
  }
  const uint32_t w = field.width / ratio, h = field.height / ratio;
  const auto reduce = [&](const Grid &g) {
    Grid out{w, h, field.spacing * static_cast<float>(ratio), std::vector<float>(size_t{w} * h)};
    for (uint32_t j = 0; j < h; ++j) {
      for (uint32_t i = 0; i < w; ++i) {
        double sum = 0.0;
        for (uint32_t b = 0; b < ratio; ++b) {
          for (uint32_t a = 0; a < ratio; ++a) {
            sum += g.v[size_t{j * ratio + b} * g.width + i * ratio + a];
          }
        }
        out.v[size_t{j} * w + i] = static_cast<float>(sum / (ratio * ratio));
      }
    }
    return out;
  };
  const Grid coarse = reduce(full);
  // Flow on a lightly smoothed DEM (~4 m effective, Lidberg et al. 2021):
  // micro-relief would fragment the drainage.
  const Grid smoothed = smoothGrid(coarse, 2.0f);
  const Grid sca = specificCatchmentArea(fillDepressions(smoothed));
  const Grid wetness = wetnessIndex(sca, smoothed);
  const Grid local = reduce(topographicPosition(full, 6.0f));
  const Grid landform = topographicPosition(coarse, 40.0f);
  const Grid sun = sunExposure(coarse, latitudeDegrees);
  // Smoothed: single steepest-descent paths leave 1-texel streaks.
  const Grid rockfall = smoothGrid(talus(coarse), 4.0f);

  GeoChannels geo;
  geo.width = w;
  geo.height = h;
  geo.ratio = ratio;
  geo.fieldWidth = field.width;
  geo.fieldHeight = field.height;
  geo.fieldSpacing = field.spacing;
  geo.texels.assign(size_t{w} * h * kGeoChannelCount, 0);
  for (size_t k = 0; k < size_t{w} * h; ++k) {
    uint8_t *t = &geo.texels[k * kGeoChannelCount];
    t[geo::kWetness] = geo::encodeWetness(wetness.v[k]);
    t[geo::kFlow] = geo::encodeFlow(sca.v[k]);
    t[geo::kLocalPosition] = geo::encodeLocalPosition(local.v[k]);
    t[geo::kLandformPosition] = geo::encodeLandformPosition(landform.v[k]);
    t[geo::kSunExposure] = geo::encodeSunExposure(sun.v[k]);
    t[geo::kTalus] = geo::encodeTalus(rockfall.v[k]);
  }
  return geo;
}

} // namespace meridian::cook
