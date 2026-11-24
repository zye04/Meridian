#include "renderer_impl.h"

#include <basisu_enc.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace meridian {
namespace {

// Each level box-filtered from the one above it; the image stays in GENERAL.
void buildMips3d(VkCommandBuffer cmd, VkImage image, uint32_t size, uint32_t levels) {
  for (uint32_t level = 1; level < levels; ++level) {
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
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

constexpr uint32_t kCloudMarchBindings = 11;

VkDescriptorType cloudMarchDescriptorType(uint32_t binding) {
  return binding == 0 || binding == 3 || binding == 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
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
  for (Image *image : {&cloudShadowRaw, &cloudShadowMap}) {
    *image = createImage(VK_FORMAT_R16G16B16A16_SFLOAT,
                         {kCloudShadowResolution, kCloudShadowResolution, 1},
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT);
  }
  skyCapture = createImage(VK_FORMAT_R16G16B16A16_SFLOAT,
                           {kSkyCaptureResolution, kSkyCaptureResolution, 1},
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
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

  // Must match cloud_noise.comp, weather_map.comp, cloud_shadow.comp and
  // cloud_shadow_blur.comp.
  constexpr uint32_t kCloudBindings = 6;
  VkDescriptorSetLayoutBinding bindings[kCloudBindings]{};
  for (uint32_t b = 0; b < kCloudBindings; ++b) {
    bindings[b] = {b, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  }
  VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
  setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  setLayoutInfo.bindingCount = kCloudBindings;
  setLayoutInfo.pBindings = bindings;
  check(vkCreateDescriptorSetLayout(dev, &setLayoutInfo, nullptr, &cloudSetLayout),
        "vkCreateDescriptorSetLayout");
  // The generator set, then one march set per resolution and parity.
  const VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kCloudBindings + 4 * 7},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * 3}};
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 5;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = poolSizes;
  check(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &cloudPool), "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = cloudPool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &cloudSetLayout;
  check(vkAllocateDescriptorSets(dev, &allocInfo, &cloudSet), "vkAllocateDescriptorSets");
  const VkDescriptorImageInfo storage[kCloudBindings] = {
      {VK_NULL_HANDLE, cloudNoiseLevel0[0], VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, cloudNoiseLevel0[1], VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, cloudCurlNoise.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, weatherMap.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, cloudShadowRaw.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, cloudShadowMap.view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[kCloudBindings]{};
  for (uint32_t b = 0; b < kCloudBindings; ++b) {
    writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[b].dstSet = cloudSet;
    writes[b].dstBinding = b;
    writes[b].descriptorCount = 1;
    writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[b].pImageInfo = &storage[b];
  }
  vkUpdateDescriptorSets(dev, kCloudBindings, writes, 0, nullptr);

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
  cloudShadowPipeline = createComputePipeline(dev, "cloud_shadow.comp", cloudLayout);
  cloudShadowBlurPipeline = createComputePipeline(dev, "cloud_shadow_blur.comp", cloudLayout);
  VkPipeline noisePipeline = createComputePipeline(dev, "cloud_noise.comp", cloudLayout);

  submitNow([&](VkCommandBuffer cmd) {
    for (const Image *image : {&cloudBaseNoise, &cloudDetailNoise, &cloudCurlNoise, &weatherMap,
                               &cloudShadowRaw, &cloudShadowMap, &skyCapture}) {
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

  // Must match clouds_march.comp, clouds_resolve.comp and
  // clouds_composite.comp: scene depth, the march targets, the history to
  // read (color, depth) and to write, HDR and motion; and sky_capture.comp's
  // capture.
  VkDescriptorSetLayoutBinding marchBindings[kCloudMarchBindings]{};
  for (uint32_t b = 0; b < kCloudMarchBindings; ++b) {
    marchBindings[b] = {b, cloudMarchDescriptorType(b), 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  }
  setLayoutInfo.bindingCount = kCloudMarchBindings;
  setLayoutInfo.pBindings = marchBindings;
  check(vkCreateDescriptorSetLayout(dev, &setLayoutInfo, nullptr, &cloudMarchSetLayout),
        "vkCreateDescriptorSetLayout");
  const VkDescriptorSetLayout marchSetLayouts[] = {cloudMarchSetLayout, cloudMarchSetLayout,
                                                   cloudMarchSetLayout, cloudMarchSetLayout};
  allocInfo.descriptorSetCount = 4;
  allocInfo.pSetLayouts = marchSetLayouts;
  check(vkAllocateDescriptorSets(dev, &allocInfo, cloudMarchSets.data()),
        "vkAllocateDescriptorSets");
  // Sets 0 and 1 as in the scene layout (atmosphere LUTs, bindless).
  const VkDescriptorSetLayout marchLayouts[] = {atmosphereSetLayout, materialSetLayout,
                                                cloudMarchSetLayout};
  const VkPushConstantRange marchPushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                           sizeof(CloudConstants)};
  layoutInfo.setLayoutCount = 3;
  layoutInfo.pSetLayouts = marchLayouts;
  layoutInfo.pPushConstantRanges = &marchPushRange;
  check(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &cloudMarchLayout),
        "vkCreatePipelineLayout");
  cloudMarchPipeline = createComputePipeline(dev, "clouds_march.comp", cloudMarchLayout);
  cloudResolvePipeline = createComputePipeline(dev, "clouds_resolve.comp", cloudMarchLayout);
  cloudCompositePipeline = createComputePipeline(dev, "clouds_composite.comp", cloudMarchLayout);
  skyCapturePipeline = createComputePipeline(dev, "sky_capture.comp", cloudMarchLayout);
  createCloudTargets();

  if (textures.size() + 6 > kMaxTextures) {
    throw std::runtime_error("texture table is full");
  }
  const auto first = static_cast<uint32_t>(textures.size());
  textures.resize(textures.size() + 6);
  clouds.weatherMap = first;
  clouds.baseNoise = first + 1;
  clouds.detailNoise = first + 2;
  clouds.curlNoise = first + 3;
  clouds.shadowMap = first + 4;
  clouds.skyCapture = first + 5;
  writeTexture(clouds.weatherMap, weatherMap.view, linearSampler, VK_IMAGE_LAYOUT_GENERAL);
  writeTexture(clouds.baseNoise, cloudBaseNoise.view, textureSampler, VK_IMAGE_LAYOUT_GENERAL);
  writeTexture(clouds.detailNoise, cloudDetailNoise.view, textureSampler, VK_IMAGE_LAYOUT_GENERAL);
  writeTexture(clouds.curlNoise, cloudCurlNoise.view, textureSampler, VK_IMAGE_LAYOUT_GENERAL);
  writeTexture(clouds.shadowMap, cloudShadowMap.view, linearSampler, VK_IMAGE_LAYOUT_GENERAL);
  writeTexture(clouds.skyCapture, skyCapture.view, linearSampler, VK_IMAGE_LAYOUT_GENERAL);
}

void Renderer::Impl::destroyClouds() {
  VkDevice dev = device.device;
  vkDestroyPipeline(dev, skyCapturePipeline, nullptr);
  vkDestroyPipeline(dev, cloudCompositePipeline, nullptr);
  vkDestroyPipeline(dev, cloudResolvePipeline, nullptr);
  vkDestroyPipeline(dev, cloudMarchPipeline, nullptr);
  vkDestroyPipelineLayout(dev, cloudMarchLayout, nullptr);
  vkDestroyDescriptorSetLayout(dev, cloudMarchSetLayout, nullptr);
  vkDestroyPipeline(dev, cloudShadowBlurPipeline, nullptr);
  vkDestroyPipeline(dev, cloudShadowPipeline, nullptr);
  vkDestroyPipeline(dev, weatherMapPipeline, nullptr);
  vkDestroyPipelineLayout(dev, cloudLayout, nullptr);
  vkDestroyDescriptorPool(dev, cloudPool, nullptr);
  vkDestroyDescriptorSetLayout(dev, cloudSetLayout, nullptr);
  for (VkImageView view : cloudNoiseLevel0) {
    vkDestroyImageView(dev, view, nullptr);
  }
  for (Image *image : {&cloudBaseNoise, &cloudDetailNoise, &cloudCurlNoise, &weatherMap,
                       &cloudShadowRaw, &cloudShadowMap, &skyCapture}) {
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

// Orthographic along the sun (cloudShadowBasis), centered on the camera and
// snapped to whole texels in the sun's frame (in double, like the weather
// map). The grid moves with the clouds (cloudWindTravel): a texel keeps
// marching the same part of a cloud, so a shadow edge glides with the
// bilinear lookup instead of jumping a texel when it crosses a texel's
// centre. Off with the clouds.
void Renderer::Impl::placeCloudShadow(Vec3 cameraPosition, Vec3 sunDirection) {
  if (!cloudsEnabled) {
    clouds.shadowSize = 0.0f;
    return;
  }
  const SunBasis basis = cloudShadowBasis(sunDirection);
  constexpr double kTexel = kCloudShadowSize / kCloudShadowResolution;
  const double x = double{cameraPosition.x} - cloudWindTravel[0];
  const double z = double{cameraPosition.z} - cloudWindTravel[1];
  const Vec3 *axes[] = {&basis.right, &basis.up, &basis.forward};
  float *targets[] = {clouds.shadowRight, clouds.shadowUp, clouds.shadowForward};
  for (int i = 0; i < 3; ++i) {
    targets[i][0] = axes[i]->x;
    targets[i][1] = axes[i]->y;
    targets[i][2] = axes[i]->z;
  }
  clouds.shadowSize = static_cast<float>(kCloudShadowSize);
  // Half the rows per frame keep their texels' clouds only while the grid
  // stays put; a step of the grid shifts what every texel stands for.
  cloudShadowFullRefresh = !historyValid;
  for (int i = 0; i < 2; ++i) {
    const Vec3 &axis = i == 0 ? basis.right : basis.up;
    const double u = x * axis.x + double{cameraPosition.y} * axis.y + z * axis.z;
    const double snap = std::floor(u / kTexel);
    clouds.shadowCamera[i] = static_cast<float>(u - snap * kTexel);
    cloudShadowFullRefresh |= static_cast<int64_t>(snap) != cloudShadowSnap[i];
    cloudShadowSnap[i] = static_cast<int64_t>(snap);
  }
}

// After the weather map (the march reads it): the Beer shadow map, then its
// blur into the map the scene and the cloud march sample.
void Renderer::Impl::drawCloudShadow(VkCommandBuffer cmd, const FrameData &frame) {
  const VkDescriptorSet sets[] = {cloudSet, materialSet};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cloudLayout, 0, 2, sets, 0,
                          nullptr);
  PushConstants push;
  push.sceneBuffer = frame.sceneBuffer.address;
  // Half the rows per frame once a full map exists and the grid kept still
  // (cloud_shadow.comp, placeCloudShadow).
  push.drawList = cloudShadowFullRefresh ? 0 : 1;
  vkCmdPushConstants(cmd, cloudLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  for (VkPipeline pipeline : {cloudShadowPipeline, cloudShadowBlurPipeline}) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    const uint32_t rows = pipeline == cloudShadowPipeline && !cloudShadowFullRefresh
                              ? kCloudShadowResolution / 2
                              : kCloudShadowResolution;
    vkCmdDispatch(cmd, kCloudShadowResolution / 8, rows / 8, 1);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  }
}

// After the cloud shadow map (the march reads it): one texel of every 2x2
// block of the sky capture, all of them when the history resets (first
// frame, swapchain rebuild). Any march set serves: they share the capture
// binding.
void Renderer::Impl::drawSkyCapture(VkCommandBuffer cmd, const FrameData &frame) {
  const VkDescriptorSet sets[] = {atmosphereSet, materialSet, cloudMarchSets[0]};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cloudMarchLayout, 0, 3, sets, 0,
                          nullptr);
  CloudConstants push;
  push.sceneBuffer = frame.sceneBuffer.address;
  push.historyValid = historyValid ? 1 : 0;
  vkCmdPushConstants(cmd, cloudMarchLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skyCapturePipeline);
  const uint32_t texels = historyValid ? kSkyCaptureResolution / 2 : kSkyCaptureResolution;
  vkCmdDispatch(cmd, texels / 8, texels / 8, 1);
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
  std::vector<const Image *> images;
  for (uint32_t r = 0; r < 2; ++r) {
    const VkExtent2D size = cloudTargetExtent(renderExtent, r);
    const VkExtent3D extent{size.width, size.height, 1};
    cloudColor[r] = createImage(kHdrFormat, extent, kUsage, VK_IMAGE_ASPECT_COLOR_BIT);
    cloudDepth[r] = createImage(VK_FORMAT_R32_SFLOAT, extent, kUsage, VK_IMAGE_ASPECT_COLOR_BIT);
    images.insert(images.end(), {&cloudColor[r], &cloudDepth[r]});
    for (uint32_t i = 0; i < 2; ++i) {
      cloudHistory[r][i] = createImage(kHdrFormat, extent, kUsage, VK_IMAGE_ASPECT_COLOR_BIT);
      cloudHistoryDepth[r][i] =
          createImage(VK_FORMAT_R32_SFLOAT, extent, kUsage, VK_IMAGE_ASPECT_COLOR_BIT);
      images.insert(images.end(), {&cloudHistory[r][i], &cloudHistoryDepth[r][i]});
    }
  }
  submitNow([&](VkCommandBuffer cmd) {
    for (const Image *image : images) {
      transitionImage(cmd, {image->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    }
  });
  cloudHistoryResolution = 2;
  // Set r * 2 + i writes history i and reads the other one.
  for (uint32_t r = 0; r < 2; ++r) {
    for (uint32_t i = 0; i < 2; ++i) {
      const VkDescriptorImageInfo infos[kCloudMarchBindings] = {
          {nearestSampler, depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {VK_NULL_HANDLE, cloudColor[r].view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, cloudDepth[r].view, VK_IMAGE_LAYOUT_GENERAL},
          {linearSampler, cloudHistory[r][1 - i].view, VK_IMAGE_LAYOUT_GENERAL},
          {nearestSampler, cloudHistoryDepth[r][1 - i].view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, cloudHistory[r][i].view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, cloudHistoryDepth[r][i].view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, hdr.view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, motion.view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, skyCapture.view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, transparencyMask.view, VK_IMAGE_LAYOUT_GENERAL}};
      VkWriteDescriptorSet writes[kCloudMarchBindings]{};
      for (uint32_t b = 0; b < kCloudMarchBindings; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = cloudMarchSets[r * 2 + i];
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = cloudMarchDescriptorType(b);
        writes[b].pImageInfo = &infos[b];
      }
      vkUpdateDescriptorSets(device.device, kCloudMarchBindings, writes, 0, nullptr);
    }
  }
}

void Renderer::Impl::destroyCloudTargets() {
  for (uint32_t r = 0; r < 2; ++r) {
    if (cloudColor[r].image != VK_NULL_HANDLE) {
      destroyImage(cloudColor[r]);
      destroyImage(cloudDepth[r]);
      for (uint32_t i = 0; i < 2; ++i) {
        destroyImage(cloudHistory[r][i]);
        destroyImage(cloudHistoryDepth[r][i]);
      }
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

// After the late scene pass, all compute: the march into this resolution's
// targets, the temporal resolve into its history, then the composite into
// HDR, motion and the transparency mask. Leaves depth in
// SHADER_READ_ONLY_OPTIMAL and HDR, motion and the mask in GENERAL.
void Renderer::Impl::drawClouds(VkCommandBuffer cmd, const FrameData &frame,
                                Vec3 cameraPosition) {
  constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  constexpr VkAccessFlags2 kStorage =
      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  transitionImage(cmd, {depth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kCompute,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
  for (VkImage target : {hdr.image, motion.image, transparencyMask.image}) {
    transitionImage(cmd, {target, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kCompute, kStorage});
  }
  // The targets and histories are shared by both frames in flight: the
  // previous frame's resolve wrote the history this one reads, and its
  // passes read what this one overwrites.
  memoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute,
                kStorage | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

  const uint32_t r = cameraInCloudLayer(cameraPosition) ? 1 : 0;
  const VkDescriptorSet sets[] = {atmosphereSet, materialSet,
                                  cloudMarchSets[r * 2 + frameNumber % 2]};
  CloudConstants push;
  push.sceneBuffer = frame.sceneBuffer.address;
  // A new resolution starts its history over.
  push.historyValid = historyValid && r == cloudHistoryResolution ? 1 : 0;
  push.inLayer = r;
  push.outline = effects.cloudOutline;
  cloudHistoryResolution = r;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cloudMarchLayout, 0, 3, sets, 0,
                          nullptr);
  vkCmdPushConstants(cmd, cloudMarchLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  const VkExtent2D size = cloudTargetExtent(renderExtent, r);
  for (VkPipeline pipeline : {cloudMarchPipeline, cloudResolvePipeline}) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdDispatch(cmd, (size.width + 7) / 8, (size.height + 7) / 8, 1);
    memoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, kStorage);
  }
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cloudCompositePipeline);
  vkCmdDispatch(cmd, (renderExtent.width + 7) / 8, (renderExtent.height + 7) / 8, 1);
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
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
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

void Renderer::printAmbient() {
  check(vkDeviceWaitIdle(impl_->device.device), "vkDeviceWaitIdle");
  Buffer readback = impl_->createBuffer(
      sizeof(SkyIrradiance) + sizeof(ExposureState), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
  impl_->submitNow([&](VkCommandBuffer cmd) {
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferCopy irradiance{0, 0, sizeof(SkyIrradiance)};
    vkCmdCopyBuffer(cmd, impl_->skyIrradianceBuffer.buffer, readback.buffer, 1, &irradiance);
    const VkBufferCopy exposure{0, sizeof(SkyIrradiance), sizeof(ExposureState)};
    vkCmdCopyBuffer(cmd, impl_->exposureBuffer.buffer, readback.buffer, 1, &exposure);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  });
  check(vmaInvalidateAllocation(impl_->allocator, readback.allocation, 0, VK_WHOLE_SIZE),
        "vmaInvalidateAllocation");
  SkyIrradiance sky;
  ExposureState state;
  std::memcpy(&sky, readback.mapped, sizeof(sky));
  std::memcpy(&state, static_cast<const char *>(readback.mapped) + sizeof(sky), sizeof(state));
  impl_->destroyBuffer(readback);
  std::printf("Exposure EV100 %.2f; sky irradiance (%.4f, %.4f, %.4f), clear sky (%.4f, %.4f, "
              "%.4f) x sun; open ground %.0f lux\n",
              state.ev100, sky.irradiance[0], sky.irradiance[1], sky.irradiance[2],
              sky.clearIrradiance[0], sky.clearIrradiance[1], sky.clearIrradiance[2],
              sky.openIlluminance);
}

} // namespace meridian
