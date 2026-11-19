#include <doctest/doctest.h>
#include <meridian/math/mat4.h>

using meridian::Mat4;
using meridian::Vec3;

namespace {

// Clip-space position (x, y, z, w) of a point.
struct Clip {
  float x, y, z, w;
};

Clip project(const Mat4 &m, Vec3 p) {
  return {m.m[0][0] * p.x + m.m[1][0] * p.y + m.m[2][0] * p.z + m.m[3][0],
          m.m[0][1] * p.x + m.m[1][1] * p.y + m.m[2][1] * p.z + m.m[3][1],
          m.m[0][2] * p.x + m.m[1][2] * p.y + m.m[2][2] * p.z + m.m[3][2],
          m.m[0][3] * p.x + m.m[1][3] * p.y + m.m[2][3] * p.z + m.m[3][3]};
}

} // namespace

TEST_CASE("identity leaves points unchanged") {
  Vec3 p = meridian::transformPoint(Mat4::identity(), {1, 2, 3});
  CHECK(p.x == 1.0f);
  CHECK(p.y == 2.0f);
  CHECK(p.z == 3.0f);
}

TEST_CASE("multiplication applies the right matrix first") {
  Mat4 moveX = meridian::translation({1, 0, 0});
  Mat4 moveY = meridian::translation({0, 2, 0});
  Vec3 p = meridian::transformPoint(moveX * moveY, {0, 0, 0});
  CHECK(p.x == 1.0f);
  CHECK(p.y == 2.0f);
  CHECK(p.z == 0.0f);
}

TEST_CASE("lookAt puts the target straight ahead on -Z") {
  Mat4 view = meridian::lookAt({0, 5, 10}, {0, 5, 0}, {0, 1, 0});
  Vec3 target = meridian::transformPoint(view, {0, 5, 0});
  CHECK(target.x == doctest::Approx(0.0f));
  CHECK(target.y == doctest::Approx(0.0f));
  CHECK(target.z == doctest::Approx(-10.0f));

  Vec3 right = meridian::transformPoint(view, {1, 5, 10});
  CHECK(right.x == doctest::Approx(1.0f));
}

TEST_CASE("perspective uses reverse-Z with an infinite far plane") {
  Mat4 proj = meridian::perspective(1.0f, 16.0f / 9.0f, 0.1f);

  Clip nearPoint = project(proj, {0, 0, -0.1f});
  CHECK(nearPoint.z / nearPoint.w == doctest::Approx(1.0f));

  Clip farPoint = project(proj, {0, 0, -100000.0f});
  CHECK(farPoint.z / farPoint.w == doctest::Approx(0.0f).epsilon(1e-5));
  CHECK(farPoint.z / farPoint.w > 0.0f);
}

TEST_CASE("perspective flips Y for Vulkan") {
  Mat4 proj = meridian::perspective(1.0f, 1.0f, 0.1f);
  Clip above = project(proj, {0, 1, -5});
  CHECK(above.y / above.w < 0.0f);
}

namespace {

// The same sphere test cull.comp runs.
bool sphereVisible(const float planes[6][4], Vec3 center, float radius) {
  for (int i = 0; i < 6; ++i) {
    if (planes[i][0] * center.x + planes[i][1] * center.y +
            planes[i][2] * center.z + planes[i][3] <
        -radius) {
      return false;
    }
  }
  return true;
}

} // namespace

TEST_CASE("frustum planes keep what the camera sees and reject the rest") {
  // Camera at the origin looking down -Z, 90 degree vertical FOV.
  Mat4 viewProjection = meridian::perspective(1.5707964f, 1.0f, 0.1f) *
                        meridian::lookAt({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
  float planes[6][4];
  meridian::extractFrustumPlanes(viewProjection, planes);

  CHECK(sphereVisible(planes, {0, 0, -10}, 1.0f));     // straight ahead
  CHECK(sphereVisible(planes, {0, 0, -1e6f}, 1.0f));   // infinite far plane
  CHECK_FALSE(sphereVisible(planes, {0, 0, 10}, 1.0f));   // behind
  CHECK_FALSE(sphereVisible(planes, {-30, 0, -10}, 1.0f)); // far left
  CHECK_FALSE(sphereVisible(planes, {0, 30, -10}, 1.0f));  // far above
  CHECK_FALSE(sphereVisible(planes, {0, 0, -0.01f}, 0.05f)); // before near
  // Partially inside counts as visible.
  CHECK(sphereVisible(planes, {-10.5f, 0, -10}, 1.0f));
}
