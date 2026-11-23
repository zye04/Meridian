#pragma once

// Geomorphometry for the geo channels (meridian-cook --geo).

#include <cstdint>
#include <vector>

#include <meridian/geo.h>

namespace meridian {
struct Heightfield;
}

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
// Topographic position index: height minus the smoothGrid mean (m).
Grid topographicPosition(const Grid &heights, float radiusMeters);
// Annual mean direct sun on the surface (cos incidence, terrain horizon in
// 16 azimuths up to 2 km, every hour of the 21st of each month, no
// atmosphere), relative to a flat open surface at the same latitude; 0
// where the sun never rises.
Grid sunExposure(const Grid &heights, float latitudeDegrees);
// Rockfall reach: from every cell steeper than 45 degrees, down the
// steepest descent while the line from that source stays at least 32
// degrees below horizontal (Heim's Fahrboschung); per cell the largest
// closeness 1 - d / D over the paths through it (d along the path from
// the source's foot, the first cell no longer steep; D the deposit's
// length); 0 on the steep ground itself and out of reach.
Grid talus(const Grid &heights);
// All channels (geo_channels.h) of a heightfield at ratio x ratio samples
// per texel; throws unless ratio divides both sides.
GeoChannels cookGeo(const Heightfield &field, uint32_t ratio, float latitudeDegrees);

} // namespace meridian::cook
