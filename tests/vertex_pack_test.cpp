#include <doctest/doctest.h>
#include <meridian/vertex_pack.h>

#include <cmath>
#include <random>
#include <vector>

using namespace meridian;

namespace {

float angleDegrees(Vec3 a, Vec3 b) {
  // atan2 in double: acos of a float dot bottoms out at ~0.02 degrees.
  const double ax = a.x, ay = a.y, az = a.z, bx = b.x, by = b.y, bz = b.z;
  const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
  return float(std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), ax * bx + ay * by + az * bz) *
               180.0 / M_PI);
}

Vec3 randomUnit(std::mt19937 &rng) {
  std::normal_distribution<float> g;
  return normalize(Vec3{g(rng), g(rng), g(rng)});
}

} // namespace

TEST_CASE("packed vertices round trip within half a quantization step") {
  // A 30 m tree: positions over a tall box, tiling bark UVs up to 12.
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  std::vector<Vertex> vertices(20000);
  for (Vertex &v : vertices) {
    v.position = {unit(rng) * 9.0f - 4.5f, unit(rng) * 30.0f, unit(rng) * 8.0f - 3.0f};
    v.normal = randomUnit(rng);
    const Vec3 t = randomUnit(rng);
    v.tangent[0] = t.x, v.tangent[1] = t.y, v.tangent[2] = t.z;
    v.tangent[3] = unit(rng) < 0.5f ? -1.0f : 1.0f;
    v.uvX = unit(rng) * 12.0f - 2.0f;
    v.uvY = unit(rng) * 3.0f;
    v.alpha = unit(rng);
  }
  vertices[0].alpha = 0.0f;
  vertices[1].alpha = 1.0f;
  const VertexQuantization q = vertexQuantization(vertices);
  const Vec3 step = q.halfExtent * (1.0f / 32767.0f);
  CHECK(step.y < 0.00046f); // ~0.46 mm over 30 m

  float maxNormal = 0.0f, maxTangent = 0.0f;
  for (const Vertex &v : vertices) {
    const Vertex r = unpackVertex(packVertex(v, q), q);
    CHECK(std::abs(r.position.x - v.position.x) <= step.x * 0.5f + 1e-5f);
    CHECK(std::abs(r.position.y - v.position.y) <= step.y * 0.5f + 1e-5f);
    CHECK(std::abs(r.position.z - v.position.z) <= step.z * 0.5f + 1e-5f);
    CHECK(std::abs(r.uvX - v.uvX) <= q.uvScale[0] / 65535.0f * 0.5f + 1e-6f);
    CHECK(std::abs(r.uvY - v.uvY) <= q.uvScale[1] / 65535.0f * 0.5f + 1e-6f);
    CHECK(std::abs(r.alpha - v.alpha) <= 0.5f / 65535.0f + 1e-7f);
    CHECK(r.tangent[3] == v.tangent[3]);
    maxNormal = std::max(maxNormal, angleDegrees(r.normal, v.normal));
    maxTangent = std::max(maxTangent, angleDegrees({r.tangent[0], r.tangent[1], r.tangent[2]},
                                                   {v.tangent[0], v.tangent[1], v.tangent[2]}));
  }
  MESSAGE("max normal error " << maxNormal << " deg, tangent " << maxTangent << " deg");
  CHECK(maxNormal < 0.01f);
  CHECK(maxTangent < 0.02f);
}

TEST_CASE("packed vertices keep the trunk-fade alpha and the axis extremes") {
  std::vector<Vertex> vertices(3);
  vertices[0].position = {-1.0f, 0.0f, 2.0f};
  vertices[1].position = {3.0f, 0.95f, 2.0f}; // a flat axis (z) has no extent
  vertices[2].position = {1.0f, 0.6f, 2.0f};
  vertices[0].alpha = 1.0f;
  vertices[1].alpha = 0.0f;
  vertices[2].alpha = 0.208f;
  const VertexQuantization q = vertexQuantization(vertices);
  for (const Vertex &v : vertices) {
    const Vertex r = unpackVertex(packVertex(v, q), q);
    CHECK(r.position.z == 2.0f);
    CHECK(std::abs(r.alpha - v.alpha) <= 0.5f / 65535.0f + 1e-7f);
  }
  CHECK(unpackVertex(packVertex(vertices[0], q), q).alpha == 1.0f);
  CHECK(unpackVertex(packVertex(vertices[1], q), q).alpha == 0.0f);
  CHECK(unpackVertex(packVertex(vertices[0], q), q).position.x == -1.0f);
  CHECK(unpackVertex(packVertex(vertices[1], q), q).position.x == 3.0f);
}
