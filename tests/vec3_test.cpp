#include <doctest/doctest.h>
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
