#pragma once

#include <basisu_comp.h>
#include <fastgltf/types.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace meridian::cook {

enum class Usage { Color, Data, Normal };

std::vector<uint8_t> readFile(const std::filesystem::path &path);
// The encoded bytes of a glTF image: a file beside the glTF (`directory`) or
// a loaded buffer view.
std::vector<uint8_t> sourceImageBytes(const fastgltf::Asset &asset, const fastgltf::Image &image,
                                      const std::filesystem::path &directory);
// Decodes a PNG or JPEG.
basisu::image decode(const std::vector<uint8_t> &bytes);
// UASTC, Zstandard-supercompressed, with a full mip chain.
// coverageChannel >= 0 (0-3 = R, G, B, A) holds an alpha-test mask: every
// mip is rescaled in that channel so the fraction of texels above 0.5
// matches mip 0 (alpha-test coverage, Castano), and the mips are built
// here instead of by the encoder.
// Mips 1.. of an alpha-tested image whose mask is in `channel`: each keeps
// mip 0's fraction of texels above 0.5.
std::vector<basisu::image> coverageMips(const basisu::image &image, Usage usage, int channel);
std::vector<uint8_t> encodeKtx2(const basisu::image &image, Usage usage,
                                basisu::job_pool &jobPool, int coverageChannel = -1);

} // namespace meridian::cook
