#include "renderer_impl.h"

#include <meridian/placement.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace meridian {

Buffer Renderer::Impl::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                    VmaAllocationCreateFlags flags,
                                    VkDeviceSize alignment) {
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usage;

  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
  allocInfo.flags = flags;

  Buffer buffer;
  VmaAllocationInfo info{};
  check(vmaCreateBufferWithAlignment(allocator, &bufferInfo, &allocInfo,
                                     alignment, &buffer.buffer,
                                     &buffer.allocation, &info),
        "vmaCreateBufferWithAlignment");
  buffer.mapped = info.pMappedData;
  buffer.size = size;
  if (flags == 0) {
    // Past the device-local budget VMA silently falls back to system memory,
    // which every later GPU read then pays for over PCIe.
    VkMemoryPropertyFlags properties = 0;
    vmaGetMemoryTypeProperties(allocator, info.memoryType, &properties);
    if (!(properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      std::fprintf(stderr, "warning: a %.0f MiB GPU buffer is outside device memory\n",
                   static_cast<double>(size) / (1 << 20));
    }
  }

  if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    VkBufferDeviceAddressInfo addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addressInfo.buffer = buffer.buffer;
    buffer.address = vkGetBufferDeviceAddress(device.device, &addressInfo);
  }
  return buffer;
}

void Renderer::Impl::destroyBuffer(Buffer &buffer) {
  vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
  buffer = {};
}

Buffer Renderer::Impl::grownPool(const Buffer &pool, VkDeviceSize bytes,
                                 VkBufferUsageFlags usage) {
  if (bytes <= pool.size) {
    return {};
  }
  return createBuffer(std::min(std::max(bytes, pool.size + pool.size / 2), maxBufferSize),
                      usage, 0);
}

Renderer::Impl::MemoryUse Renderer::Impl::memoryUse() const {
  VmaTotalStatistics stats;
  vmaCalculateStatistics(allocator, &stats);
  const VkPhysicalDeviceMemoryProperties *properties = nullptr;
  vmaGetMemoryProperties(allocator, &properties);
  MemoryUse use;
  use.all = stats.total.statistics.blockBytes;
  for (uint32_t h = 0; h < properties->memoryHeapCount; ++h) {
    if (properties->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
      use.deviceLocal += stats.memoryHeap[h].statistics.blockBytes;
    }
  }
  return use;
}

void Renderer::Impl::trimPools() {
  struct Trim {
    Buffer &pool;
    VkDeviceSize used;
    VkBufferUsageFlags usage;
  };
  Trim trims[] = {
      {vertexPool, VkDeviceSize{vertexCount} * sizeof(Vertex), kVertexPoolUsage},
      {indexPool, VkDeviceSize{indexCount} * sizeof(uint32_t), kIndexPoolUsage},
      {clusterPool, VkDeviceSize{clusterCount} * sizeof(ClusterInfo), kPoolUsage},
      {clusterVertexPool, VkDeviceSize{clusterVertexCount} * sizeof(uint32_t), kPoolUsage},
      {clusterTrianglePool, VkDeviceSize{clusterTriangleCount} * sizeof(uint32_t), kPoolUsage},
      {clusterNodePool, VkDeviceSize{clusterNodeCount} * sizeof(ClusterNodeInfo), kPoolUsage},
      {clusterLevelPool, VkDeviceSize{clusterLevelCount} * sizeof(ClusterLevelGpu), kPoolUsage},
      {voxelBrickPool, VkDeviceSize{voxelBrickCount} * sizeof(GpuVoxelBrick), kPoolUsage},
      {voxelPool, VkDeviceSize{voxelCount} * sizeof(PackedVoxel), kPoolUsage},
      {voxelAxisPool, VkDeviceSize{voxelCount} * sizeof(uint32_t), kPoolUsage},
  };
  // Frames in flight may still read the pools, and the copies read them.
  check(vkDeviceWaitIdle(device.device), "vkDeviceWaitIdle");
  // One pool at a time, the old one freed before the next is allocated: all
  // the copies at once exceeded device memory and VMA placed them in system
  // memory (2.2 GB of voxel and cluster pools, forest edge 25 -> 42 ms).
  for (Trim &t : trims) {
    const VkDeviceSize bytes = std::max(t.used, kMinPoolBytes);
    if (bytes >= t.pool.size) {
      continue;
    }
    Buffer trimmed = createBuffer(bytes, t.usage, 0);
    submitNow([&](VkCommandBuffer cmd) {
      if (t.used > 0) {
        const VkBufferCopy copy{0, 0, t.used};
        vkCmdCopyBuffer(cmd, t.pool.buffer, trimmed.buffer, 1, &copy);
      }
      VkMemoryBarrier2 copied{};
      copied.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
      copied.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      copied.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      copied.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      copied.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
      VkDependencyInfo dependency{};
      dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      dependency.memoryBarrierCount = 1;
      dependency.pMemoryBarriers = &copied;
      vkCmdPipelineBarrier2(cmd, &dependency);
    });
    destroyBuffer(t.pool);
    t.pool = trimmed;
  }
  writeVoxelPoolInfo();
}

