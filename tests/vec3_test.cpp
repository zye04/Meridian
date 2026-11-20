#include <doctest/doctest.h>
#include <meridian/camera.h>
#include <meridian/math/vec3.h>

TEST_CASE("dot product") {
  CHECK(meridian::dot({1, 2, 3}, {4, 5, 6}) == 32.0f);
}

TEST_CASE("length") { CHECK(meridian::length({3, 4, 0}) == 5.0f); }

TEST_CASE("cross") {
  meridian::Vec3 result = meridian::cross({1, 0, 0}, {0, 1, 0});
  CHECK(result.x == 0.0f);
  CHECK(result.y == 0.0f);
  CHECK(result.z == 1.0f);
}

TEST_CASE("scripted camera path ends exactly at the target") {
  const meridian::Vec3 end{7000.0f, 1288.2f, 7000.0f};
  const meridian::Vec3 v{5.0f, 0.0f, -3.0f};
  const int frames = 240;
  const meridian::Vec3 last =
      meridian::scriptedPosition(end, v, 1.0f / 60.0f, frames - frames);
  CHECK(last.x == end.x);
  CHECK(last.y == end.y);
  CHECK(last.z == end.z);
  const meridian::Vec3 first =
      meridian::scriptedPosition(end, v, 1.0f / 60.0f, frames);
  CHECK(first.x == doctest::Approx(end.x - 20.0f).epsilon(1e-6));
}
