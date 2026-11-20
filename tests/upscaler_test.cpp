#include <doctest/doctest.h>
#include <meridian/upscaler.h>

using meridian::Extent;

TEST_CASE("scaled extent rounds each side") {
  const Extent display{2560, 1440};
  CHECK(meridian::scaledExtent(display, 1.0f) == Extent{2560, 1440});
  CHECK(meridian::scaledExtent(display, 0.5f) == Extent{1280, 720});
  CHECK(meridian::scaledExtent(display, 2.0f / 3) == Extent{1707, 960});
}

TEST_CASE("jitter phase count grows with the upscale ratio") {
  const Extent e{2560, 1440};
  CHECK(meridian::jitterPhaseCount(e, e, false) == 8);
  CHECK(meridian::jitterPhaseCount(e, e, true) == 32);
  CHECK(meridian::jitterPhaseCount({1280, 720}, {2560, 1440}, false) == 32);
  CHECK(meridian::jitterPhaseCount({1707, 960}, {2560, 1440}, false) == 18);
}

TEST_CASE("texture mip bias") {
  const Extent e{2560, 1440};
  CHECK(meridian::textureMipBias(e, e) == 0.0f);
  CHECK(meridian::textureMipBias({1280, 720}, {2560, 1440}) == doctest::Approx(-2.0f));
}