AccelerationStructure
Renderer::Impl::createAccelerationStructure(VkAccelerationStructureTypeKHR type,
                                            VkDeviceSize size) {
  AccelerationStructure as;
  as.buffer = createBuffer(size,
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           0);

  VkAccelerationStructureCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
  info.buffer = as.buffer.buffer;
  info.size = size;
  info.type = type;
  check(dispatch.createAccelerationStructureKHR(&info, nullptr, &as.handle),
        "vkCreateAccelerationStructureKHR");

  VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
  addressInfo.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
  addressInfo.accelerationStructure = as.handle;
  as.address = dispatch.getAccelerationStructureDeviceAddressKHR(&addressInfo);
  as.size = size;
  return as;
}

void Renderer::Impl::destroyAccelerationStructure(AccelerationStructure &as) {
  dispatch.destroyAccelerationStructureKHR(as.handle, nullptr);
  destroyBuffer(as.buffer);
  as = {};
}

AccelerationStructure Renderer::Impl::buildBlas(std::span<const uint32_t> meshIndices,
                                                VkDeviceSize *builtBytes) {
  std::vector<VkAccelerationStructureGeometryKHR> geometries;
  std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
  std::vector<uint32_t> triangleCounts;
  for (uint32_t index : meshIndices) {
    const GpuMesh &mesh = meshes.at(index);
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    // Alpha-masked geometry must reach the shader's alpha test.
    geometry.flags = mesh.alphaMode == AlphaMode::Mask ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto &triangles = geometry.geometry.triangles;
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress =
        vertexPool.address + VkDeviceSize(mesh.vertexOffset) * sizeof(Vertex);
    triangles.vertexStride = sizeof(Vertex);
    triangles.maxVertex = mesh.vertexCount - 1;
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress =
        indexPool.address + VkDeviceSize{mesh.firstIndex} * sizeof(uint32_t);
    geometries.push_back(geometry);
    triangleCounts.push_back(mesh.indexCount / 3);
    ranges.push_back({mesh.indexCount / 3, 0, 0, 0});
  }

  VkAccelerationStructureBuildGeometryInfoKHR build{};
  build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = static_cast<uint32_t>(geometries.size());
  build.pGeometries = geometries.data();

  VkAccelerationStructureBuildSizesInfoKHR sizes{};
  sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  dispatch.getAccelerationStructureBuildSizesKHR(VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                 &build, triangleCounts.data(), &sizes);
  AccelerationStructure built = createAccelerationStructure(
      VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize);
  Buffer scratch = createBuffer(
      sizes.buildScratchSize,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, 0,
      scratchAlignment);
  build.dstAccelerationStructure = built.handle;
  build.scratchData.deviceAddress = scratch.address;

  VkQueryPoolCreateInfo queryInfo{};
  queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
  queryInfo.queryCount = 1;
  VkQueryPool query = VK_NULL_HANDLE;
  check(vkCreateQueryPool(device.device, &queryInfo, nullptr, &query), "vkCreateQueryPool");

  VkMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  // Property queries and copies run in the build stage too (without
  // VK_KHR_ray_tracing_maintenance1).
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  VkDependencyInfo dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  submitNow([&](VkCommandBuffer cmd) {
    vkCmdResetQueryPool(cmd, query, 0, 1);
    const VkAccelerationStructureBuildRangeInfoKHR *rangeInfos = ranges.data();
    {
      DriverLeakScope driverLeak;
      dispatch.cmdBuildAccelerationStructuresKHR(cmd, 1, &build, &rangeInfos);
    }
    vkCmdPipelineBarrier2(cmd, &dependency);
    dispatch.cmdWriteAccelerationStructuresPropertiesKHR(
        cmd, 1, &built.handle, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, query,
        0);
  });
  VkDeviceSize compactedBytes = 0;
  check(vkGetQueryPoolResults(device.device, query, 0, 1, sizeof(compactedBytes),
                              &compactedBytes, sizeof(compactedBytes),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
        "vkGetQueryPoolResults");
  vkDestroyQueryPool(device.device, query, nullptr);
  destroyBuffer(scratch);

  AccelerationStructure compacted =
      createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, compactedBytes);
  submitNow([&](VkCommandBuffer cmd) {
    VkCopyAccelerationStructureInfoKHR copy{};
    copy.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
    copy.src = built.handle;
    copy.dst = compacted.handle;
    copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
    dispatch.cmdCopyAccelerationStructureKHR(cmd, &copy);
    // Later TLAS builds and ray queries reference it.
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    vkCmdPipelineBarrier2(cmd, &dependency);
  });
  if (builtBytes) {
    *builtBytes = sizes.accelerationStructureSize;
  }
  destroyAccelerationStructure(built);
  return compacted;
}

