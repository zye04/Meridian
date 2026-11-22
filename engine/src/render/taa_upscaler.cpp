#include "taa_upscaler.h"

#include "renderer_impl.h"

namespace meridian {

namespace {

// Must match TaaConstants in taa.comp and sharpen.comp.
struct TaaConstants {
  VkDeviceAddress exposure = 0;
  uint32_t historyValid = 0;
  uint32_t stable = 0; // Effects::stableTaa
  float jitter[2] = {}; // render pixels, UpscalerFrame::jitter
};
static_assert(sizeof(TaaConstants) == 24);

// Must match taa.comp and sharpen.comp, which share the set: scene colour,
// motion, previous history, then the new history and the output as storage
// images.
constexpr uint32_t kBindings = 5;
VkDescriptorType bindingType(uint32_t binding) {
  return binding >= 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                      : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

} // namespace

TaaUpscaler::TaaUpscaler(Renderer::Impl &renderer, Extent display)
    : renderer_(renderer), display_(display) {
  VkDevice dev = renderer_.device.device;

  VkDescriptorSetLayoutBinding bindings[kBindings]{};
  for (uint32_t b = 0; b < kBindings; ++b) {
    bindings[b].binding = b;
    bindings[b].descriptorType = bindingType(b);
    bindings[b].descriptorCount = 1;
    bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
  setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  setLayoutInfo.bindingCount = kBindings;
  setLayoutInfo.pBindings = bindings;
  check(vkCreateDescriptorSetLayout(dev, &setLayoutInfo, nullptr, &setLayout_),
        "vkCreateDescriptorSetLayout");

  const VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * 3},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * 2},
  };
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 2;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = poolSizes;
  check(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &pool_),
        "vkCreateDescriptorPool");
  const VkDescriptorSetLayout setLayouts[] = {setLayout_, setLayout_};
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = pool_;
  allocInfo.descriptorSetCount = 2;
  allocInfo.pSetLayouts = setLayouts;
  check(vkAllocateDescriptorSets(dev, &allocInfo, sets_.data()),
        "vkAllocateDescriptorSets");

  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                      sizeof(TaaConstants)};
  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.setLayoutCount = 1;
  layoutInfo.pSetLayouts = &setLayout_;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &pushRange;
  check(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &layout_),
        "vkCreatePipelineLayout");
  pipeline_ = createComputePipeline(dev, "taa.comp", layout_);
  sharpenPipeline_ = createComputePipeline(dev, "sharpen.comp", layout_);

  for (Image &image : history_) {
    image = renderer_.createImage(kHdrFormat, {display.width, display.height, 1},
                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
  }
  renderer_.submitNow([&](VkCommandBuffer cmd) {
    for (Image &image : history_) {
      transitionImage(cmd, {image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, 0,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    }
  });
}

TaaUpscaler::~TaaUpscaler() {
  VkDevice dev = renderer_.device.device;
  for (Image &image : history_) {
    renderer_.destroyImage(image);
  }
  vkDestroyPipeline(dev, pipeline_, nullptr);
  vkDestroyPipeline(dev, sharpenPipeline_, nullptr);
  vkDestroyPipelineLayout(dev, layout_, nullptr);
  vkDestroyDescriptorPool(dev, pool_, nullptr);
  vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
}

// Safe without waiting: the inputs and output change only when the
// swapchain is recreated, with the device idle.
void TaaUpscaler::writeSets(const UpscalerFrame &frame) {
  for (uint32_t i = 0; i < 2; ++i) {
    const VkDescriptorImageInfo images[kBindings] = {
        // Linear: the resolve reads the scene colour at display pixel centers.
        {renderer_.linearSampler, frame.color.view,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {renderer_.nearestSampler, frame.motion.view,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {renderer_.linearSampler, history_[i].view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, history_[1 - i].view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, frame.output.view, VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet writes[kBindings]{};
    for (uint32_t b = 0; b < kBindings; ++b) {
      writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[b].dstSet = sets_[i];
      writes[b].dstBinding = b;
      writes[b].descriptorCount = 1;
      writes[b].descriptorType = bindingType(b);
      writes[b].pImageInfo = &images[b];
    }
    vkUpdateDescriptorSets(renderer_.device.device, kBindings, writes, 0, nullptr);
  }
  writtenColor_ = frame.color.view;
  writtenMotion_ = frame.motion.view;
  writtenOutput_ = frame.output.view;
}

void TaaUpscaler::evaluate(const UpscalerFrame &frame) {
  if (frame.color.view != writtenColor_ || frame.motion.view != writtenMotion_ ||
      frame.output.view != writtenOutput_) {
    writeSets(frame);
  }
  VkCommandBuffer cmd = frame.cmd;
  // This frame reads the history the previous one wrote, and overwrites the
  // one the frame before that read.
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

  // sets_[1 - next_] reads history_[1 - next_] and writes history_[next_].
  const VkDescriptorSet set = sets_[1 - next_];
  TaaConstants constants;
  constants.exposure = renderer_.exposureBuffer.address;
  constants.historyValid = frame.reset ? 0u : 1u;
  constants.stable = renderer_.effects.stableTaa ? 1u : 0u;
  constants.jitter[0] = frame.jitter[0];
  constants.jitter[1] = frame.jitter[1];
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1,
                          &set, 0, nullptr);
  vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(constants), &constants);
  vkCmdDispatch(cmd, (display_.width + 7) / 8, (display_.height + 7) / 8, 1);

  // The sharpen reads the new history's neighbourhood.
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sharpenPipeline_);
  vkCmdDispatch(cmd, (display_.width + 7) / 8, (display_.height + 7) / 8, 1);
  next_ = 1 - next_;
}

} // namespace meridian
