// The core's Upscaler backend: temporal anti-aliasing (taa.comp) with a
// ping-ponged, display-sized history of its own, then an output-only
// sharpen (sharpen.comp).
#pragma once

#include "gpu_types.h"

#include <meridian/renderer.h>
#include <meridian/upscaler_vk.h>

#include <array>

namespace meridian {

class TaaUpscaler final : public Upscaler {
public:
  // History at the display size. `renderer` outlives this.
  TaaUpscaler(Renderer::Impl &renderer, Extent display);
  ~TaaUpscaler() override;
  TaaUpscaler(const TaaUpscaler &) = delete;
  TaaUpscaler &operator=(const TaaUpscaler &) = delete;

  void evaluate(const UpscalerFrame &frame) override;
  bool wantsGuides() const override { return false; }

private:
  void writeSets(const UpscalerFrame &frame);

  Renderer::Impl &renderer_;
  // taa.comp writes the resolve to the history; sharpen.comp sharpens it
  // into the frame's output, so the sharpening never feeds back.
  // Always in VK_IMAGE_LAYOUT_GENERAL.
  std::array<Image, 2> history_;
  // sets_[i] reads history_[i] and writes history_[1 - i].
  std::array<VkDescriptorSet, 2> sets_{};
  uint32_t next_ = 0;
  // The views sets_ point at: the inputs change only with the swapchain.
  VkImageView writtenColor_ = VK_NULL_HANDLE;
  VkImageView writtenMotion_ = VK_NULL_HANDLE;
  VkImageView writtenOutput_ = VK_NULL_HANDLE;
  Extent display_;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkPipeline sharpenPipeline_ = VK_NULL_HANDLE;
};

} // namespace meridian