// A GPU-only 2D image, or 3D when extent.depth > 1.
Image Renderer::Impl::createImage(VkFormat format, VkExtent3D extent,
                                  VkImageUsageFlags usage,
                                  VkImageAspectFlags aspect,
                                  uint32_t mipLevels) {
  const bool is3d = extent.depth > 1;
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  imageInfo.format = format;
  imageInfo.extent = extent;
  imageInfo.mipLevels = mipLevels;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = usage;

  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
  // Render targets get their own allocation, which drivers handle best;
  // sampled textures share larger blocks.
  if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
  }

  Image image;
  check(vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image,
                       &image.allocation, nullptr),
        "vmaCreateImage");

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = image.image;
  viewInfo.viewType = is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = format;
  viewInfo.subresourceRange.aspectMask = aspect;
  viewInfo.subresourceRange.levelCount = mipLevels;
  viewInfo.subresourceRange.layerCount = 1;
  check(vkCreateImageView(device.device, &viewInfo, nullptr, &image.view),
        "vkCreateImageView");
  return image;
}

Image Renderer::Impl::createRenderImage(VkFormat format,
                                        VkImageUsageFlags usage,
                                        VkImageAspectFlags aspect) {
  return createImage(format, {renderExtent.width, renderExtent.height, 1}, usage,
                     aspect);
}

Image Renderer::Impl::createDisplayImage(VkFormat format,
                                         VkImageUsageFlags usage,
                                         VkImageAspectFlags aspect) {
  return createImage(format,
                     {swapchain.extent.width, swapchain.extent.height, 1},
                     usage, aspect);
}

void Renderer::Impl::destroyImage(Image &image) {
  vkDestroyImageView(device.device, image.view, nullptr);
  vmaDestroyImage(allocator, image.image, image.allocation);
  image = {};
}

