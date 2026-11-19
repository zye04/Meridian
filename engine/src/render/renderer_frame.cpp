#include "renderer_impl.h"

#include <basisu_enc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace meridian {

namespace {

// x minus the nearest multiple of period: identity within half a period of
// the origin.
float wrapAround(float x, double period) {
  return static_cast<float>(x - period * std::round(x / period));
}

} // namespace

// Writes the CPU instance list to the GPU instance table when it changed.
// The table is shared by both frames in flight, so the copy waits for the
// previous frame's reads.
void Renderer::Impl::uploadInstances(VkCommandBuffer cmd, FrameData &frame) {
  if (!instancesDirty) {
    return;
  }
  const VkDeviceSize bytes = instances.size() * sizeof(InstanceData);
  std::memcpy(frame.instanceStaging.mapped, instances.data(), bytes);
  check(vmaFlushAllocation(allocator, frame.instanceStaging.allocation, 0, bytes),
        "vmaFlushAllocation");

  constexpr VkPipelineStageFlags2 kReaders =
      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  memoryBarrier(cmd, kReaders, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
  VkBufferCopy copy{0, 0, bytes};
  vkCmdCopyBuffer(cmd, frame.instanceStaging.buffer, instanceTable.buffer, 1,
                  &copy);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, kReaders,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  instancesDirty = false;
}

// Clears the DrawCounters before the terrain selection and both culling
// phases count into them. The counters, work items and draw lists (the
// terrain's too) are shared by both frames in flight, so this first waits
// for the previous frame's use of them.
void Renderer::Impl::resetDrawCounters(VkCommandBuffer cmd) {
  memoryBarrier(cmd,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                    VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
  DrawCounters initial{};
  for (auto &dispatch : initial.workDispatch) {
    dispatch[1] = dispatch[2] = 1;
  }
  for (auto &lists : initial.meshTasks) {
    for (auto &tasks : lists) {
      tasks[1] = tasks[2] = 1;
    }
  }
  initial.retestDispatch[1] = initial.retestDispatch[2] = 1;
  vkCmdUpdateBuffer(cmd, drawCounters.buffer, 0, sizeof(initial), &initial);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

// Culls on the GPU and fills one phase's cluster draw lists. Phase 0 takes
// last frame's visible instances (frustum test only); phase 1 tests all of
// them against the depth pyramid, adds the newly visible ones and records
// visibility for the next frame. In each phase the instance cull emits work
// items, which the cluster cull LOD-selects and frustum-culls.
void Renderer::Impl::cullInstances(VkCommandBuffer cmd, const FrameData &frame,
                                   uint32_t phase) {
  CullConstants constants;
  constants.sceneBuffer = frame.sceneBuffer.address;
  constants.workItems =
      workItems.address + VkDeviceSize{phase} * kMaxWorkItems * 2 * sizeof(uint32_t);
  constants.counters = drawCounters.address;
  constants.visibility = visibility.address;
  constants.phase = phase;
  constants.occlusion = occlusionCulling ? 1 : 0;
  constants.pyramidSize[0] = static_cast<float>(depthPyramidExtent.width);
  constants.pyramidSize[1] = static_cast<float>(depthPyramidExtent.height);
  constants.pyramidLevels = static_cast<uint32_t>(depthPyramidLevels.size());
  constants.previousPyramid = pyramidValid ? 1 : 0;
  constants.retest = retestClusters.address;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullLayout, 0, 1,
                          &cullSet, 0, nullptr);
  vkCmdPushConstants(cmd, cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(constants), &constants);
  vkCmdDispatch(cmd, (static_cast<uint32_t>(instances.size()) + 63) / 64, 1, 1);

  // Phase 1 also waits on phase 0's visibility reads before writing.
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, clusterCullPipeline);
  vkCmdDispatchIndirect(cmd, drawCounters.buffer,
                        offsetof(DrawCounters, workDispatch) +
                            phase * sizeof(DrawCounters::workDispatch[0]));
  if (phase == 1) {
    // Retest what phase 0 found occluded by last frame's pyramid; draws into
    // phase 1's lists like the newly visible instances.
    constants.phase = kCullRetest;
    vkCmdPushConstants(cmd, cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(constants), &constants);
    vkCmdDispatchIndirect(cmd, drawCounters.buffer,
                          offsetof(DrawCounters, retestDispatch));
  }
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                    VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
                    VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  if (phase == 1) {
    const VkBufferCopy copy{0, 0, sizeof(DrawCounters)};
    vkCmdCopyBuffer(cmd, drawCounters.buffer, frame.drawCountReadback.buffer, 1, &copy);
    // A fence wait alone doesn't make device writes visible to the host.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  }
}

// Reduces the scene depth into the pyramid, one level per dispatch.
void Renderer::Impl::buildDepthPyramid(VkCommandBuffer cmd) {
  transitionImage(cmd, {depth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
  // The pyramid is shared by both frames in flight: wait for the previous
  // frame's reads before overwriting it.
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pyramidPipeline);
  int32_t sourceWidth = static_cast<int32_t>(swapchain.extent.width);
  int32_t sourceHeight = static_cast<int32_t>(swapchain.extent.height);
  for (uint32_t level = 0; level < depthPyramidLevels.size(); ++level) {
    PyramidConstants constants;
    constants.sourceSize[0] = sourceWidth;
    constants.sourceSize[1] = sourceHeight;
    constants.destinationSize[0] =
        std::max(1, static_cast<int32_t>(depthPyramidExtent.width >> level));
    constants.destinationSize[1] =
        std::max(1, static_cast<int32_t>(depthPyramidExtent.height >> level));
    constants.level = level;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pyramidLayout, 0,
                            1, &pyramidSets[level], 0, nullptr);
    vkCmdPushConstants(cmd, pyramidLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(constants), &constants);
    vkCmdDispatch(cmd, (constants.destinationSize[0] + 7) / 8,
                  (constants.destinationSize[1] + 7) / 8, 1);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    sourceWidth = constants.destinationSize[0];
    sourceHeight = constants.destinationSize[1];
  }
}

// Rebuilds the TLAS over all instances (including those outside the view:
// they still cast shadows into it).
void Renderer::Impl::buildTlas(VkCommandBuffer cmd, FrameData &frame) {
  auto *tlasInstances = static_cast<VkAccelerationStructureInstanceKHR *>(
      frame.instanceBuffer.mapped);
  for (size_t i = 0; i < instances.size(); ++i) {
    const GpuMesh &mesh = meshes[instances[i].meshIndex];
    const Mat4 &m = instances[i].transform;
    VkAccelerationStructureInstanceKHR instance{};
    // Vulkan wants a row-major 3x4 matrix; Mat4 is column-major.
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 4; ++column) {
        instance.transform.matrix[row][column] = m.m[column][row];
      }
    }
    instance.instanceCustomIndex = static_cast<uint32_t>(i);
    instance.mask = mesh.alphaMode == AlphaMode::Blend ? kNonCasterMask
                                                       : kShadowCasterMask;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = mesh.blas.address;
    tlasInstances[i] = instance;
  }
  check(vmaFlushAllocation(allocator, frame.instanceBuffer.allocation, 0,
                           VK_WHOLE_SIZE),
        "vmaFlushAllocation");

  VkAccelerationStructureGeometryKHR geometry{};
  geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  geometry.geometry.instances.data.deviceAddress = frame.instanceBuffer.address;

  VkAccelerationStructureBuildGeometryInfoKHR build{};
  build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.dstAccelerationStructure = frame.tlas.handle;
  build.geometryCount = 1;
  build.pGeometries = &geometry;
  build.scratchData.deviceAddress = frame.tlasScratch.address;

  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = static_cast<uint32_t>(instances.size());
  const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
  {
    DriverLeakScope driverLeak;
    dispatch.cmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);
  }

  VkMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;

  VkDependencyInfo dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dependency);
}

