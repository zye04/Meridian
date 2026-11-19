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

// The cloud march resolution: [0] quarter, [1] half of the screen, rounded up.
VkExtent2D cloudTargetExtent(VkExtent2D screen, uint32_t resolution) {
  const uint32_t divisor = resolution == 0 ? 4 : 2;
  return {(screen.width + divisor - 1) / divisor, (screen.height + divisor - 1) / divisor};
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
  // The generator set, then one march set per resolution.
  const VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 + 2 * 2},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * 3}};
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 3;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = poolSizes;
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
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  });
  vkDestroyPipeline(dev, noisePipeline, nullptr);

  // Must match clouds_march.comp and clouds_composite.frag.
  constexpr VkShaderStageFlags kMarchStages =
      VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  const VkDescriptorSetLayoutBinding marchBindings[] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kMarchStages, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  setLayoutInfo.bindingCount = 5;
  setLayoutInfo.pBindings = marchBindings;
  check(vkCreateDescriptorSetLayout(dev, &setLayoutInfo, nullptr, &cloudMarchSetLayout),
        "vkCreateDescriptorSetLayout");
  const VkDescriptorSetLayout marchSetLayouts[] = {cloudMarchSetLayout, cloudMarchSetLayout};
  allocInfo.descriptorSetCount = 2;
  allocInfo.pSetLayouts = marchSetLayouts;
  check(vkAllocateDescriptorSets(dev, &allocInfo, cloudMarchSets.data()),
        "vkAllocateDescriptorSets");
  // Sets 0 and 1 as in the scene layout (atmosphere LUTs, bindless).
  const VkDescriptorSetLayout marchLayouts[] = {atmosphereSetLayout, materialSetLayout,
                                                cloudMarchSetLayout};
  const VkPushConstantRange marchPushRange{kMarchStages, 0, sizeof(PushConstants)};
  layoutInfo.setLayoutCount = 3;
  layoutInfo.pSetLayouts = marchLayouts;
  layoutInfo.pPushConstantRanges = &marchPushRange;
  check(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &cloudMarchLayout),
        "vkCreatePipelineLayout");
  cloudMarchPipeline = createComputePipeline(dev, "clouds_march.comp", cloudMarchLayout);
  const VkFormat hdrFormat[] = {kHdrFormat};
  cloudCompositePipeline = createGraphicsPipeline(dev, {.vertexShader = "fullscreen.vert",
                                                        .fragmentShader = "clouds_composite.frag",
                                                        .layout = cloudMarchLayout,
                                                        .colorFormats = hdrFormat,
                                                        .alphaBlend = true});
  createCloudTargets();

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
  writeTexture(clouds.baseNoise, cloudBaseNoise.view, textureSampler, VK_IMAGE_LAYOUT_GENERAL);
  writeTexture(clouds.detailNoise, cloudDetailNoise.view, textureSampler, VK_IMAGE_LAYOUT_GENERAL);
  writeTexture(clouds.curlNoise, cloudCurlNoise.view, textureSampler, VK_IMAGE_LAYOUT_GENERAL);
}