TextureId Renderer::uploadTexture(const TextureData &texture) {
  if (impl_->textures.size() == kMaxTextures) {
    throw std::runtime_error("texture table is full");
  }
  const VkFormat format = texture.colorSpace == ColorSpace::Srgb
                              ? VK_FORMAT_BC7_SRGB_BLOCK
                              : VK_FORMAT_BC7_UNORM_BLOCK;
  const auto mipCount = static_cast<uint32_t>(texture.mips.size());
  Image image = impl_->createImage(
      format, {texture.width, texture.height, 1},
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT, mipCount);

  VkDeviceSize totalBytes = 0;
  for (const auto &mip : texture.mips) {
    totalBytes += mip.size();
  }
  Buffer staging = impl_->createBuffer(
      totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);

  std::vector<VkBufferImageCopy> regions(mipCount);
  VkDeviceSize offset = 0;
  for (uint32_t level = 0; level < mipCount; ++level) {
    std::memcpy(static_cast<char *>(staging.mapped) + offset,
                texture.mips[level].data(), texture.mips[level].size());
    VkBufferImageCopy &region = regions[level];
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    region.imageExtent = {std::max(1u, texture.width >> level),
                          std::max(1u, texture.height >> level), 1};
    offset += texture.mips[level].size();
  }
  check(vmaFlushAllocation(impl_->allocator, staging.allocation, 0,
                           VK_WHOLE_SIZE),
        "vmaFlushAllocation");

  impl_->submitNow([&](VkCommandBuffer cmd) {
    transitionImage(cmd, {image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_NONE, 0,
                          VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT});
    vkCmdCopyBufferToImage(cmd, staging.buffer, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mipCount,
                           regions.data());
    transitionImage(cmd, {image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
  });
  impl_->destroyBuffer(staging);

  const auto index = static_cast<uint32_t>(impl_->textures.size());
  VkDescriptorImageInfo imageInfo{impl_->textureSampler, image.view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = impl_->materialSet;
  write.dstBinding = 1;
  write.dstArrayElement = index;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &imageInfo;
  vkUpdateDescriptorSets(impl_->device.device, 1, &write, 0, nullptr);

  impl_->textures.push_back(image);
  return {index};
}

void Renderer::setTerrainMaterial(const TerrainMaterial &material) {
  size_t total = 0;
  for (const std::vector<TerrainLayer> &variants : material.layers) {
    if (variants.empty()) {
      throw std::runtime_error("terrain material: every role needs a layer");
    }
    total += variants.size();
  }
  if (total > static_cast<size_t>(kMaxTerrainLayers)) {
    throw std::runtime_error("terrain material: more than kMaxTerrainLayers layers");
  }
  GpuTerrain &terrain = impl_->terrain;
  uint32_t count = 0;
  for (int role = 0; role < kTerrainRoles; ++role) {
    const std::vector<TerrainLayer> &variants = material.layers[role];
    terrain.roles[role] = count | static_cast<uint32_t>(variants.size()) << 8;
    for (const TerrainLayer &layer : variants) {
      terrain.layers[count++] = toGpu(layer);
    }
  }
  terrain.layerCount = count;
  // setPlacement bakes the floor for this layer id.
  impl_->forestLayerId = material.forestLayerId;
  if (material.forestLayerId == 0) {
    terrain.forestEnabled = 0;
  }
  terrain.snowLine = material.snowLine;
  terrain.enabled = 1;
}

MaterialId Renderer::createMaterial(const MaterialData &material) {
  if (impl_->materialCount == kMaxMaterials) {
    throw std::runtime_error("material table is full");
  }
  const uint32_t index = impl_->materialCount++;
  impl_->materials.push_back(material);
  const GpuMaterial gpu = toGpu(material);
  impl_->submitNow([&](VkCommandBuffer cmd) {
    vkCmdUpdateBuffer(cmd, impl_->materialBuffer.buffer,
                      index * sizeof(GpuMaterial), sizeof(gpu), &gpu);
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
  });
  return {index};
}

MeshId Renderer::uploadMesh(const MeshData &data, MaterialId material) {
  const auto vertexCount = static_cast<uint32_t>(data.vertices.size());
  const auto indexCount = static_cast<uint32_t>(data.indices.size());
  const MaterialData &materialData = impl_->materials[material.index];

  // Opaque and masked meshes are drawn as clusters; blended ones whole.
  ClusterLod builtClusters;
  const bool clustered = materialData.alphaMode != AlphaMode::Blend;
  if (clustered && data.clusters.clusters.empty()) {
    builtClusters = buildClusterLod(data);
  }
  const ClusterLod &lod = data.clusters.clusters.empty() ? builtClusters : data.clusters;
  const auto meshIndex = static_cast<uint32_t>(impl_->meshes.size());
  std::vector<ClusterInfo> clusterInfos;
  std::vector<ClusterNodeInfo> nodeInfos;
  std::vector<ClusterLevelGpu> levelInfos;
  uint32_t traversalDepths = clustered ? 1 : 0;
  std::vector<uint32_t> clusterTriangles;
  std::vector<GpuVoxelBrick> voxelBricks;
  if (clustered) {
    // Triangle clusters whose group voxel clusters replace (the voxel LOD
    // A/B keeps drawing them instead).
    std::vector<uint8_t> voxelGroup;
    for (const Cluster &c : lod.clusters) {
      if (c.brickCount > 0 && c.refinedGroup >= 0) {
        voxelGroup.resize(std::max<size_t>(voxelGroup.size(), size_t(c.refinedGroup) + 1), 0);
        voxelGroup[size_t(c.refinedGroup)] = 1;
      }
    }
    const std::vector<LodThresholds> thresholds = clusterThresholds(lod, 1.0f);
    clusterInfos.reserve(lod.clusters.size());
    for (size_t i = 0; i < lod.clusters.size(); ++i) {
      const Cluster &c = lod.clusters[i];
      ClusterInfo info;
      info.boundsCenter = c.boundsCenter;
      info.boundsRadius = c.boundsRadius;
      info.selfCenter = c.self.center;
      info.selfRadius = c.self.radius;
      info.selfError = c.self.error;
      info.refinedCenter = c.refined.center;
      info.refinedRadius = c.refined.radius;
      info.refinedError = c.refined.error;
      if (c.brickCount > 0) {
        info.firstTriangle = impl_->voxelBrickCount + c.firstBrick;
        info.triangleCount = c.brickCount;
        info.voxelEdge = c.voxelEdge;
        info.firstVertex = data.voxelSeed ^ uint32_t(i);
        // 1: the refined side switches at the threshold itself (made from
        // triangles), else at kVoxelPixels times it.
        info.vertexCount = thresholds[i].refined < thresholds[i].self ? 1u : 0u;
      } else {
        info.firstVertex = impl_->clusterVertexCount + c.firstVertex;
        info.firstTriangle = impl_->clusterTriangleCount + c.firstTriangle;
        info.vertexCount = c.vertexCount;
        info.triangleCount = c.triangleCount;
        if (c.group < voxelGroup.size() && voxelGroup[c.group]) {
          info.voxelEdge = -1.0f;
        }
      }
      info.mesh = meshIndex;
      clusterInfos.push_back(info);
    }
    voxelBricks.reserve(lod.bricks.size());
    for (const VoxelBrickRef &b : lod.bricks) {
      voxelBricks.push_back({{b.origin[0], b.origin[1], b.origin[2]},
                             impl_->voxelCount + b.firstVoxel,
                             {uint32_t(b.occupied), uint32_t(b.occupied >> 32)}});
    }
    // Children and roots index the shared pools.
    for (const ClusterBvhNode &n : lod.nodes) {
      ClusterNodeInfo node;
      node.lodCenter = n.lod.center;
      node.lodRadius = n.lod.radius;
      node.lodError = n.lod.error;
      node.boundsCenter = n.boundsCenter;
      node.boundsRadius = n.boundsRadius;
      node.firstChild = n.firstChild + (n.leaf ? impl_->clusterCount : impl_->clusterNodeCount);
      node.childCount = n.childCount;
      node.leaf = n.leaf;
      nodeInfos.push_back(node);
    }
    for (const ClusterLevelInfo &l : lod.levels) {
      levelInfos.push_back({l.rootNode + impl_->clusterNodeCount, l.minError, l.maxError,
                            l.minGroupRadius});
    }
    // Roots stay at depth 0: they are nobody's child, and children come
    // after their parent.
    std::vector<uint32_t> depths(lod.nodes.size(), 0);
    for (size_t n = 0; n < lod.nodes.size(); ++n) {
      for (uint32_t i = 0; !lod.nodes[n].leaf && i < lod.nodes[n].childCount; ++i) {
        depths[lod.nodes[n].firstChild + i] = depths[n] + 1;
        traversalDepths = std::max(traversalDepths, depths[n] + 2);
      }
    }
    clusterTriangles.reserve(lod.triangles.size() / 3);
    for (size_t i = 0; i < lod.triangles.size(); i += 3) {
      clusterTriangles.push_back(uint32_t{lod.triangles[i]} |
                                 uint32_t{lod.triangles[i + 1]} << 8 |
                                 uint32_t{lod.triangles[i + 2]} << 16);
    }
  }
  const auto clusterCount = static_cast<uint32_t>(clusterInfos.size());
  const auto clusterVertexCount =
      clustered ? static_cast<uint32_t>(lod.vertices.size()) : 0u;
  const auto clusterTriangleCount = static_cast<uint32_t>(clusterTriangles.size());
  const auto nodeCount = static_cast<uint32_t>(nodeInfos.size());
  const auto levelCount = static_cast<uint32_t>(levelInfos.size());
  const auto brickCount = static_cast<uint32_t>(voxelBricks.size());
  const auto voxelCount = clustered ? static_cast<uint32_t>(lod.voxels.size()) : 0u;
  if (clustered && lod.voxelAxis.size() != lod.voxels.size()) {
    throw std::runtime_error("voxel axis depths missing (bakeModelAxisDepths)");
  }

  const auto total = [](uint32_t used, uint32_t added) { return uint64_t{used} + added; };
  if (total(impl_->vertexCount, vertexCount) > INT32_MAX ||
      total(impl_->indexCount, indexCount) > UINT32_MAX ||
      total(impl_->clusterVertexCount, clusterVertexCount) > UINT32_MAX ||
      total(impl_->clusterTriangleCount, clusterTriangleCount) > UINT32_MAX ||
      total(impl_->clusterNodeCount, nodeCount) > UINT32_MAX ||
      total(impl_->clusterLevelCount, levelCount) > UINT32_MAX ||
      total(impl_->voxelBrickCount, brickCount) > UINT32_MAX ||
      total(impl_->voxelCount, voxelCount) > UINT32_MAX) {
    throw std::runtime_error("geometry pool offsets exceed 32 bits");
  }
  // A voxel quad entry packs its pool brick above the 6-bit voxel index.
  if (total(impl_->voxelBrickCount, brickCount) > (uint64_t{1} << 26)) {
    throw std::runtime_error("more voxel bricks than a quad entry can index (2^26)");
  }
  if (total(impl_->clusterCount, clusterCount) > kMaxClusterIndex) {
    throw std::runtime_error("more clusters than work items can index (kMaxClusterIndex)");
  }
  if (traversalDepths > kBvhDepths) {
    throw std::runtime_error("cluster BVH deeper than the traversal's kBvhDepths");
  }
  if (impl_->meshes.size() == kMaxMeshes) {
    throw std::runtime_error("mesh table is full (kMaxMeshes)");
  }
  if (std::max({total(impl_->vertexCount, vertexCount) * sizeof(Vertex),
                total(impl_->indexCount, indexCount) * sizeof(uint32_t),
                total(impl_->clusterCount, clusterCount) * sizeof(ClusterInfo),
                total(impl_->clusterVertexCount, clusterVertexCount) * sizeof(uint32_t),
                total(impl_->clusterTriangleCount, clusterTriangleCount) * sizeof(uint32_t),
                total(impl_->clusterNodeCount, nodeCount) * sizeof(ClusterNodeInfo),
                total(impl_->clusterLevelCount, levelCount) * sizeof(ClusterLevelGpu),
                total(impl_->voxelBrickCount, brickCount) * sizeof(GpuVoxelBrick),
                total(impl_->voxelCount, voxelCount) * sizeof(PackedVoxel)}) >
      impl_->maxBufferSize) {
    throw std::runtime_error("a geometry pool would exceed the device's maxBufferSize");
  }

  GpuMesh mesh;
  mesh.firstIndex = impl_->indexCount;
  mesh.indexCount = indexCount;
  mesh.vertexCount = vertexCount;
  mesh.vertexOffset = static_cast<int32_t>(impl_->vertexCount);
  mesh.materialIndex = material.index;
  mesh.alphaMode = materialData.alphaMode;
  mesh.doubleSided = materialData.doubleSided;
  switch (mesh.alphaMode) {
  case AlphaMode::Opaque:
    mesh.drawBucket = mesh.doubleSided ? 1 : 0;
    break;
  case AlphaMode::Mask:
    mesh.drawBucket = mesh.doubleSided ? 3 : 2;
    break;
  case AlphaMode::Blend:
    mesh.drawBucket = kDrawBucketBlended;
    break;
  }
  impl_->vertexCount += vertexCount;
  impl_->indexCount += indexCount;
  const uint32_t firstCluster = impl_->clusterCount;
  const uint32_t firstClusterVertex = impl_->clusterVertexCount;
  const uint32_t firstClusterTriangle = impl_->clusterTriangleCount;
  impl_->clusterCount += clusterCount;
  impl_->clusterVertexCount += clusterVertexCount;
  impl_->clusterTriangleCount += clusterTriangleCount;
  const uint32_t firstNode = impl_->clusterNodeCount;
  const uint32_t firstLevel = impl_->clusterLevelCount;
  impl_->clusterNodeCount += nodeCount;
  impl_->clusterLevelCount += levelCount;
  const uint32_t firstBrick = impl_->voxelBrickCount;
  const uint32_t firstVoxel = impl_->voxelCount;
  impl_->voxelBrickCount += brickCount;
  impl_->voxelCount += voxelCount;
  impl_->traversalDepths = std::max(impl_->traversalDepths, traversalDepths);

  // Bounding sphere around the bounding box: cheap and good enough for
  // culling.
  Vec3 lo = data.vertices.front().position;
  Vec3 hi = lo;
  for (const Vertex &v : data.vertices) {
    lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y),
          std::min(lo.z, v.position.z)};
    hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y),
          std::max(hi.z, v.position.z)};
  }
  MeshInfo info;
  info.boundsCenter = (lo + hi) * 0.5f;
  info.halfExtent = (hi - lo) * 0.5f;
  for (const Vertex &v : data.vertices) {
    info.boundsRadius =
        std::max(info.boundsRadius, length(v.position - info.boundsCenter));
  }
  mesh.boundsCenter = info.boundsCenter;
  mesh.boundsRadius = info.boundsRadius;
  info.firstIndex = mesh.firstIndex;
  info.indexCount = mesh.indexCount;
  info.vertexOffset = mesh.vertexOffset;
  info.materialIndex = mesh.materialIndex;
  info.drawBucket = mesh.drawBucket;
  info.firstCluster = firstCluster;
  info.clusterCount = clusterCount;
  info.firstLevel = firstLevel;
  info.levelCount = levelCount;

  const VkDeviceSize vertexBytes = VkDeviceSize{vertexCount} * sizeof(Vertex);
  const VkDeviceSize indexBytes = VkDeviceSize{indexCount} * sizeof(uint32_t);
  const VkDeviceSize vertexPoolOffset =
      VkDeviceSize(mesh.vertexOffset) * sizeof(Vertex);
  const VkDeviceSize indexPoolOffset =
      VkDeviceSize{mesh.firstIndex} * sizeof(uint32_t);

  // GPU-only memory is fastest to read but the CPU can't write it, so the
  // data goes through a temporary CPU-visible staging buffer.
  const VkDeviceSize clusterBytes = VkDeviceSize{clusterCount} * sizeof(ClusterInfo);
  const VkDeviceSize clusterVertexBytes =
      VkDeviceSize{clusterVertexCount} * sizeof(uint32_t);
  const VkDeviceSize clusterTriangleBytes =
      VkDeviceSize{clusterTriangleCount} * sizeof(uint32_t);
  const VkDeviceSize nodeBytes = VkDeviceSize{nodeCount} * sizeof(ClusterNodeInfo);
  const VkDeviceSize levelBytes = VkDeviceSize{levelCount} * sizeof(ClusterLevelGpu);
  const VkDeviceSize brickBytes = VkDeviceSize{brickCount} * sizeof(GpuVoxelBrick);
  const VkDeviceSize voxelBytes = VkDeviceSize{voxelCount} * sizeof(PackedVoxel);
  const VkDeviceSize axisBytes = VkDeviceSize{voxelCount} * sizeof(uint32_t);
  // Staging layout: vertices, indices, clusters, cluster vertices, cluster
  // triangles, BVH nodes, levels, voxel bricks, voxels, axis depths.
  const VkDeviceSize stagingOffsets[] = {
      0,
      vertexBytes,
      vertexBytes + indexBytes,
      vertexBytes + indexBytes + clusterBytes,
      vertexBytes + indexBytes + clusterBytes + clusterVertexBytes,
      vertexBytes + indexBytes + clusterBytes + clusterVertexBytes + clusterTriangleBytes,
      vertexBytes + indexBytes + clusterBytes + clusterVertexBytes + clusterTriangleBytes +
          nodeBytes,
      vertexBytes + indexBytes + clusterBytes + clusterVertexBytes + clusterTriangleBytes +
          nodeBytes + levelBytes,
      vertexBytes + indexBytes + clusterBytes + clusterVertexBytes + clusterTriangleBytes +
          nodeBytes + levelBytes + brickBytes,
      vertexBytes + indexBytes + clusterBytes + clusterVertexBytes + clusterTriangleBytes +
          nodeBytes + levelBytes + brickBytes + voxelBytes};
  Buffer staging = impl_->createBuffer(
      stagingOffsets[9] + axisBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  auto *stagingBytes = static_cast<char *>(staging.mapped);
  std::memcpy(stagingBytes, data.vertices.data(), vertexBytes);
  std::memcpy(stagingBytes + stagingOffsets[1], data.indices.data(), indexBytes);
  if (clusterCount > 0) {
    std::memcpy(stagingBytes + stagingOffsets[2], clusterInfos.data(), clusterBytes);
    std::memcpy(stagingBytes + stagingOffsets[3], lod.vertices.data(),
                clusterVertexBytes);
    std::memcpy(stagingBytes + stagingOffsets[4], clusterTriangles.data(),
                clusterTriangleBytes);
    std::memcpy(stagingBytes + stagingOffsets[5], nodeInfos.data(), nodeBytes);
    std::memcpy(stagingBytes + stagingOffsets[6], levelInfos.data(), levelBytes);
    if (brickCount > 0) {
      std::memcpy(stagingBytes + stagingOffsets[7], voxelBricks.data(), brickBytes);
      std::memcpy(stagingBytes + stagingOffsets[8], lod.voxels.data(), voxelBytes);
      std::memcpy(stagingBytes + stagingOffsets[9], lod.voxelAxis.data(), axisBytes);
    }
  }
  check(vmaFlushAllocation(impl_->allocator, staging.allocation, 0,
                           VK_WHOLE_SIZE),
        "vmaFlushAllocation");

  // Grown pools are allocated before recording and swapped in only after
  // the submit, so a failure leaves every pool as it was.
  struct PoolUpload {
    Buffer &pool;
    VkDeviceSize offset;
    VkDeviceSize stagingOffset;
    VkDeviceSize bytes;
    VkBufferUsageFlags usage;
    Buffer grown{};
  };
  PoolUpload uploads[] = {
      {impl_->vertexPool, vertexPoolOffset, stagingOffsets[0], vertexBytes, kVertexPoolUsage},
      {impl_->indexPool, indexPoolOffset, stagingOffsets[1], indexBytes, kIndexPoolUsage},
      {impl_->clusterPool, firstCluster * sizeof(ClusterInfo), stagingOffsets[2], clusterBytes,
       kPoolUsage},
      {impl_->clusterVertexPool, firstClusterVertex * sizeof(uint32_t), stagingOffsets[3],
       clusterVertexBytes, kPoolUsage},
      {impl_->clusterTrianglePool, firstClusterTriangle * sizeof(uint32_t), stagingOffsets[4],
       clusterTriangleBytes, kPoolUsage},
      {impl_->clusterNodePool, firstNode * sizeof(ClusterNodeInfo), stagingOffsets[5], nodeBytes,
       kPoolUsage},
      {impl_->clusterLevelPool, firstLevel * sizeof(ClusterLevelGpu), stagingOffsets[6],
       levelBytes, kPoolUsage},
      {impl_->voxelBrickPool, firstBrick * sizeof(GpuVoxelBrick), stagingOffsets[7], brickBytes,
       kPoolUsage},
      {impl_->voxelPool, firstVoxel * sizeof(PackedVoxel), stagingOffsets[8], voxelBytes,
       kPoolUsage},
      {impl_->voxelAxisPool, firstVoxel * sizeof(uint32_t), stagingOffsets[9], axisBytes,
       kPoolUsage},
  };
  bool grows = false;
  for (PoolUpload &upload : uploads) {
    upload.grown = impl_->grownPool(upload.pool, upload.offset + upload.bytes, upload.usage);
    grows |= upload.grown.buffer != VK_NULL_HANDLE;
  }
  if (grows) {
    const Impl::MemoryUse use = impl_->memoryUse();
    impl_->loadPeak.deviceLocal = std::max(impl_->loadPeak.deviceLocal, use.deviceLocal);
    impl_->loadPeak.all = std::max(impl_->loadPeak.all, use.all);
  }

  impl_->submitNow([&](VkCommandBuffer cmd) {
    if (grows) {
      // Earlier uploads wrote the old pools by copies too.
      VkMemoryBarrier2 written{};
      written.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
      written.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      written.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      written.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      written.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
      VkDependencyInfo dependency{};
      dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      dependency.memoryBarrierCount = 1;
      dependency.pMemoryBarriers = &written;
      vkCmdPipelineBarrier2(cmd, &dependency);
    }
    for (const PoolUpload &upload : uploads) {
      if (upload.grown.buffer != VK_NULL_HANDLE && upload.offset > 0) {
        const VkBufferCopy copy{0, 0, upload.offset};
        vkCmdCopyBuffer(cmd, upload.pool.buffer, upload.grown.buffer, 1, &copy);
      }
      if (upload.bytes > 0) {
        const VkBufferCopy copy{upload.stagingOffset, upload.offset, upload.bytes};
        vkCmdCopyBuffer(cmd, staging.buffer,
                        upload.grown.buffer != VK_NULL_HANDLE ? upload.grown.buffer
                                                              : upload.pool.buffer,
                        1, &copy);
      }
    }
    vkCmdUpdateBuffer(cmd, impl_->meshTable.buffer, meshIndex * sizeof(MeshInfo),
                      sizeof(info), &info);

    VkMemoryBarrier2 copied{};
    copied.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    copied.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    copied.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    copied.dstStageMask =
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
        VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    copied.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                           VK_ACCESS_2_INDEX_READ_BIT |
                           VK_ACCESS_2_SHADER_READ_BIT;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &copied;
    vkCmdPipelineBarrier2(cmd, &dependency);
  });

  impl_->destroyBuffer(staging);
  if (grows) {
    // Frames still in flight may read the old pools.
    check(vkDeviceWaitIdle(impl_->device.device), "vkDeviceWaitIdle");
    for (PoolUpload &upload : uploads) {
      if (upload.grown.buffer != VK_NULL_HANDLE) {
        impl_->destroyBuffer(upload.pool);
        upload.pool = upload.grown;
      }
    }
    impl_->writeVoxelPoolInfo();
  }
  if (brickCount > 0 && impl_->voxelInstanceList.buffer == VK_NULL_HANDLE) {
    impl_->voxelInstanceList =
        impl_->createBuffer(2 * (VkDeviceSize{kMaxVoxelInstances} + kMaxVoxelQuads) * 8,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                            0);
  }

  impl_->meshes.push_back(mesh);
  impl_->meshFarVoxels.push_back({lod.farBricks, lod.farVoxels});
  return {meshIndex};
}

