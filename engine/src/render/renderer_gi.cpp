#include "renderer_impl.h"

#include <meridian/gi.h>

namespace meridian {

static_assert(kGiVolumeBytes ==
              VkDeviceSize{kGiLevels} * kGiColumns * kGiColumns * kGiSlices * 3 * 8);

// The diffuse albedo the ground cache reads is created with the targets, so
// turning GI on after they exist recreates them (as setAntiAliasing does).
void Renderer::setGiMode(GiMode mode) {
  impl_->giReset = impl_->giReset || mode != impl_->giMode;
  impl_->giMode = mode;
  if (mode != GiMode::Off && impl_->diffuseAlbedo.image == VK_NULL_HANDLE) {
    check(vkDeviceWaitIdle(impl_->device.device), "vkDeviceWaitIdle");
    impl_->destroySwapchain();
    impl_->createSwapchain();
  }
}

// Created on the first frame with GI on: the probe table and the ground
// cache start zeroed (valid = 0, never lit), so everything is traced fresh.
void Renderer::Impl::createGi() {
  VkDevice dev = device.device;
  constexpr VkDeviceSize kProbes =
      VkDeviceSize{kGiLevels} * kGiColumns * kGiColumns * kGiSlices;
  constexpr VkDeviceSize kTexels =
      VkDeviceSize{kGiGroundLevels} * kGiGroundTexels * kGiGroundTexels;
  constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  giBuffer = createBuffer(sizeof(GpuGiHeader) + kProbes * kGiProbeBytes, kUsage, 0);
  giGroundBuffer = createBuffer(kTexels * (kGiGroundTexelBytes + kGiGroundAccumBytes), kUsage, 0);
  giVolumeBuffer = createBuffer(kGiVolumeBytes, kUsage, 0);
  submitNow([&](VkCommandBuffer cmd) {
    vkCmdFillBuffer(cmd, giBuffer.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, giGroundBuffer.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, giVolumeBuffer.buffer, 0, VK_WHOLE_SIZE, 0);
  });
  giProbesPipeline = createComputePipeline(dev, "gi_probes.comp", sceneLayout);
  giGroundPipeline = createComputePipeline(dev, "gi_ground.comp", sceneLayout);

  VkDescriptorSetLayoutBinding bindings[2]{};
  for (uint32_t b = 0; b < 2; ++b) {
    bindings[b].binding = b;
    bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[b].descriptorCount = 1;
    bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
  setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  setLayoutInfo.bindingCount = 2;
  setLayoutInfo.pBindings = bindings;
  check(vkCreateDescriptorSetLayout(dev, &setLayoutInfo, nullptr, &giAlbedoSetLayout),
        "vkCreateDescriptorSetLayout");
  const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &poolSize;
  check(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &giAlbedoPool), "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = giAlbedoPool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &giAlbedoSetLayout;
  check(vkAllocateDescriptorSets(dev, &allocInfo, &giAlbedoSet), "vkAllocateDescriptorSets");
  writeGiAlbedoSet();

  const VkDescriptorSetLayout setLayouts[] = {atmosphereSetLayout, materialSetLayout,
                                              giAlbedoSetLayout};
  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.setLayoutCount = 3;
  layoutInfo.pSetLayouts = setLayouts;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &pushRange;
  check(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &giAlbedoLayout), "vkCreatePipelineLayout");
  giAlbedoScatterPipeline = createComputePipeline(dev, "gi_albedo_scatter.comp", giAlbedoLayout);
  giAlbedoResolvePipeline = createComputePipeline(dev, "gi_albedo_resolve.comp", giAlbedoLayout);
  giReset = true;
}

// The scene depth and diffuse albedo change with the swapchain; the device
// is idle whenever it is recreated.
void Renderer::Impl::writeGiAlbedoSet() {
  const VkDescriptorImageInfo infos[2] = {
      {nearestSampler, depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {nearestSampler, diffuseAlbedo.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
  VkWriteDescriptorSet writes[2]{};
  for (uint32_t b = 0; b < 2; ++b) {
    writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[b].dstSet = giAlbedoSet;
    writes[b].dstBinding = b;
    writes[b].descriptorCount = 1;
    writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[b].pImageInfo = &infos[b];
  }
  vkUpdateDescriptorSets(device.device, 2, writes, 0, nullptr);
}

void Renderer::Impl::destroyGi() {
  VkDevice dev = device.device;
  for (VkPipeline *pipeline : {&giProbesPipeline, &giGroundPipeline, &giAlbedoScatterPipeline,
                               &giAlbedoResolvePipeline}) {
    if (*pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(dev, *pipeline, nullptr);
      *pipeline = VK_NULL_HANDLE;
    }
  }
  if (giAlbedoLayout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(dev, giAlbedoLayout, nullptr);
    vkDestroyDescriptorPool(dev, giAlbedoPool, nullptr);
    vkDestroyDescriptorSetLayout(dev, giAlbedoSetLayout, nullptr);
    giAlbedoLayout = VK_NULL_HANDLE;
  }
  destroyBuffer(giVolumeBuffer);
  destroyBuffer(giGroundBuffer);
  destroyBuffer(giBuffer);
}

// The clipmap windows around the camera, then the probe trace. Expects the
// scene sets and push constants bound (as for sky_irradiance.comp).
void Renderer::Impl::drawGiProbes(VkCommandBuffer cmd, Vec3 cameraPosition,
                                  uint32_t frameIndex) {
  GpuGiHeader header{};
  for (uint32_t k = 0; k < kGiLevels; ++k) {
    const float spacing = giSpacing(k);
    const auto origin = giClipmapOrigin(spacing, kGiColumns, cameraPosition.x, cameraPosition.z);
    header.levels[k] = {origin[0], origin[1], spacing, giSliceBase(k)};
  }
  for (uint32_t k = 0; k < kGiGroundLevels; ++k) {
    const float texel = kGiGroundTexel0 * static_cast<float>(1u << k);
    const auto origin =
        giClipmapOrigin(texel, kGiGroundTexels, cameraPosition.x, cameraPosition.z);
    header.ground[k] = {origin[0], origin[1], texel, 0.0f};
  }
  header.frame = frameIndex;
  header.reset = giReset ? 1 : 0;
  header.groundBuffer = giGroundBuffer.address;
  header.volumeBuffer = giVolumeBuffer.address;
  giReset = false;
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
  vkCmdUpdateBuffer(cmd, giBuffer.buffer, 0, sizeof(header), &header);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, giProbesPipeline);
  constexpr uint32_t kRowGroups = kGiColumns * kGiColumns * kGiSlices / 8;
  vkCmdDispatch(cmd, kRowGroups, 8 * kGiLevels, 1);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

// The ground relight, after drawGiProbes (same bindings): it reads this
// frame's probes, which every window probe has (the probe trace reads
// last frame's ground).
void Renderer::Impl::drawGiGround(VkCommandBuffer cmd) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, giGroundPipeline);
  constexpr uint32_t kGroups = kGiGroundTexels / 8;
  vkCmdDispatch(cmd, kGroups, kGroups, kGiGroundLevels);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

// After the scene passes: scatter this frame's near-ground albedo into the
// ground cache's accumulator, then fold it into the texels. Needs depth and
// the diffuse albedo in SHADER_READ_ONLY.
void Renderer::Impl::drawGiAlbedo(VkCommandBuffer cmd, VkDeviceAddress sceneBuffer) {
  PushConstants push;
  push.sceneBuffer = sceneBuffer;
  const VkDescriptorSet sets[] = {atmosphereSet, materialSet, giAlbedoSet};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, giAlbedoLayout, 0, 3, sets, 0,
                          nullptr);
  vkCmdPushConstants(cmd, giAlbedoLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, giAlbedoScatterPipeline);
  vkCmdDispatch(cmd, (renderExtent.width + 31) / 32, (renderExtent.height + 31) / 32, 1);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, giAlbedoResolvePipeline);
  constexpr uint32_t kGroups = kGiGroundTexels / 8;
  vkCmdDispatch(cmd, kGroups, kGroups, kGiGroundLevels);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

} // namespace meridian