float Renderer::gpuFrameMs() const { return impl_->gpuFrameMs; }

std::span<const PassTime> Renderer::gpuPassTimes() const {
  return {impl_->passTimes.data(), impl_->passTimeCount};
}

uint32_t Renderer::drawnInstanceCount() const { return impl_->drawnInstances; }

uint32_t Renderer::drawnClusterCount() const { return impl_->drawnClusters; }

uint32_t Renderer::drawnTriangleCount() const { return impl_->drawnTriangles; }

uint32_t Renderer::droppedCullEntryCount() const { return impl_->droppedEntries; }

void Renderer::setLodErrorPixels(float pixels) { impl_->lodErrorPixels = pixels; }

void Renderer::setOcclusionCulling(bool enabled) {
  impl_->occlusionCulling = enabled;
}

void Renderer::requestCapture(const std::filesystem::path &path) {
  impl_->capturePath = path;
}

uint32_t Renderer::validationErrorCount() const { return validationErrors; }

void Renderer::draw(const Camera &camera, const Lighting &lighting,
                    float deltaSeconds) {
  VkDevice dev = impl_->device.device;

  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(impl_->window, &width, &height);
  if (width == 0 || height == 0) {
    return;
  }

  // Wayland never reports the swapchain as out of date on resize, so the
  // size has to be compared by hand.
  if (impl_->swapchainOutOfDate ||
      static_cast<uint32_t>(width) != impl_->swapchain.extent.width ||
      static_cast<uint32_t>(height) != impl_->swapchain.extent.height) {
    check(vkDeviceWaitIdle(dev), "vkDeviceWaitIdle");
    impl_->destroySwapchain();
    impl_->createSwapchain();
  }

  const uint32_t frameSlot =
      static_cast<uint32_t>(impl_->frameNumber % kFramesInFlight);
  FrameData &frame = impl_->frames[frameSlot];
  check(vkWaitForFences(dev, 1, &frame.renderFence, VK_TRUE, UINT64_MAX),
        "vkWaitForFences");

  // Once each slot has been submitted, the fence guarantees its previous
  // timestamps are written.
  uint64_t timestamps[kMaxPassTimes + 1] = {};
  const uint32_t timestampCount = impl_->passTimeCount + 1;
  if (impl_->frameNumber >= kFramesInFlight &&
      vkGetQueryPoolResults(dev, impl_->timestampPool,
                            frameSlot * (kMaxPassTimes + 1), timestampCount,
                            sizeof(timestamps), timestamps, sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
    const float msPerTick = impl_->timestampPeriodNs * 1e-6f;
    for (uint32_t i = 0; i < impl_->passTimeCount; ++i) {
      impl_->passTimes[i].ms =
          static_cast<float>(timestamps[i + 1] - timestamps[i]) * msPerTick;
    }
    impl_->gpuFrameMs =
        static_cast<float>(timestamps[impl_->passTimeCount] - timestamps[0]) *
        msPerTick;
  }
  if (impl_->frameNumber >= kFramesInFlight) {
    check(vmaInvalidateAllocation(impl_->allocator, frame.drawCountReadback.allocation,
                                  0, VK_WHOLE_SIZE),
          "vmaInvalidateAllocation");
    const auto *counters =
        static_cast<const DrawCounters *>(frame.drawCountReadback.mapped);
    impl_->drawnInstances = counters->instances;
    impl_->drawnTriangles = counters->triangles;
    impl_->droppedEntries = counters->overflow;
    impl_->drawnTerrainNodes = counters->terrainNodes;
    impl_->drawnClusters = 0;
    for (const auto &lists : counters->meshTasks) {
      for (const auto &tasks : lists) {
        impl_->drawnClusters += tasks[0];
      }
    }
  }

  uint32_t imageIndex = 0;
  VkResult acquired = vkAcquireNextImageKHR(
      dev, impl_->swapchain.swapchain, UINT64_MAX, frame.imageAvailable,
      VK_NULL_HANDLE, &imageIndex);
  if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
    impl_->swapchainOutOfDate = true;
    return;
  }
  if (acquired != VK_SUBOPTIMAL_KHR) {
    check(acquired, "vkAcquireNextImageKHR");
  }

  // Reset only once we know we'll submit, or the next wait never returns.
  check(vkResetFences(dev, 1, &frame.renderFence), "vkResetFences");

  const VkExtent2D extent = impl_->swapchain.extent;
  const float aspect =
      static_cast<float>(extent.width) / static_cast<float>(extent.height);

  // Sub-pixel jitter in pixels, cycling through Halton(2,3). The temporal
  // resolve turns it into anti-aliasing (and DLSS will need it too).
  const uint32_t phase =
      static_cast<uint32_t>(impl_->frameNumber % kJitterPhases) + 1;
  const float jitterX = halton(phase, 2) - 0.5f;
  const float jitterY = halton(phase, 3) - 0.5f;

  // Camera-relative: the matrices leave out the translation (see
  // SceneBuffer in common.glsl).
  const Mat4 view = camera.viewRotation();
  const Mat4 projection = camera.projection(aspect);
  Mat4 jitteredProjection = projection;
  jitteredProjection.m[2][0] += 2.0f * jitterX / static_cast<float>(extent.width);
  jitteredProjection.m[2][1] += 2.0f * jitterY / static_cast<float>(extent.height);

  SceneData scene;
  scene.viewProjection = jitteredProjection * view;
  scene.currentViewProjection = projection * view;
  if (!impl_->historyValid) {
    impl_->previousViewProjection = scene.currentViewProjection;
    impl_->previousView = view;
    impl_->previousCameraPosition = camera.position;
  }
  scene.previousViewProjection = impl_->previousViewProjection;
  scene.previousView = impl_->previousView;
  scene.previousCameraPosition = impl_->previousCameraPosition;
  impl_->previousViewProjection = scene.currentViewProjection;
  impl_->previousView = view;
  impl_->previousCameraPosition = camera.position;
  scene.frameIndex = static_cast<uint32_t>(impl_->frameNumber);
  scene.sunDirection = lighting.sunDirection;
  scene.exposure = impl_->exposureBuffer.address;
  scene.sunIlluminance = lighting.sunIlluminance;
  scene.cameraPosition = camera.position;
  scene.sunAngularRadius = lighting.sunAngularRadius;
  scene.skyIrradiance = impl_->skyIrradianceBuffer.address;
  scene.inverseResolution[0] = 1.0f / static_cast<float>(extent.width);
  scene.inverseResolution[1] = 1.0f / static_cast<float>(extent.height);
  scene.atmosphere = toGpu(lighting.atmosphere);
  scene.terrain = impl_->terrain;
  if (impl_->terrain.enabled) {
    for (int i = 0; i < 3; ++i) {
      GpuTerrainLayer &layer = scene.terrain.layers[i];
      const double period = kTerrainWrapTiles * static_cast<double>(layer.tileSize);
      // Soil and needles (0, 1) are hex-tiled; rock is triplanar.
      const double periodZ =
          i < 2 ? kTerrainHexWrapRows * std::sqrt(3.0) / 4.0 * layer.tileSize : period;
      layer.offset[0] = wrapAround(camera.position.x, period);
      layer.offset[1] = wrapAround(camera.position.y, period);
      layer.offset[2] = wrapAround(camera.position.z, periodZ);
    }
    scene.terrain.noiseOffset[0] = wrapAround(camera.position.x, kTerrainNoiseWrap);
    scene.terrain.noiseOffset[1] = wrapAround(camera.position.z, kTerrainNoiseWrap);
  }
  scene.view = view;
  scene.projectionScale[0] = projection.m[0][0];
  scene.projectionScale[1] = projection.m[1][1];
  scene.nearPlane = camera.nearPlane;
  scene.tlas = frame.tlas.address;
  scene.vertices = impl_->vertexPool.address;
  scene.indices = impl_->indexPool.address;
  scene.meshes = impl_->meshTable.address;
  scene.instances = impl_->instanceTable.address;
  scene.materials = impl_->materialBuffer.address;
  scene.instanceCount = static_cast<uint32_t>(impl_->instances.size());
  scene.lodErrorPixels = impl_->lodErrorPixels;
  scene.clusters = impl_->clusterPool.address;
  scene.clusterVertices = impl_->clusterVertexPool.address;
  scene.clusterTriangles = impl_->clusterTrianglePool.address;
  scene.drawLists = impl_->drawLists.address;
  extractFrustumPlanes(scene.currentViewProjection, scene.frustumPlanes);
  std::memcpy(frame.sceneBuffer.mapped, &scene, sizeof(scene));
  check(vmaFlushAllocation(impl_->allocator, frame.sceneBuffer.allocation, 0,
                           VK_WHOLE_SIZE),
        "vmaFlushAllocation");

  VkCommandBuffer cmd = frame.commandBuffer;
  check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  check(vkBeginCommandBuffer(cmd, &beginInfo), "vkBeginCommandBuffer");

  const uint32_t firstQuery = frameSlot * (kMaxPassTimes + 1);
  vkCmdResetQueryPool(cmd, impl_->timestampPool, firstQuery, kMaxPassTimes + 1);
  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_NONE, impl_->timestampPool,
                       firstQuery);
  // Closes the named pass: it spans from the previous timestamp to here.
  uint32_t passCount = 0;
  auto endPass = [&](const char *name) {
    if (passCount == kMaxPassTimes) {
      throw std::runtime_error("more passes than kMaxPassTimes");
    }
    impl_->passTimes[passCount].name = name;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         impl_->timestampPool, firstQuery + ++passCount);
  };

  VkViewport viewport{};
  viewport.width = static_cast<float>(extent.width);
  viewport.height = static_cast<float>(extent.height);
  viewport.maxDepth = 1.0f;
  VkRect2D scissor = {{0, 0}, extent};

  impl_->uploadInstances(cmd, frame);
  impl_->buildTlas(cmd, frame);
  endPass("upload + tlas");
  impl_->resetDrawCounters(cmd);
  impl_->selectTerrain(cmd, frame);
  endPass("terrain select");
  impl_->cullInstances(cmd, frame, 0);
  endPass("cull early");

  // --- Atmosphere: LUTs for the sky, sun color, ambient light and haze.

  // The LUTs are shared by both frames in flight; wait for the previous
  // frame's reads before overwriting them.
  VkMemoryBarrier2 atmosphereBarrier{};
  atmosphereBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  atmosphereBarrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  atmosphereBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  atmosphereBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  VkDependencyInfo atmosphereDependency{};
  atmosphereDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  atmosphereDependency.memoryBarrierCount = 1;
  atmosphereDependency.pMemoryBarriers = &atmosphereBarrier;
  vkCmdPipelineBarrier2(cmd, &atmosphereDependency);

  // From here on, each pass reads what the one before it wrote.
  atmosphereBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  atmosphereBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  atmosphereBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  atmosphereBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

  PushConstants scenePush;
  scenePush.sceneBuffer = frame.sceneBuffer.address;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          impl_->sceneLayout, 0, 1, &impl_->atmosphereSet, 0,
                          nullptr);
  vkCmdPushConstants(cmd, impl_->sceneLayout, kScenePushStages, 0,
                     sizeof(scenePush), &scenePush);

  if (impl_->builtAtmosphere != lighting.atmosphere) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      impl_->transmittanceLutPipeline);
    vkCmdDispatch(cmd, 256 / 8, 64 / 8, 1);
    vkCmdPipelineBarrier2(cmd, &atmosphereDependency);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      impl_->multiScatteringLutPipeline);
    vkCmdDispatch(cmd, 32, 32, 1);
    vkCmdPipelineBarrier2(cmd, &atmosphereDependency);
    impl_->builtAtmosphere = lighting.atmosphere;
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    impl_->skyViewLutPipeline);
  vkCmdDispatch(cmd, (192 + 7) / 8, (108 + 7) / 8, 1);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    impl_->aerialPerspectiveLutPipeline);
  vkCmdDispatch(cmd, 32 / 8, 32 / 8, 32);
  vkCmdPipelineBarrier2(cmd, &atmosphereDependency);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    impl_->skyIrradiancePipeline);
  vkCmdDispatch(cmd, 1, 1, 1);
  vkCmdPipelineBarrier2(cmd, &atmosphereDependency);
  endPass("atmosphere");

  // --- Scene pass: shade into the HDR target.

  // The HDR and depth images are shared by both frames in flight, so each
  // barrier also waits for the previous frame's use of the image.
  for (VkImage target : {impl_->hdr.image, impl_->motion.image}) {
    transitionImage(cmd, {target, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
  }

  constexpr VkPipelineStageFlags2 kDepthStages =
      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  transitionImage(cmd, {impl_->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        kDepthStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        kDepthStages,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT});

  VkRenderingAttachmentInfo hdrAttachment{};
  hdrAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  hdrAttachment.imageView = impl_->hdr.view;
  hdrAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  // The sky pass covers every pixel, so nothing needs clearing.
  hdrAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  hdrAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingAttachmentInfo depthAttachment{};
  depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  depthAttachment.imageView = impl_->depth.view;
  depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  // Kept for the depth pyramid.
  depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depthAttachment.clearValue.depthStencil.depth = 0.0f;

  VkRenderingInfo sceneRendering{};
  sceneRendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  sceneRendering.renderArea = {{0, 0}, extent};
  sceneRendering.layerCount = 1;
  VkRenderingAttachmentInfo motionAttachment{};
  motionAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  motionAttachment.imageView = impl_->motion.view;
  motionAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  motionAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  motionAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  const VkRenderingAttachmentInfo sceneAttachments[] = {hdrAttachment,
                                                        motionAttachment};
  sceneRendering.colorAttachmentCount = 2;
  sceneRendering.pColorAttachments = sceneAttachments;
  sceneRendering.pDepthAttachment = &depthAttachment;

  // Two phases (occlusion culling): first what was visible last frame, then,
  // tested against a depth pyramid of that, whatever has become visible.
  auto beginScene = [&] {
    vkCmdBeginRendering(cmd, &sceneRendering);
    const VkDescriptorSet sceneSets[] = {impl_->atmosphereSet, impl_->materialSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            impl_->sceneLayout, 0, 2, sceneSets, 0, nullptr);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushConstants(cmd, impl_->sceneLayout, kScenePushStages, 0,
                       sizeof(scenePush), &scenePush);
    vkCmdBindIndexBuffer(cmd, impl_->indexPool.buffer, 0, VK_INDEX_TYPE_UINT32);
  };
  // The GPU-culled cluster draw lists: opaque, then alpha-masked, each
  // single- and double-sided, one mesh workgroup per cluster.
  auto drawCulled = [&](uint32_t phase) {
    for (uint32_t bucket = 0; bucket < kDrawBuckets; ++bucket) {
      const bool masked = bucket >= 2;
      const bool doubleSided = bucket % 2 == 1;
      PushConstants listPush = scenePush;
      listPush.drawList = phase * kDrawBuckets + bucket;
      vkCmdPushConstants(cmd, impl_->sceneLayout, kScenePushStages, 0,
                         sizeof(listPush), &listPush);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        masked ? impl_->maskedMeshPipeline : impl_->meshPipeline);
      vkCmdSetCullMode(cmd, doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
      impl_->dispatch.cmdDrawMeshTasksIndirectEXT(
          cmd, impl_->drawCounters.buffer,
          offsetof(DrawCounters, meshTasks) +
              listPush.drawList * sizeof(DrawCounters::meshTasks[0][0]),
          1, sizeof(DrawCounters::meshTasks[0][0]));
    }
    vkCmdPushConstants(cmd, impl_->sceneLayout, kScenePushStages, 0,
                       sizeof(scenePush), &scenePush);
  };

  beginScene();
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->skyPipeline);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  impl_->drawTerrain(cmd);
  drawCulled(0);
  vkCmdEndRendering(cmd);
  endPass("scene early");

  impl_->buildDepthPyramid(cmd);
  impl_->pyramidValid = true;
  endPass("pyramid");
  impl_->cullInstances(cmd, frame, 1);
  endPass("cull late");

  transitionImage(cmd, {impl_->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, kDepthStages,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT});
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  VkRenderingAttachmentInfo lateAttachments[] = {hdrAttachment, motionAttachment};
  lateAttachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  lateAttachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  // Nothing reads depth after the late pass.
  depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  sceneRendering.pColorAttachments = lateAttachments;
  beginScene();
  drawCulled(1);

  // Blended last, back to front, from the CPU: each surface must composite
  // over everything behind it, and there are few of them.
  impl_->blendedDraws.clear();
  for (uint32_t i = 0; i < impl_->instances.size(); ++i) {
    const InstanceData &instance = impl_->instances[i];
    if (impl_->meshes[instance.meshIndex].drawBucket == kDrawBucketBlended) {
      const Mat4 &t = instance.transform;
      const Vec3 offset = Vec3{t.m[3][0], t.m[3][1], t.m[3][2]} - camera.position;
      impl_->blendedDraws.emplace_back(dot(offset, offset), i);
    }
  }
  std::sort(impl_->blendedDraws.begin(), impl_->blendedDraws.end(),
            [](const auto &a, const auto &b) { return a.first > b.first; });
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    impl_->blendedMeshPipeline);
  for (const auto &[distance, index] : impl_->blendedDraws) {
    const GpuMesh &mesh = impl_->meshes[impl_->instances[index].meshIndex];
    vkCmdSetCullMode(cmd, mesh.doubleSided ? VK_CULL_MODE_NONE
                                           : VK_CULL_MODE_BACK_BIT);
    vkCmdDrawIndexed(cmd, mesh.indexCount, 1, mesh.firstIndex, mesh.vertexOffset,
                     index);
  }

  vkCmdEndRendering(cmd);
  endPass("scene late");

  // --- Temporal resolve: scene color + history -> new history.

  for (VkImage target : {impl_->hdr.image, impl_->motion.image}) {
    transitionImage(cmd, {target, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
  }

  // The history images stay in GENERAL. This frame reads what the previous
  // frame wrote, and overwrites the image the previous frame's tonemap read.
  VkMemoryBarrier2 historyBarrier{};
  historyBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  historyBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  historyBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  historyBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  historyBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  VkDependencyInfo historyDependency{};
  historyDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  historyDependency.memoryBarrierCount = 1;
  historyDependency.pMemoryBarriers = &historyBarrier;
  vkCmdPipelineBarrier2(cmd, &historyDependency);

  VkDescriptorSet postSet = impl_->postSets[impl_->frameNumber % 2];
  PostConstants postConstants;
  postConstants.historyValid = impl_->historyValid ? 1 : 0;
  postConstants.deltaSeconds = deltaSeconds;
  postConstants.histogram = impl_->histogramBuffer.address;
  postConstants.exposure = impl_->exposureBuffer.address;
  postConstants.exposureCompensation = camera.exposureCompensation;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->taaPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          impl_->postLayout, 0, 1, &postSet, 0, nullptr);
  vkCmdPushConstants(cmd, impl_->postLayout,
                     VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(postConstants), &postConstants);
  vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);

  // Every pass below reads what the previous one wrote, so one barrier
  // shape (compute writes -> compute/fragment reads) serves all of them.
  historyBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  historyBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  historyBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  historyBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  vkCmdPipelineBarrier2(cmd, &historyDependency);
  impl_->historyValid = true;
  endPass("taa");

  // --- Auto-exposure: histogram of the resolved frame, then adaptation.
  // The new exposure is used by the next frame's scene pass.

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    impl_->histogramPipeline);
  // One thread per 2x2 pixels (see histogram.comp).
  vkCmdDispatch(cmd, (extent.width + 31) / 32, (extent.height + 31) / 32, 1);
  vkCmdPipelineBarrier2(cmd, &historyDependency);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    impl_->exposurePipeline);
  vkCmdDispatch(cmd, 1, 1, 1);
  vkCmdPipelineBarrier2(cmd, &historyDependency);
  endPass("exposure");

  // --- Tonemap pass: resolved HDR -> swapchain image.

  VkImage swapchainImage = impl_->swapchainImages[imageIndex];
  transitionImage(cmd, {swapchainImage, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});

  VkRenderingAttachmentInfo swapchainAttachment{};
  swapchainAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  swapchainAttachment.imageView = impl_->swapchainImageViews[imageIndex];
  swapchainAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  swapchainAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swapchainAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo tonemapRendering{};
  tonemapRendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  tonemapRendering.renderArea = {{0, 0}, extent};
  tonemapRendering.layerCount = 1;
  tonemapRendering.colorAttachmentCount = 1;
  tonemapRendering.pColorAttachments = &swapchainAttachment;

  vkCmdBeginRendering(cmd, &tonemapRendering);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    impl_->tonemapPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          impl_->postLayout, 0, 1, &postSet, 0, nullptr);
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  endPass("tonemap");
  impl_->passTimeCount = passCount;

  // Optional capture: copy the finished frame into a CPU-readable buffer.
  Buffer captureBuffer;
  const bool capturing = impl_->capturePath.has_value();
  if (capturing) {
    captureBuffer = impl_->createBuffer(
        VkDeviceSize{extent.width} * extent.height * 4,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    transitionImage(cmd, {swapchainImage, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT});
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, swapchainImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           captureBuffer.buffer, 1, &region);
    transitionImage(cmd, {swapchainImage, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                          VK_PIPELINE_STAGE_2_NONE, 0});
  } else {
    transitionImage(cmd, {swapchainImage, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_NONE, 0});
  }

  check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

  VkCommandBufferSubmitInfo cmdInfo{};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cmdInfo.commandBuffer = cmd;

  VkSemaphoreSubmitInfo waitInfo{};
  waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  waitInfo.semaphore = frame.imageAvailable;
  waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

  VkSemaphoreSubmitInfo signalInfo{};
  signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  signalInfo.semaphore = impl_->renderFinished[imageIndex];
  signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

  VkSubmitInfo2 submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit.waitSemaphoreInfoCount = 1;
  submit.pWaitSemaphoreInfos = &waitInfo;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmdInfo;
  submit.signalSemaphoreInfoCount = 1;
  submit.pSignalSemaphoreInfos = &signalInfo;
  check(vkQueueSubmit2(impl_->graphicsQueue, 1, &submit, frame.renderFence),
        "vkQueueSubmit2");

  if (capturing) {
    check(vkWaitForFences(dev, 1, &frame.renderFence, VK_TRUE, UINT64_MAX),
          "vkWaitForFences");
    check(vmaInvalidateAllocation(impl_->allocator, captureBuffer.allocation, 0,
                                  VK_WHOLE_SIZE),
          "vmaInvalidateAllocation");
    // The swapchain is BGRA8 sRGB, already display-encoded; PNG wants RGBA.
    basisu::image png(extent.width, extent.height);
    const auto *bgra = static_cast<const uint8_t *>(captureBuffer.mapped);
    for (uint32_t i = 0; i < extent.width * extent.height; ++i) {
      png.get_ptr()[i].set(bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4], 255);
    }
    if (!basisu::save_png(impl_->capturePath->string(), png)) {
      std::fprintf(stderr, "Capture failed: %s\n", impl_->capturePath->c_str());
    }
    impl_->destroyBuffer(captureBuffer);
    impl_->capturePath.reset();
  }

  VkPresentInfoKHR present{};
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &impl_->renderFinished[imageIndex];
  present.swapchainCount = 1;
  present.pSwapchains = &impl_->swapchain.swapchain;
  present.pImageIndices = &imageIndex;
  VkResult presented = vkQueuePresentKHR(impl_->graphicsQueue, &present);
  if (presented == VK_ERROR_OUT_OF_DATE_KHR ||
      presented == VK_SUBOPTIMAL_KHR) {
    impl_->swapchainOutOfDate = true;
  } else {
    check(presented, "vkQueuePresentKHR");
  }

  ++impl_->frameNumber;
}

} // namespace meridian
