#pragma once

#include <basisu_comp.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace meridian::cook {

enum class Usage { Color, Data, Normal };

std::vector<uint8_t> readFile(const std::filesystem::path &path);
// Decodes a PNG or JPEG.
basisu::image decode(const std::vector<uint8_t> &bytes);
// UASTC, Zstandard-supercompressed, with a full mip chain.
std::vector<uint8_t> encodeKtx2(const basisu::image &image, Usage usage,
                                basisu::job_pool &jobPool);

} // namespace meridian::cook
