// Terrain: heightfield upload, CDLOD node selection on the GPU (one
// dispatch per quadtree level, terrain_select.comp) and the terrain draw
// (terrain.mesh + terrain.frag).
#include "renderer_impl.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace meridian {

// A GPU-only storage buffer holding `bytes`.
Buffer Renderer::Impl::uploadStorage(const void *data, VkDeviceSize bytes) {
  Buffer buffer = createBuffer(bytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               0);
  Buffer staging = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                    VMA_ALLOCATION_CREATE_MAPPED_BIT);
  std::memcpy(staging.mapped, data, bytes);
  check(vmaFlushAllocation(allocator, staging.allocation, 0, VK_WHOLE_SIZE),
        "vmaFlushAllocation");
  submitNow([&](VkCommandBuffer cmd) {
    const VkBufferCopy copy{0, 0, bytes};
    vkCmdCopyBuffer(cmd, staging.buffer, buffer.buffer, 1, &copy);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  });
  destroyBuffer(staging);
  return buffer;
}

// Points bindless slot `index` (material.glsl's textures[]) at `view`.
void Renderer::Impl::writeTexture(uint32_t index, VkImageView view, VkSampler sampler,
                                  VkImageLayout layout) {
  const VkDescriptorImageInfo imageInfo{sampler, view, layout};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = materialSet;
  write.dstBinding = 1;
  write.dstArrayElement = index;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &imageInfo;
  vkUpdateDescriptorSets(device.device, 1, &write, 0, nullptr);
}

