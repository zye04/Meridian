#include <doctest/doctest.h>

#include <cmath>
#include <sstream>

#include "meridian/camera_route.h"

using namespace meridian;

TEST_CASE("routePose interpolates position and pitch linearly") {
  const std::vector<CameraKey> keys = {{0, {0, 0, 0}, 0, 0, ""}, {2, {10, 0, -4}, 0, 0.4f, ""}};
  const CameraPose p = routePose(keys, 1.0f);
  CHECK(p.position.x == doctest::Approx(5));
  CHECK(p.position.z == doctest::Approx(-2));
  CHECK(p.pitch == doctest::Approx(0.2f));
}

TEST_CASE("routePose clamps outside the route") {
  const std::vector<CameraKey> keys = {{0, {0, 0, 0}, 0, 0, ""}, {2, {10, 0, -4}, 1, 0.4f, ""}};
  CHECK(routePose(keys, -1).position.x == 0);
  CHECK(routePose(keys, 5).position.x == 10);
  CHECK(routePose(keys, 5).yaw == 1);
}

TEST_CASE("routePose turns yaw the short way across the wrap") {
  const std::vector<CameraKey> keys = {{0, {0, 0, 0}, 3.0f, 0, ""}, {1, {0, 0, 0}, -3.0f, 0, ""}};
  const float yaw = routePose(keys, 0.5f).yaw;
  CHECK(std::abs(std::remainder(yaw - 3.14159265f, 6.2831853f)) < 1e-4f);
}

TEST_CASE("parseRoute skips comments and keeps labels") {
  std::istringstream in("# r\n0 1 2 3 0.5 -0.1 meadow\n\n4 1 2 9 0.5 -0.1\n");
  const auto keys = parseRoute(in);
  REQUIRE(keys.size() == 2);
  CHECK(keys[0].label == "meadow");
  CHECK(keys[1].label.empty());
  CHECK(keys[1].position.z == 9);
}

TEST_CASE("parseRoute rejects short lines and non-increasing times") {
  std::istringstream shortLine("0 1 2 3 0.5\n1 1 2 3 0.5 0\n");
  CHECK_THROWS_WITH_AS(parseRoute(shortLine),
                       "route line 1: expected TIME X Y Z YAW PITCH [LABEL]",
                       std::runtime_error);
  std::istringstream flat("0 1 2 3 0.5 0\n0 1 2 3 0.5 0\n");
  CHECK_THROWS_WITH_AS(parseRoute(flat), "route line 2: time must increase", std::runtime_error);
}