uint32_t Renderer::instanceCount() const {
  return static_cast<uint32_t>(impl_->instances.size());
}

InstanceId Renderer::addInstance(MeshId mesh, const Mat4 &transform) {
  if (impl_->instances.size() == kMaxInstances) {
    throw std::runtime_error("instance table is full");
  }
  GpuMesh &gpuMesh = impl_->meshes.at(mesh.index);
  if (gpuMesh.blas.handle == VK_NULL_HANDLE) {
    gpuMesh.blas = impl_->buildBlas(std::span(&mesh.index, 1));
  }
  InstanceData instance;
  instance.transform = transform;
  instance.meshIndex = mesh.index;
  impl_->instances.push_back(instance);
  impl_->instancesDirty = true;
  impl_->shadowStale = true;
  return {static_cast<uint32_t>(impl_->instances.size() - 1)};
}

void Renderer::Impl::writeVoxelPoolInfo() {
  GpuVoxelPools pools;
  pools.bricks = voxelBrickPool.address;
  pools.voxels = voxelPool.address;
  pools.axis = voxelAxisPool.address;
  std::memcpy(voxelPoolInfo.mapped, &pools, sizeof(pools));
  check(vmaFlushAllocation(allocator, voxelPoolInfo.allocation, 0, VK_WHOLE_SIZE),
        "vmaFlushAllocation");
}

} // namespace meridian
