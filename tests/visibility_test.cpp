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

namespace {

// A camera at the origin looking down -z: clip = (f x, f y, 0.1, -z), render
// 100 x 80, f = 1.5 (the test needs only x, y and w).
void project(const float v[3], float out[4]) {
  out[0] = 1.5f * v[0];
  out[1] = 1.5f * v[1];
  out[2] = 0.1f;
  out[3] = -v[2];
}

// The weights at the pixel the 3D point sum beta_i v_i lands on.
Barycentrics atPoint(const float v[3][3], const float beta[3]) {
  float c[3][4];
  float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (int i = 0; i < 3; ++i) {
    project(v[i], c[i]);
    for (int k = 0; k < 4; ++k) {
      p[k] += beta[i] * c[i][k];
    }
  }
  const float px = (p[0] / p[3] * 0.5f + 0.5f) * 100.0f;
  const float py = (p[1] / p[3] * 0.5f + 0.5f) * 80.0f;
  return barycentricsAt(c[0], c[1], c[2], px, py, 100.0f, 80.0f);
}

} // namespace

TEST_CASE("visibility: interiorIsPerspectiveCorrect") {
  // A triangle receding in depth: screen-linear weights would be wrong here.
  const float v[3][3] = {{-1.0f, -0.6f, -1.5f}, {1.2f, -0.4f, -6.0f}, {0.1f, 0.9f, -3.0f}};
  const float betas[3][3] = {{0.2f, 0.3f, 0.5f}, {0.6f, 0.3f, 0.1f}, {0.1f, 0.8f, 0.1f}};
  for (const auto &beta : betas) {
    const Barycentrics r = atPoint(v, beta);
    for (int j = 0; j < 3; ++j) {
      // The vertices snap to 1/256 px: a few 1e-4 on a triangle tens of px wide.
      CHECK(component(r.b, j) == doctest::Approx(beta[j]).epsilon(2e-3).scale(1.0f));
    }
    CHECK(r.outside == 0.0f);
  }
}

TEST_CASE("visibility: vertexBehindOrAtCameraGivesVisiblePartsWeights") {
  // Vertex 0 behind the camera (w = -0.5) or in its plane (w = 0, where a
  // division by w gives NaN): the rasterizer clips the triangle at the near
  // plane and draws the part in front.
  for (const float z0 : {0.5f, 0.0f}) {
    const float v[3][3] = {{-0.3f, -0.2f, z0}, {1.0f, -0.5f, -4.0f}, {-0.5f, 0.8f, -3.0f}};
    const float beta[3] = {0.1f, 0.45f, 0.45f};
    const Barycentrics r = atPoint(v, beta);
    for (int j = 0; j < 3; ++j) {
      CHECK(component(r.b, j) == doctest::Approx(beta[j]).epsilon(1e-4).scale(1.0f));
    }
    CHECK(r.outside == 0.0f);
    // Derivatives still match a finite difference.
    float c[3][4];
    for (int i = 0; i < 3; ++i) {
      project(v[i], c[i]);
    }
    const float px = 50.0f, py = 40.0f, h = 1.0f / 64.0f;
    const Barycentrics base = barycentricsAt(c[0], c[1], c[2], px, py, 100.0f, 80.0f);
    const Barycentrics xp = barycentricsAt(c[0], c[1], c[2], px + h, py, 100.0f, 80.0f);
    const Barycentrics xm = barycentricsAt(c[0], c[1], c[2], px - h, py, 100.0f, 80.0f);
    for (int j = 0; j < 3; ++j) {
      const float fx = (component(xp.b, j) - component(xm.b, j)) / (2.0f * h);
      CHECK(component(base.dbdx, j) == doctest::Approx(fx).epsilon(1e-2).scale(1e-3));
    }
  }
}
