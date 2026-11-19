#include "renderer_impl.h"

#include <basisu_enc.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace meridian {
namespace {

// Each level box-filtered from the one above it; the image stays in GENERAL.
void buildMips3d(VkCommandBuffer cmd, VkImage image, uint32_t size, uint32_t levels) {
  for (uint32_t level = 1; level < levels; ++level) {
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    const auto source = static_cast<int32_t>(size >> (level - 1));
    const auto destination = static_cast<int32_t>(size >> level);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
    blit.srcOffsets[1] = {source, source, source};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    blit.dstOffsets[1] = {destination, destination, destination};
    vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_GENERAL, 1,
                   &blit, VK_FILTER_LINEAR);
  }
}

} // namespace

void Renderer::Impl::createClouds() {
  VkDevice dev = device.device;
  constexpr VkFormat kNoiseFormat = VK_FORMAT_R8G8B8A8_UNORM;
  constexpr VkImageUsageFlags kNoiseUsage =
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  const auto baseLevels = static_cast<uint32_t>(std::bit_width(kBaseNoiseSize));
  const auto detailLevels = static_cast<uint32_t>(std::bit_width(kDetailNoiseSize));
  cloudBaseNoise = createImage(kNoiseFormat, {kBaseNoiseSize, kBaseNoiseSize, kBaseNoiseSize},
                               kNoiseUsage, VK_IMAGE_ASPECT_COLOR_BIT, baseLevels);
  cloudDetailNoise =
      createImage(kNoiseFormat, {kDetailNoiseSize, kDetailNoiseSize, kDetailNoiseSize},
                  kNoiseUsage, VK_IMAGE_ASPECT_COLOR_BIT, detailLevels);
  cloudCurlNoise = createImage(VK_FORMAT_R16G16B16A16_SFLOAT, {kCurlNoiseSize, kCurlNoiseSize, 1},
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
  weatherMap = createImage(kNoiseFormat, {kWeatherMapResolution, kWeatherMapResolution, 1},
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);

  const Image *volumes[] = {&cloudBaseNoise, &cloudDetailNoise};
  for (size_t i = 0; i < 2; ++i) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = volumes[i]->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = kNoiseFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(dev, &viewInfo, nullptr, &cloudNoiseLevel0[i]), "vkCreateImageView");
  }

  // Must match cloud_noise.comp and weather_map.comp.
  VkDescriptorSetLayoutBinding bindings[4]{};
  for (uint32_t b = 0; b < 4; ++b) {
    bindings[b] = {b, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  }
  VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
  setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  setLayoutInfo.bindingCount = 4;
  setLayoutInfo.pBindings = bindings;
  check(vkCreateDescriptorSetLayout(dev, &setLayoutInfo, nullptr, &cloudSetLayout),
        "vkCreateDescriptorSetLayout");
  const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4};
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &poolSize;
  check(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &cloudPool), "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = cloudPool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &cloudSetLayout;
  check(vkAllocateDescriptorSets(dev, &allocInfo, &cloudSet), "vkAllocateDescriptorSets");
  const VkDescriptorImageInfo storage[4] = {
      {VK_NULL_HANDLE, cloudNoiseLevel0[0], VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, cloudNoiseLevel0[1], VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, cloudCurlNoise.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, weatherMap.view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[4]{};
  for (uint32_t b = 0; b < 4; ++b) {
    writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[b].dstSet = cloudSet;
    writes[b].dstBinding = b;
    writes[b].descriptorCount = 1;
    writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[b].pImageInfo = &storage[b];
  }
  vkUpdateDescriptorSets(dev, 4, writes, 0, nullptr);

  // Set 1 is the bindless table clouds.glsl reads; push constants carry the
  // scene buffer.
  const VkDescriptorSetLayout setLayouts[] = {cloudSetLayout, materialSetLayout};
  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.setLayoutCount = 2;
  layoutInfo.pSetLayouts = setLayouts;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &pushRange;
  check(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &cloudLayout), "vkCreatePipelineLayout");
  weatherMapPipeline = createComputePipeline(dev, "weather_map.comp", cloudLayout);
  VkPipeline noisePipeline = createComputePipeline(dev, "cloud_noise.comp", cloudLayout);

  submitNow([&](VkCommandBuffer cmd) {
    for (const Image *image : {&cloudBaseNoise, &cloudDetailNoise, &cloudCurlNoise, &weatherMap}) {
      transitionImage(cmd, {image->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, noisePipeline);
    const VkDescriptorSet sets[] = {cloudSet, materialSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cloudLayout, 0, 2, sets, 0,
                            nullptr);
    vkCmdDispatch(cmd, kBaseNoiseSize / 4, kBaseNoiseSize / 4, kBaseNoiseSize / 4);
    buildMips3d(cmd, cloudBaseNoise.image, kBaseNoiseSize, baseLevels);
    buildMips3d(cmd, cloudDetailNoise.image, kDetailNoiseSize, detailLevels);
    for (const Image *image : {&cloudBaseNoise, &cloudDetailNoise, &cloudCurlNoise}) {
      transitionImage(cmd, {image->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
    }
  });
  vkDestroyPipeline(dev, noisePipeline, nullptr);

  if (textures.size() + 4 > kMaxTextures) {
    throw std::runtime_error("texture table is full");
  }
  const auto first = static_cast<uint32_t>(textures.size());
  textures.resize(textures.size() + 4);
  clouds.weatherMap = first;
  clouds.baseNoise = first + 1;
  clouds.detailNoise = first + 2;
  clouds.curlNoise = first + 3;
  writeTexture(clouds.weatherMap, weatherMap.view, linearSampler, VK_IMAGE_LAYOUT_GENERAL);
  writeTexture(clouds.baseNoise, cloudBaseNoise.view, textureSampler);
  writeTexture(clouds.detailNoise, cloudDetailNoise.view, textureSampler);
  writeTexture(clouds.curlNoise, cloudCurlNoise.view, textureSampler);
}

void Renderer::Impl::destroyClouds() {
  VkDevice dev = device.device;
  vkDestroyPipeline(dev, weatherMapPipeline, nullptr);
  vkDestroyPipelineLayout(dev, cloudLayout, nullptr);
  vkDestroyDescriptorPool(dev, cloudPool, nullptr);
  vkDestroyDescriptorSetLayout(dev, cloudSetLayout, nullptr);
  for (VkImageView view : cloudNoiseLevel0) {
    vkDestroyImageView(dev, view, nullptr);
  }
  for (Image *image : {&cloudBaseNoise, &cloudDetailNoise, &cloudCurlNoise, &weatherMap}) {
    destroyImage(*image);
  }
}

// Centered on the camera, snapped to whole texels so the map doesn't swim as
// the camera moves (computed in double: far from the origin, float would
// round the snap).
void Renderer::Impl::placeWeatherMap(Vec3 cameraPosition) {
  constexpr double kTexel = kWeatherMapSize / kWeatherMapResolution;
  const double x = std::floor(cameraPosition.x / kTexel) * kTexel - 0.5 * kWeatherMapSize;
  const double z = std::floor(cameraPosition.z / kTexel) * kTexel - 0.5 * kWeatherMapSize;
  clouds.mapOrigin[0] = static_cast<float>(x);
  clouds.mapOrigin[1] = static_cast<float>(z);
}

// Runs after a barrier that waits for the previous frame's reads (the map is
// shared by both frames in flight).
void Renderer::Impl::drawWeatherMap(VkCommandBuffer cmd, const FrameData &frame) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, weatherMapPipeline);
  const VkDescriptorSet sets[] = {cloudSet, materialSet};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cloudLayout, 0, 2, sets, 0,
                          nullptr);
  PushConstants push;
  push.sceneBuffer = frame.sceneBuffer.address;
  vkCmdPushConstants(cmd, cloudLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  vkCmdDispatch(cmd, kWeatherMapResolution / 8, kWeatherMapResolution / 8, 1);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void Renderer::dumpWeatherMap(const std::filesystem::path &path) {
  check(vkDeviceWaitIdle(impl_->device.device), "vkDeviceWaitIdle");
  constexpr uint32_t kSize = kWeatherMapResolution;
  Buffer readback = impl_->createBuffer(
      VkDeviceSize{kSize} * kSize * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
  impl_->submitNow([&](VkCommandBuffer cmd) {
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kSize, kSize, 1};
    vkCmdCopyImageToBuffer(cmd, impl_->weatherMap.image, VK_IMAGE_LAYOUT_GENERAL,
                           readback.buffer, 1, &region);
  });
  check(vmaInvalidateAllocation(impl_->allocator, readback.allocation, 0, VK_WHOLE_SIZE),
        "vmaInvalidateAllocation");
  basisu::image png(kSize, kSize);
  const auto *rgba = static_cast<const uint8_t *>(readback.mapped);
  for (uint32_t i = 0; i < kSize * kSize; ++i) {
    png.get_ptr()[i].set(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], 255);
  }
  if (!basisu::save_png(path.string(), png)) {
    std::fprintf(stderr, "Weather map dump failed: %s\n", path.c_str());
  }
  impl_->destroyBuffer(readback);
  std::printf("Weather map: %u^2 texels, %.0f m from (%.1f, %.1f)\n", kSize, kWeatherMapSize,
              impl_->clouds.mapOrigin[0], impl_->clouds.mapOrigin[1]);
}

} // namespace meridian
