// NVIDIA DLSS (Super Resolution / DLAA) through NGX, as an UpscalerProvider.
// Built only with MERIDIAN_DLSS=ON; the core library never sees NGX.
#pragma once

#include <meridian/upscaler_vk.h>

#include <filesystem>
#include <memory>

namespace meridian::dlss {

// `featureDirectory` holds the NGX snippets (libnvidia-ngx-dlss*.so),
// normally the game binary's directory. Pass the result to the Renderer,
// which it must outlive.
std::unique_ptr<UpscalerProvider> createProvider(const std::filesystem::path &featureDirectory);

// The attribution the DLSS SDK licence requires in the startup log and the
// credits.
const char *attribution();

} // namespace meridian::dlss
