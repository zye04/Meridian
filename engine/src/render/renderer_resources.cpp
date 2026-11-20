#include "renderer_impl.h"

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
  return as;
}

void Renderer::Impl::destroyAccelerationStructure(AccelerationStructure &as) {
  dispatch.destroyAccelerationStructureKHR(as.handle, nullptr);
  destroyBuffer(as.buffer);
  as = {};
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

Image Renderer::Impl::createScreenImage(VkFormat format,
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
  impl_->terrain.layers[0] = toGpu(material.soil);
  impl_->terrain.layers[1] = toGpu(material.needles);
  impl_->terrain.layers[2] = toGpu(material.rock);
  impl_->terrain.snowLine = material.snowLine;
  impl_->terrain.enabled = 1;
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
  std::vector<ClusterInfo> clusterInfos;
  std::vector<uint32_t> clusterTriangles;
  if (clustered) {
    clusterInfos.reserve(lod.clusters.size());
    for (const Cluster &c : lod.clusters) {
      ClusterInfo info;
      info.boundsCenter = c.boundsCenter;
      info.boundsRadius = c.boundsRadius;
      info.selfCenter = c.self.center;
      info.selfRadius = c.self.radius;
      info.selfError = c.self.error;
      info.refinedCenter = c.refined.center;
      info.refinedRadius = c.refined.radius;
      info.refinedError = c.refined.error;
      info.firstVertex = impl_->clusterVertexCount + c.firstVertex;
      info.firstTriangle = impl_->clusterTriangleCount + c.firstTriangle;
      info.vertexCount = c.vertexCount;
      info.triangleCount = c.triangleCount;
      clusterInfos.push_back(info);
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

  if (impl_->vertexCount + vertexCount > kVertexPoolCapacity ||
      impl_->indexCount + indexCount > kIndexPoolCapacity ||
      impl_->clusterCount + clusterCount > kMaxClusters ||
      impl_->clusterVertexCount + clusterVertexCount > kClusterVertexCapacity ||
      impl_->clusterTriangleCount + clusterTriangleCount > kClusterTriangleCapacity ||
      impl_->meshes.size() == kMaxMeshes) {
    throw std::runtime_error("geometry pools are full");
  }

  GpuMesh mesh;
  mesh.firstIndex = impl_->indexCount;
  mesh.indexCount = indexCount;
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
  // Staging layout: vertices, indices, clusters, cluster vertices, cluster
  // triangles.
  const VkDeviceSize stagingOffsets[] = {
      0, vertexBytes, vertexBytes + indexBytes,
      vertexBytes + indexBytes + clusterBytes,
      vertexBytes + indexBytes + clusterBytes + clusterVertexBytes};
  Buffer staging = impl_->createBuffer(
      stagingOffsets[4] + clusterTriangleBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
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
  }
  check(vmaFlushAllocation(impl_->allocator, staging.allocation, 0,
                           VK_WHOLE_SIZE),
        "vmaFlushAllocation");

  VkAccelerationStructureGeometryKHR geometry{};
  geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  // Alpha-masked geometry must reach the shader's alpha test.
  geometry.flags = mesh.alphaMode == AlphaMode::Mask ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
  auto &triangles = geometry.geometry.triangles;
  triangles.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  triangles.vertexData.deviceAddress = impl_->vertexPool.address + vertexPoolOffset;
  triangles.vertexStride = sizeof(Vertex);
  triangles.maxVertex = vertexCount - 1;
  triangles.indexType = VK_INDEX_TYPE_UINT32;
  triangles.indexData.deviceAddress = impl_->indexPool.address + indexPoolOffset;

  VkAccelerationStructureBuildGeometryInfoKHR build{};
  build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geometry;

  const uint32_t triangleCount = indexCount / 3;
  VkAccelerationStructureBuildSizesInfoKHR sizes{};
  sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  impl_->dispatch.getAccelerationStructureBuildSizesKHR(
      VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &triangleCount,
      &sizes);

  mesh.blas = impl_->createAccelerationStructure(
      VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
      sizes.accelerationStructureSize);
  Buffer scratch = impl_->createBuffer(
      sizes.buildScratchSize,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      0, impl_->scratchAlignment);
  build.dstAccelerationStructure = mesh.blas.handle;
  build.scratchData.deviceAddress = scratch.address;

  const auto meshIndex = static_cast<uint32_t>(impl_->meshes.size());
  impl_->submitNow([&](VkCommandBuffer cmd) {
    VkBufferCopy vertexCopy{0, vertexPoolOffset, vertexBytes};
    vkCmdCopyBuffer(cmd, staging.buffer, impl_->vertexPool.buffer, 1, &vertexCopy);
    VkBufferCopy indexCopy{vertexBytes, indexPoolOffset, indexBytes};
    vkCmdCopyBuffer(cmd, staging.buffer, impl_->indexPool.buffer, 1, &indexCopy);
    if (clusterCount > 0) {
      const VkBufferCopy clusterCopy{stagingOffsets[2],
                                     firstCluster * sizeof(ClusterInfo), clusterBytes};
      vkCmdCopyBuffer(cmd, staging.buffer, impl_->clusterPool.buffer, 1, &clusterCopy);
      const VkBufferCopy vertexRefCopy{
          stagingOffsets[3], firstClusterVertex * sizeof(uint32_t), clusterVertexBytes};
      vkCmdCopyBuffer(cmd, staging.buffer, impl_->clusterVertexPool.buffer, 1,
                      &vertexRefCopy);
      const VkBufferCopy triangleCopy{stagingOffsets[4],
                                      firstClusterTriangle * sizeof(uint32_t),
                                      clusterTriangleBytes};
      vkCmdCopyBuffer(cmd, staging.buffer, impl_->clusterTrianglePool.buffer, 1,
                      &triangleCopy);
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

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = triangleCount;
    const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
    {
      DriverLeakScope driverLeak;
      impl_->dispatch.cmdBuildAccelerationStructuresKHR(cmd, 1, &build,
                                                        &ranges);
    }

    // Later TLAS builds reference this BLAS.
    VkMemoryBarrier2 built{};
    built.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    built.srcStageMask =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    built.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    built.dstStageMask =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    built.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    dependency.pMemoryBarriers = &built;
    vkCmdPipelineBarrier2(cmd, &dependency);
  });

  impl_->destroyBuffer(scratch);
  impl_->destroyBuffer(staging);

  impl_->meshes.push_back(mesh);
  return {meshIndex};
}

uint32_t Renderer::instanceCount() const {
  return static_cast<uint32_t>(impl_->instances.size());
}

InstanceId Renderer::addInstance(MeshId mesh, const Mat4 &transform) {
  if (impl_->instances.size() == kMaxInstances) {
    throw std::runtime_error("instance table is full");
  }
  InstanceData instance;
  instance.transform = transform;
  instance.meshIndex = mesh.index;
  impl_->instances.push_back(instance);
  impl_->instancesDirty = true;
  return {static_cast<uint32_t>(impl_->instances.size() - 1)};
}

} // namespace meridian