void Renderer::Impl::destroyClouds() {
  VkDevice dev = device.device;
  vkDestroyPipeline(dev, cloudCompositePipeline, nullptr);
  vkDestroyPipeline(dev, cloudMarchPipeline, nullptr);
  vkDestroyPipelineLayout(dev, cloudMarchLayout, nullptr);
  vkDestroyDescriptorSetLayout(dev, cloudMarchSetLayout, nullptr);
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

// Called with the swapchain (the targets follow the render size) and once the
// march sets exist; the device is idle either way.
void Renderer::Impl::createCloudTargets() {
  if (cloudMarchSets[0] == VK_NULL_HANDLE) {
    return;
  }
  constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  for (uint32_t r = 0; r < 2; ++r) {
    const VkExtent2D size = cloudTargetExtent(renderExtent, r);
    const VkExtent3D extent{size.width, size.height, 1};
    cloudColor[r] = createImage(kHdrFormat, extent, kUsage, VK_IMAGE_ASPECT_COLOR_BIT);
    cloudDepth[r] = createImage(VK_FORMAT_R32_SFLOAT, extent, kUsage, VK_IMAGE_ASPECT_COLOR_BIT);
  }
  submitNow([&](VkCommandBuffer cmd) {
    for (uint32_t r = 0; r < 2; ++r) {
      for (const Image *image : {&cloudColor[r], &cloudDepth[r]}) {
        transitionImage(cmd, {image->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
      }
    }
  });
  for (uint32_t r = 0; r < 2; ++r) {
    const VkDescriptorImageInfo images[] = {
        {nearestSampler, depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, cloudColor[r].view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, cloudDepth[r].view, VK_IMAGE_LAYOUT_GENERAL},
        {linearSampler, cloudColor[r].view, VK_IMAGE_LAYOUT_GENERAL},
        {linearSampler, cloudDepth[r].view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t b = 0; b < 5; ++b) {
      writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[b].dstSet = cloudMarchSets[r];
      writes[b].dstBinding = b;
      writes[b].descriptorCount = 1;
      writes[b].descriptorType = b == 1 || b == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                  : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[b].pImageInfo = &images[b];
    }
    vkUpdateDescriptorSets(device.device, 5, writes, 0, nullptr);
  }
}

void Renderer::Impl::destroyCloudTargets() {
  for (uint32_t r = 0; r < 2; ++r) {
    if (cloudColor[r].image != VK_NULL_HANDLE) {
      destroyImage(cloudColor[r]);
      destroyImage(cloudDepth[r]);
    }
  }
}

// Inside the local layer or within kCloudLayerMargin of it, the march runs
// at half resolution: clouds fill the view up close, where quarter
// resolution would blur them.
bool Renderer::Impl::cameraInCloudLayer(Vec3 cameraPosition) const {
  constexpr float kCloudLayerMargin = 250.0f;
  return cameraPosition.y > cameraCloudLayer[0] - kCloudLayerMargin &&
         cameraPosition.y < cameraCloudLayer[1] + kCloudLayerMargin;
}

// After the late scene pass: the march (compute) into this resolution's
// targets, then the composite over the HDR target (still a color
// attachment). Leaves the depth image in SHADER_READ_ONLY_OPTIMAL.
void Renderer::Impl::drawClouds(VkCommandBuffer cmd, const FrameData &frame,
                                Vec3 cameraPosition) {
  transitionImage(cmd, {depth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
  // The targets are shared by both frames in flight: wait for the previous
  // frame's composite before overwriting them.
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

  const uint32_t r = cameraInCloudLayer(cameraPosition) ? 1 : 0;
  const VkDescriptorSet sets[] = {atmosphereSet, materialSet, cloudMarchSets[r]};
  PushConstants push;
  push.sceneBuffer = frame.sceneBuffer.address;
  constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cloudMarchPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cloudMarchLayout, 0, 3, sets, 0,
                          nullptr);
  vkCmdPushConstants(cmd, cloudMarchLayout, kStages, 0, sizeof(push), &push);
  const VkExtent2D size = cloudTargetExtent(renderExtent, r);
  vkCmdDispatch(cmd, (size.width + 7) / 8, (size.height + 7) / 8, 1);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

  VkRenderingAttachmentInfo color{};
  color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  color.imageView = hdr.view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  rendering.renderArea = {{0, 0}, renderExtent};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  vkCmdBeginRendering(cmd, &rendering);
  VkViewport viewport{};
  viewport.width = static_cast<float>(renderExtent.width);
  viewport.height = static_cast<float>(renderExtent.height);
  viewport.maxDepth = 1.0f;
  const VkRect2D scissor{{0, 0}, renderExtent};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cloudCompositePipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cloudMarchLayout, 0, 3, sets, 0,
                          nullptr);
  vkCmdPushConstants(cmd, cloudMarchLayout, kStages, 0, sizeof(push), &push);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
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
