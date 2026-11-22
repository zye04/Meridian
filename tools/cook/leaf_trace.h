#pragma once

// Alpha-tested leaf cards into opaque leaf geometry: an atlas's alpha is
// traced once into a filled, simplified mesh in texture space, which is then
// clipped onto every card through the card's UVs.

#include <array>
#include <cstdint>
#include <vector>

namespace meridian::cook {

// Atlas-space triangles: positions are texture coordinates (glTF: u right,
// v down, 0..1 over the image).
struct AtlasMesh {
  std::vector<std::array<float, 2>> uv;
  std::vector<uint32_t> indices;
};

// alpha: width * height bytes, row 0 = v 0. Fills where alpha >= cutoff * 255
// (marching squares, edges interpolated between texel centres), simplifies to
// `toleranceTexels`, drops connected parts under `minAreaTexels`.
AtlasMesh traceAtlas(const std::vector<uint8_t> &alpha, uint32_t width, uint32_t height,
                     float cutoff, float toleranceTexels = 1.0f, float minAreaTexels = 16.0f);

// In UV units squared.
double atlasArea(const AtlasMesh &mesh);

struct CardVertex {
  std::array<float, 3> position;
  std::array<float, 3> normal;
  float uv[2];
};

// Every card triangle (indices into `vertices`) keeps the atlas triangles
// under its UV triangle (in every integer UV tile it touches), clipped to it
// and mapped by barycentrics to its position and normal. Output vertices on a
// card edge are computed from that edge's endpoints in a fixed order, so cards
// sharing an edge produce bit-identical positions there.
void clipToCards(const AtlasMesh &atlas, const std::vector<CardVertex> &vertices,
                 const std::vector<uint32_t> &indices, std::vector<CardVertex> &outVertices,
                 std::vector<uint32_t> &outIndices);

} // namespace meridian::cook
