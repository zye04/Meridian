#pragma once

// Geomorphometry for the geo channels (meridian-cook --geo).

#include <cstdint>
#include <vector>

namespace meridian::cook {

// Row-major, row 0 = north; x = column * spacing, z = row * spacing.
struct Grid {
  uint32_t width = 0, height = 0;
  float spacing = 0.0f;
  std::vector<float> v;
};

// Priority-Flood+epsilon (Barnes, Lehman and Mulla 2014): raises every
// depression so each cell drains to the grid edge along a strictly
// descending path.
Grid fillDepressions(const Grid &heights);
// Box mean over (2r + 1)^2 cells, r = round(radius / spacing), clamped at
// the edges.
Grid smoothGrid(const Grid &grid, float radiusMeters);
// Multiple flow direction (FD8, Freeman 1991, exponent 1.1) over a filled
// grid; m^2 per m of contour.
Grid specificCatchmentArea(const Grid &filled);
// ln(sca / (tan b + tan 0.001 deg)), Beven and Kirkby 1979; b from the
// heights' central differences.
Grid wetnessIndex(const Grid &sca, const Grid &heights);

} // namespace meridian::cook
