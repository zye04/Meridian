#include <doctest/doctest.h>
#include <meridian/terrain.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>

namespace {

meridian::Heightfield rampField(uint32_t w, uint32_t h, float spacing) {
  std::vector<float> heights(static_cast<size_t>(w) * h);
  for (size_t k = 0; k < heights.size(); ++k) {
    heights[k] = 100.0f + static_cast<float>((k * 37) % 91);
  }
  return meridian::quantizeHeightfield(heights, w, h, spacing);
}

} // namespace

TEST_CASE("heightfield file round-trips") {
  const meridian::Heightfield a = rampField(5, 3, 2.0f);
  const auto path = std::filesystem::temp_directory_path() / "meridian_test.height";
  meridian::saveHeightfield(a, path);
  const meridian::Heightfield b = meridian::loadHeightfield(path);
  std::filesystem::remove(path);
  CHECK(b.width == a.width);
  CHECK(b.height == a.height);
  CHECK(b.spacing == a.spacing);
  CHECK(b.minHeight == a.minHeight);
  CHECK(b.heightScale == a.heightScale);
  CHECK(b.samples == a.samples);
}

TEST_CASE("quantizeHeightfield keeps heights within half a step") {
  const std::vector<float> heights = {600.0f, 1406.69f, 812.3f, 950.0f};
  const meridian::Heightfield field = meridian::quantizeHeightfield(heights, 2, 2, 2.0f);
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      CHECK(std::abs(field.sample(i, j) - heights[j * 2 + i]) <=
            field.heightScale / 2 + 1e-4f);
    }
  }
}

TEST_CASE("heightAt is exact at samples and bilinear between") {
  const meridian::Heightfield field = rampField(3, 3, 2.0f);
  for (int j = 0; j < 3; ++j) {
    for (int i = 0; i < 3; ++i) {
      CHECK(field.heightAt((i - 1) * 2.0f, (j - 1) * 2.0f) ==
            doctest::Approx(field.sample(i, j)).epsilon(1e-5));
    }
  }
  CHECK(field.heightAt(-1.0f, -2.0f) ==
        doctest::Approx((field.sample(0, 0) + field.sample(1, 0)) / 2).epsilon(1e-5));
  CHECK(field.heightAt(2.0f, 1.0f) ==
        doctest::Approx((field.sample(2, 1) + field.sample(2, 2)) / 2).epsilon(1e-5));
}

TEST_CASE("indices mirror outside the grid like MIRRORED_REPEAT") {
  const meridian::Heightfield field = rampField(4, 4, 2.0f);
  CHECK(field.sample(-1, 0) == field.sample(0, 0));
  CHECK(field.sample(4, 0) == field.sample(3, 0));
  CHECK(field.sample(5, 0) == field.sample(2, 0));
  CHECK(field.sample(8, 0) == field.sample(0, 0));
  CHECK(field.sample(0, -1) == field.sample(0, 0));
  CHECK(field.sample(0, 5) == field.sample(0, 2));
  const float eastEdge = 3.0f; // sample 3 sits at x = 3
  for (float d : {1.0f, 7.0f}) {
    // The sample-space mirror about the edge is x -> 2*(edge + spacing/2) - x.
    const float mirrored = 2.0f * (eastEdge + 1.0f) - (eastEdge + d);
    CHECK(field.heightAt(eastEdge + d, 0.0f) ==
          doctest::Approx(field.heightAt(mirrored, 0.0f)).epsilon(1e-5));
  }
}

TEST_CASE("height pyramid bounds every sample it covers") {
  std::mt19937 rng(7);
  std::vector<float> heights(7 * 5);
  for (float &h : heights) {
    h = static_cast<float>(rng() % 1000);
  }
  const meridian::Heightfield field = meridian::quantizeHeightfield(heights, 7, 5, 2.0f);
  const auto levels = meridian::buildHeightPyramid(field);
  REQUIRE(levels.size() == 4);
  CHECK(levels.back().width == 1);
  CHECK(levels.back().height == 1);
  for (size_t k = 0; k < levels.size(); ++k) {
    const uint32_t cover = 1u << k;
    for (uint32_t y = 0; y < levels[k].height; ++y) {
      for (uint32_t x = 0; x < levels[k].width; ++x) {
        uint32_t lo = 0xFFFF;
        uint32_t hi = 0;
        for (uint32_t sy = y * cover; sy < std::min((y + 1) * cover, field.height); ++sy) {
          for (uint32_t sx = x * cover; sx < std::min((x + 1) * cover, field.width); ++sx) {
            lo = std::min<uint32_t>(lo, field.samples[sy * field.width + sx]);
            hi = std::max<uint32_t>(hi, field.samples[sy * field.width + sx]);
          }
        }
        CHECK(levels[k].minMax[y * levels[k].width + x] == (lo | hi << 16));
      }
    }
  }
}

TEST_CASE("procedural fallback is a valid heightfield") {
  meridian::HeightfieldSettings settings;
  settings.resolution = 64;
  const meridian::Heightfield field = meridian::generateHeightfield(settings);
  CHECK(field.width == 64);
  CHECK(field.height == 64);
  CHECK(field.samples.size() == 64 * 64);
  for (int j = 0; j < 64; ++j) {
    for (int i = 0; i < 64; ++i) {
      CHECK(field.sample(i, j) >= settings.baseHeight - 1e-3f);
      CHECK(field.sample(i, j) <= settings.baseHeight + settings.maxHeight + 1e-3f);
    }
  }
}

TEST_CASE("loadHeightfield rejects files whose size disagrees with the header") {
  const auto path = std::filesystem::temp_directory_path() / "meridian_bad.height";
  meridian::saveHeightfield(rampField(5, 3, 2.0f), path);
  std::vector<char> bytes(std::filesystem::file_size(path));
  std::ifstream(path, std::ios::binary).read(bytes.data(), bytes.size());

  auto write = [&](size_t size, bool hugeWidth) {
    std::vector<char> copy(bytes);
    copy.resize(size);
    if (hugeWidth) {
      const uint32_t width = 0x40000000;
      std::memcpy(copy.data() + 8, &width, sizeof(width));
    }
    std::ofstream(path, std::ios::binary).write(copy.data(), copy.size());
  };
  write(bytes.size() - 2, false);
  CHECK_THROWS_AS(meridian::loadHeightfield(path), std::runtime_error);
  write(bytes.size() + 2, false);
  CHECK_THROWS_AS(meridian::loadHeightfield(path), std::runtime_error);
  write(bytes.size(), true);
  CHECK_THROWS_AS(meridian::loadHeightfield(path), std::runtime_error);
  std::filesystem::remove(path);
}
