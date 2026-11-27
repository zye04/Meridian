#include <doctest/doctest.h>

#include <meridian/cluster.h>
#include <meridian/visibility.h>

#include <cmath>
#include <random>

using namespace meridian;

namespace {

constexpr uint32_t kMaxInstanceIdTest = (1u << 24) - 1;
constexpr uint32_t kMaxClusterIndexTest = (1u << 26) - 1;
constexpr uint32_t kMaxVoxelQuadsTest = 55870000;

float component(Vec3 v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }

// Clip position of framebuffer point (px, py) at depth w (render size 100 x 80).
void clipAt(float px, float py, float w, float out[4]) {
  out[0] = (px / 100.0f * 2.0f - 1.0f) * w;
  out[1] = (py / 80.0f * 2.0f - 1.0f) * w;
  out[2] = 0.1f;
  out[3] = w;
}

} // namespace

TEST_CASE("visibility: packRoundTrip") {
  for (uint32_t instance : {0u, kMaxInstanceIdTest}) {
    for (uint32_t cluster : {0u, kMaxClusterIndexTest}) {
      for (uint32_t triangle : {0u, 127u}) {
        for (uint32_t sun : {0u, 1u, 255u}) {
          const UnpackedVisibility u =
              unpackVisibility(packClusterTriangle(instance, cluster, triangle, sun));
          CHECK(u.kind == VisibilityKind::ClusterTriangle);
          CHECK(u.instance == instance);
          CHECK(u.cluster == cluster);
          CHECK(u.triangle == triangle);
          CHECK(u.terrainSunCode == sun);
        }
      }
    }
  }
  for (uint32_t phase : {0u, 1u}) {
    const UnpackedVisibility u = unpackVisibility(packVoxel(phase, kMaxVoxelQuadsTest - 1));
    CHECK(u.kind == VisibilityKind::Voxel);
    CHECK(u.phase == phase);
    CHECK(u.index == kMaxVoxelQuadsTest - 1);
  }
}

TEST_CASE("visibility: clearedIsNone") {
  CHECK(visibilityKind({0, 0}) == VisibilityKind::None);
  CHECK(visibilityKind(packClusterTriangle(0, 0, 0, 0)) == VisibilityKind::ClusterTriangle);
}

TEST_CASE("visibility: barycentricsAtVertices") {
  const float p[3][2] = {{10.0f, 10.0f}, {90.0f, 20.0f}, {40.0f, 70.0f}};
  const float w[3] = {1.0f, 2.0f, 4.0f};
  float c[3][4];
  for (int i = 0; i < 3; ++i) {
    clipAt(p[i][0], p[i][1], w[i], c[i]);
  }
  for (int i = 0; i < 3; ++i) {
    const Barycentrics r = barycentricsAt(c[0], c[1], c[2], p[i][0], p[i][1], 100.0f, 80.0f);
    for (int j = 0; j < 3; ++j) {
      CHECK(component(r.b, j) == doctest::Approx(i == j ? 1.0f : 0.0f).epsilon(1e-5));
    }
  }
}

TEST_CASE("visibility: barycentricDerivativesMatchFiniteDifference") {
  std::mt19937 random(7);
  std::uniform_real_distribution<float> position(0.0f, 100.0f);
  std::uniform_real_distribution<float> depth(1.0f, 4.0f);
  int checked = 0;
  for (int t = 0; t < 1000; ++t) {
    float c[3][4];
    for (auto &v : c) {
      clipAt(position(random), position(random) * 0.8f, depth(random), v);
    }
    const Barycentrics base = barycentricsAt(c[0], c[1], c[2], 50.0f, 40.0f, 100.0f, 80.0f);
    if (base.outside > 0.0f) {
      continue;
    }
    const float h = 1.0f / 64.0f;
    const Barycentrics xp = barycentricsAt(c[0], c[1], c[2], 50.0f + h, 40.0f, 100.0f, 80.0f);
    const Barycentrics xm = barycentricsAt(c[0], c[1], c[2], 50.0f - h, 40.0f, 100.0f, 80.0f);
    const Barycentrics yp = barycentricsAt(c[0], c[1], c[2], 50.0f, 40.0f + h, 100.0f, 80.0f);
    const Barycentrics ym = barycentricsAt(c[0], c[1], c[2], 50.0f, 40.0f - h, 100.0f, 80.0f);
    for (int j = 0; j < 3; ++j) {
      const float fx = (component(xp.b, j) - component(xm.b, j)) / (2.0f * h);
      const float fy = (component(yp.b, j) - component(ym.b, j)) / (2.0f * h);
      CHECK(component(base.dbdx, j) == doctest::Approx(fx).epsilon(1e-2).scale(1e-3));
      CHECK(component(base.dbdy, j) == doctest::Approx(fy).epsilon(1e-2).scale(1e-3));
    }
    ++checked;
  }
  CHECK(checked > 50);
}

TEST_CASE("visibility: facingMatchesWindingUnderMirror") {
  // Counter-clockwise as seen on screen (y down): top-left, bottom-left,
  // top-right. Vulkan's a = -1/2 sum(x_i y_(i+1) - x_(i+1) y_i) = +0.5 * scale.
  float c[3][4];
  clipAt(10.0f, 10.0f, 1.0f, c[0]);
  clipAt(10.0f, 60.0f, 1.0f, c[1]);
  clipAt(60.0f, 10.0f, 1.0f, c[2]);
  CHECK(barycentricsAt(c[0], c[1], c[2], 20.0f, 20.0f, 100.0f, 80.0f).frontFacing);
  // Mirrored in x: clockwise on screen.
  float m[3][4];
  clipAt(90.0f, 10.0f, 1.0f, m[0]);
  clipAt(90.0f, 60.0f, 1.0f, m[1]);
  clipAt(40.0f, 10.0f, 1.0f, m[2]);
  CHECK_FALSE(barycentricsAt(m[0], m[1], m[2], 80.0f, 20.0f, 100.0f, 80.0f).frontFacing);
}

TEST_CASE("visibility: outsideMeasuresDistance") {
  float c[3][4];
  clipAt(10.0f, 10.0f, 1.0f, c[0]);
  clipAt(10.0f, 60.0f, 1.0f, c[1]);
  clipAt(60.0f, 10.0f, 1.0f, c[2]);
  const float cx = (10.0f + 10.0f + 60.0f) / 3.0f;
  const float cy = (10.0f + 60.0f + 10.0f) / 3.0f;
  CHECK(barycentricsAt(c[0], c[1], c[2], cx, cy, 100.0f, 80.0f).outside == 0.0f);
  // One pixel left of the vertical edge x = 10.
  CHECK(barycentricsAt(c[0], c[1], c[2], 9.0f, 30.0f, 100.0f, 80.0f).outside > 0.0f);
}
