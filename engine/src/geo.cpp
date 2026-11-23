#include <meridian/geo.h>
#include <meridian/terrain.h>

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace meridian {

namespace {

struct GeoHeader {
  char magic[4];
  uint32_t version;
  uint32_t width, height, ratio;
  uint32_t fieldWidth, fieldHeight;
  float fieldSpacing;
  uint32_t channelCount;
};
static_assert(sizeof(GeoHeader) == 36);

} // namespace

void saveGeoChannels(const GeoChannels &geo, const std::filesystem::path &path) {
  const GeoHeader header{{'M', 'G', 'C', '1'}, 1,           geo.width,
                         geo.height,           geo.ratio,   geo.fieldWidth,
                         geo.fieldHeight,      geo.fieldSpacing, kGeoChannelCount};
  std::ofstream stream(path, std::ios::binary);
  stream.write(reinterpret_cast<const char *>(&header), sizeof(header));
  stream.write(reinterpret_cast<const char *>(geo.texels.data()),
               static_cast<std::streamsize>(geo.texels.size()));
  if (!stream) {
    throw std::runtime_error("cannot write " + path.string());
  }
}

GeoChannels loadGeoChannels(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot open " + path.string());
  }
  GeoHeader header;
  stream.read(reinterpret_cast<char *>(&header), sizeof(header));
  if (!stream || std::memcmp(header.magic, "MGC1", 4) != 0 || header.version != 1 ||
      header.channelCount != kGeoChannelCount || header.width == 0 || header.height == 0) {
    throw std::runtime_error("not a geo channel file: " + path.string());
  }
  const uint64_t expected =
      sizeof(header) + uint64_t{header.width} * header.height * kGeoChannelCount;
  if (std::filesystem::file_size(path) != expected) {
    throw std::runtime_error("geo channel file has the wrong size: " + path.string());
  }
  GeoChannels geo;
  geo.width = header.width;
  geo.height = header.height;
  geo.ratio = header.ratio;
  geo.fieldWidth = header.fieldWidth;
  geo.fieldHeight = header.fieldHeight;
  geo.fieldSpacing = header.fieldSpacing;
  geo.texels.resize(static_cast<size_t>(expected - sizeof(header)));
  stream.read(reinterpret_cast<char *>(geo.texels.data()),
              static_cast<std::streamsize>(geo.texels.size()));
  if (!stream) {
    throw std::runtime_error("truncated geo channel file: " + path.string());
  }
  return geo;
}

bool geoMatches(const GeoChannels &geo, const Heightfield &field) {
  return geo.ratio > 0 && geo.fieldWidth == field.width && geo.fieldHeight == field.height &&
         geo.fieldSpacing == field.spacing && geo.width * geo.ratio == field.width &&
         geo.height * geo.ratio == field.height;
}

std::filesystem::path geoPathFor(const std::filesystem::path &heightPath) {
  std::filesystem::path path = heightPath;
  return path.replace_extension(".geo");
}

} // namespace meridian