// The samples as an R16_UNORM image, read exactly with texelFetch.
void Renderer::Impl::uploadTerrainHeights(const Heightfield &field) {
  terrainHeightImage =
      createImage(VK_FORMAT_R16_UNORM, {field.width, field.height, 1},
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT);
  const VkDeviceSize bytes = field.samples.size() * sizeof(uint16_t);
  Buffer staging = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                    VMA_ALLOCATION_CREATE_MAPPED_BIT);
  std::memcpy(staging.mapped, field.samples.data(), bytes);
  check(vmaFlushAllocation(allocator, staging.allocation, 0, VK_WHOLE_SIZE),
        "vmaFlushAllocation");
  submitNow([&](VkCommandBuffer cmd) {
    transitionImage(cmd, {terrainHeightImage.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT});
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {field.width, field.height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, terrainHeightImage.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transitionImage(cmd, {terrainHeightImage.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                              VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
  });
  destroyBuffer(staging);
  writeTexture(terrain.heightTexture, terrainHeightImage.view, nearestSampler);
}

// Must match the push constants in terrain_normals.comp.
struct TerrainNormalConstants {
  uint32_t heightTexture;
  uint32_t width;
  uint32_t height;
  float spacing;
  float heightScale;
};

// The normal map: level 0 from the heights (terrain_normals.comp), the
// other levels blitted down from it, so distant ground samples filtered
// normals.
void Renderer::Impl::buildTerrainNormals(const Heightfield &field) {
  constexpr VkFormat kFormat = VK_FORMAT_R16G16_SNORM;
  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(device.physical_device, kFormat, &properties);
  constexpr VkFormatFeatureFlags kNeeded =
      VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
      VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if ((properties.optimalTilingFeatures & kNeeded) != kNeeded) {
    throw std::runtime_error("GPU can't build the terrain normal map (RG16_SNORM)");
  }
  const auto mipLevels =
      static_cast<uint32_t>(std::bit_width(std::max(field.width, field.height)));
  terrainNormalImage = createImage(
      kFormat, {field.width, field.height, 1},
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);

  VkDevice dev = device.device;
  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = terrainNormalImage.image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = kFormat;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView level0 = VK_NULL_HANDLE;
  check(vkCreateImageView(dev, &viewInfo, nullptr, &level0), "vkCreateImageView");

  const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                             VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
  setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  setLayoutInfo.bindingCount = 1;
  setLayoutInfo.pBindings = &binding;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  check(vkCreateDescriptorSetLayout(dev, &setLayoutInfo, nullptr, &setLayout),
        "vkCreateDescriptorSetLayout");
  const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &poolSize;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  check(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = pool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &setLayout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  check(vkAllocateDescriptorSets(dev, &allocInfo, &set), "vkAllocateDescriptorSets");
  const VkDescriptorImageInfo storage{VK_NULL_HANDLE, level0, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  write.pImageInfo = &storage;
  vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

  // Set 1 is the bindless table the heights are read from.
  const VkDescriptorSetLayout setLayouts[] = {setLayout, materialSetLayout};
  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                      sizeof(TerrainNormalConstants)};
  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.setLayoutCount = 2;
  layoutInfo.pSetLayouts = setLayouts;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &pushRange;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  check(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
  VkPipeline pipeline = createComputePipeline(dev, "terrain_normals.comp", layout);

  const TerrainNormalConstants constants{terrain.heightTexture, field.width, field.height,
                                         field.spacing, field.heightScale};
  submitNow([&](VkCommandBuffer cmd) {
    transitionImage(cmd, {terrainNormalImage.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_NONE, 0,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    const VkDescriptorSet sets[] = {set, materialSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, sets, 0,
                            nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                       &constants);
    vkCmdDispatch(cmd, (field.width + 7) / 8, (field.height + 7) / 8, 1);

    // Each level is blitted (box-filtered) from the one above it.
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.image = terrainNormalImage.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    auto transition = [&](uint32_t level, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                          VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
      barrier.subresourceRange.baseMipLevel = level;
      barrier.oldLayout = from;
      barrier.newLayout = to;
      barrier.srcStageMask = srcStage;
      barrier.srcAccessMask = srcAccess;
      barrier.dstStageMask = dstStage;
      barrier.dstAccessMask = dstAccess;
      vkCmdPipelineBarrier2(cmd, &dependency);
    };
    transition(0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    int32_t width = static_cast<int32_t>(field.width);
    int32_t height = static_cast<int32_t>(field.height);
    for (uint32_t level = 1; level < mipLevels; ++level) {
      transition(level, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_BLIT_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
      blit.srcOffsets[1] = {width, height, 1};
      width = std::max(width / 2, 1);
      height = std::max(height / 2, 1);
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
      blit.dstOffsets[1] = {width, height, 1};
      vkCmdBlitImage(cmd, terrainNormalImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     terrainNormalImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                     &blit, VK_FILTER_LINEAR);
      transition(level, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    barrier.subresourceRange.levelCount = mipLevels;
    transition(0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  });

  vkDestroyPipeline(dev, pipeline, nullptr);
  vkDestroyPipelineLayout(dev, layout, nullptr);
  vkDestroyDescriptorPool(dev, pool, nullptr);
  vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
  vkDestroyImageView(dev, level0, nullptr);
  writeTexture(terrain.normalTexture, terrainNormalImage.view, terrainSampler);
}

void Renderer::setTerrain(const Heightfield &field) {
  impl_->giReset = true;
  impl_->terrainShadowValid = false;
  const std::vector<HeightPyramidLevel> levels = buildHeightPyramid(field);
  // Levels >= 1 go to the GPU (level 0 is the samples themselves).
  if (levels.size() - 1 >= kMaxHeightPyramidLevels) {
    throw std::runtime_error("heightfield too large for the height pyramid");
  }
  std::vector<uint32_t> pyramid;
  GpuTerrain &terrain = impl_->terrain;
  for (size_t k = 1; k < levels.size(); ++k) {
    terrain.pyramidOffsets[k] = static_cast<uint32_t>(pyramid.size());
    pyramid.insert(pyramid.end(), levels[k].minMax.begin(), levels[k].minMax.end());
  }

  check(vkDeviceWaitIdle(impl_->device.device), "vkDeviceWaitIdle");
  if (impl_->terrainPyramid.buffer != VK_NULL_HANDLE) {
    impl_->destroyImage(impl_->terrainHeightImage);
    impl_->destroyImage(impl_->terrainNormalImage);
    impl_->destroyBuffer(impl_->terrainPyramid);
  } else {
    // Two bindless slots, kept for every later terrain. The table owns no
    // image there (destroying an empty Image is a no-op).
    if (impl_->textures.size() + 2 > kMaxTextures) {
      throw std::runtime_error("texture table is full");
    }
    terrain.heightTexture = static_cast<uint32_t>(impl_->textures.size());
    terrain.normalTexture = terrain.heightTexture + 1;
    impl_->textures.resize(impl_->textures.size() + 2);
  }
  impl_->uploadTerrainHeights(field);
  impl_->buildTerrainNormals(field);
  impl_->terrainPyramid =
      impl_->uploadStorage(pyramid.data(), pyramid.size() * sizeof(uint32_t));

  impl_->heightfield = field;
  if (!field.samples.empty()) {
    impl_->shadowCasterTop =
        field.minHeight +
        static_cast<double>(field.heightScale) *
            *std::max_element(field.samples.begin(), field.samples.end()) +
        kTallestCaster;
  }
  terrain.pyramid = impl_->terrainPyramid.address;
  terrain.pyramidLevels = static_cast<uint32_t>(levels.size() - 1);
  terrain.width = field.width;
  terrain.height = field.height;
  terrain.spacing = field.spacing;
  terrain.minHeight = field.minHeight;
  terrain.heightScale = field.heightScale;
  terrain.lodRange0 = kTerrainLodRange0;
  terrain.rootLevel = kTerrainRootLevel;
}

uint32_t Renderer::drawnTerrainNodeCount() const { return impl_->drawnTerrainNodes; }

// Walks the quadtree from the root down, one dispatch per level, into the
// terrain draw list. Runs after resetDrawCounters (it counts drawn and
// dropped nodes there).
void Renderer::Impl::selectTerrain(VkCommandBuffer cmd, const FrameData &frame) {
  if (terrainPyramid.buffer == VK_NULL_HANDLE) {
    return;
  }
  // The root's dispatch is a single workgroup; every other level's grows as
  // the level above adds nodes to its list.
  TerrainCounters initial{};
  for (auto &dispatch : initial.dispatch) {
    dispatch[1] = dispatch[2] = 1;
  }
  initial.dispatch[kTerrainRootLevel][0] = 1;
  initial.drawTasks[1] = initial.drawTasks[2] = 1;
  vkCmdUpdateBuffer(cmd, terrainCounters.buffer, 0, sizeof(initial), &initial);

  constexpr VkAccessFlags2 kSelectAccess = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                kSelectAccess);

  TerrainSelectConstants constants;
  constants.sceneBuffer = frame.sceneBuffer.address;
  constants.counters = terrainCounters.address;
  constants.lists = terrainLists.address;
  constants.drawCounters = drawCounters.address;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, terrainSelectPipeline);
  for (uint32_t level = kTerrainRootLevel + 1; level-- > 0;) {
    constants.level = level;
    vkCmdPushConstants(cmd, terrainSelectLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(constants), &constants);
    vkCmdDispatchIndirect(cmd, terrainCounters.buffer,
                          offsetof(TerrainCounters, dispatch) +
                              level * sizeof(TerrainCounters::dispatch[0]));
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT,
                  kSelectAccess);
  }
}

// In the early scene pass, before the clusters, so hills reach the depth
// pyramid and occlude what's behind them.
void Renderer::Impl::drawTerrain(VkCommandBuffer cmd) {
  if (terrainPyramid.buffer == VK_NULL_HANDLE) {
    return;
  }
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipeline);
  vkCmdSetCullMode(cmd, VK_CULL_MODE_BACK_BIT);
  dispatch.cmdDrawMeshTasksIndirectEXT(cmd, terrainCounters.buffer,
                                       offsetof(TerrainCounters, drawTasks), 1,
                                       sizeof(TerrainCounters::drawTasks));
}

} // namespace meridian
