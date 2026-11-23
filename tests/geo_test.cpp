#include <doctest/doctest.h>
#include <meridian/geo.h>
#include <meridian/terrain.h>

#include <filesystem>
#include <fstream>

namespace {

meridian::GeoChannels sampleGeo() {
  meridian::GeoChannels geo;
  geo.width = 3;
  geo.height = 2;
  geo.ratio = 4;
  geo.fieldWidth = 12;
  geo.fieldHeight = 8;
  geo.fieldSpacing = 0.5f;
  geo.texels.resize(size_t{geo.width} * geo.height * meridian::kGeoChannelCount);
  for (size_t k = 0; k < geo.texels.size(); ++k) {
    geo.texels[k] = static_cast<uint8_t>(k * 7);
  }
  return geo;
}

meridian::Heightfield flatField(uint32_t w, uint32_t h, float spacing) {
  return meridian::quantizeHeightfield(std::vector<float>(size_t{w} * h, 100.0f), w, h, spacing);
}

} // namespace

TEST_CASE("geo round trip") {
  const meridian::GeoChannels a = sampleGeo();
  const auto path = std::filesystem::temp_directory_path() / "meridian_test.geo";
  meridian::saveGeoChannels(a, path);
  const meridian::GeoChannels b = meridian::loadGeoChannels(path);
  std::filesystem::remove(path);
  CHECK(b.width == a.width);
  CHECK(b.height == a.height);
  CHECK(b.ratio == a.ratio);
  CHECK(b.fieldWidth == a.fieldWidth);
  CHECK(b.fieldHeight == a.fieldHeight);
  CHECK(b.fieldSpacing == a.fieldSpacing);
  CHECK(b.texels == a.texels);
  CHECK(b.at(2, 1, 5) == a.texels[(1 * 3 + 2) * meridian::kGeoChannelCount + 5]);
}

TEST_CASE("geo load rejects a truncated file") {
  const auto path = std::filesystem::temp_directory_path() / "meridian_test_short.geo";
  meridian::saveGeoChannels(sampleGeo(), path);
  std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
  CHECK_THROWS(meridian::loadGeoChannels(path));
  std::filesystem::remove(path);
}

TEST_CASE("geoMatches requires width = ratio x channel width") {
  meridian::GeoChannels geo;
  geo.width = geo.height = 20;
  geo.ratio = 4;
  geo.fieldWidth = geo.fieldHeight = 80;
  geo.fieldSpacing = 0.5f;
  CHECK(meridian::geoMatches(geo, flatField(80, 80, 0.5f)));
  CHECK_FALSE(meridian::geoMatches(geo, flatField(79, 80, 0.5f)));
  CHECK_FALSE(meridian::geoMatches(geo, flatField(80, 80, 1.0f)));
  geo.width = 19;
  CHECK_FALSE(meridian::geoMatches(geo, flatField(80, 80, 0.5f)));
}

TEST_CASE("geoPathFor swaps the extension") {
  CHECK(meridian::geoPathFor("a/b/napf.height") == std::filesystem::path("a/b/napf.geo"));
}
