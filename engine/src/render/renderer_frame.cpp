#include "renderer_impl.h"

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <basisu_enc.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <iostream>
#include <span>
#include <stdexcept>

namespace meridian {

namespace {

// x minus the nearest multiple of period: identity within half a period of
// the origin.
float wrapAround(float x, double period) {
  return static_cast<float>(x - period * std::round(x / period));
}

float halfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t{h & 0x8000u} << 16;
  const uint32_t exponent = (h >> 10) & 0x1f;
  const uint32_t mantissa = h & 0x3ff;
  if (exponent == 0) {
    const float subnormal = std::ldexp(static_cast<float>(mantissa), -24);
    return sign ? -subnormal : subnormal;
  }
  const uint32_t bits =
      exponent == 31 ? sign | 0x7f800000u | (mantissa << 13)
                     : sign | ((exponent + 112) << 23) | (mantissa << 13);
  return std::bit_cast<float>(bits);
}

// Portable Float Map: RGB float, rows bottom to top; a negative scale means
// little-endian, which the host is.
bool savePfm(const std::filesystem::path &path, VkExtent2D extent,
             const uint16_t *rgba) {
  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (!file) {
    return false;
  }
  std::fprintf(file, "PF\n%u %u\n-1.0\n", extent.width, extent.height);
  std::vector<float> row(size_t{extent.width} * 3);
  for (uint32_t y = extent.height; y-- > 0;) {
    for (uint32_t x = 0; x < extent.width; ++x) {
      for (uint32_t c = 0; c < 3; ++c) {
        row[x * 3 + c] =
            halfToFloat(rgba[(size_t{y} * extent.width + x) * 4 + c]);
      }
    }
    std::fwrite(row.data(), sizeof(float), row.size(), file);
  }
  const bool written = !std::ferror(file);
  return std::fclose(file) == 0 && written;
}

GpuGrade toGpu(const Grade &grade) {
  GpuGrade gpu;
  const std::array<float, 9> wb =
      whiteBalanceMatrix(grade.temperatureKelvin, grade.tint);
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      gpu.whiteBalance[row][column] = wb[row * 3 + column];
    }
  }
  const Vec3 cdl[] = {grade.slope, grade.offset, grade.power};
  float *targets[] = {gpu.slope, gpu.offset, gpu.power};
  for (int i = 0; i < 3; ++i) {
    targets[i][0] = cdl[i].x;
    targets[i][1] = cdl[i].y;
    targets[i][2] = cdl[i].z;
  }
  gpu.exposureScale = std::exp2(grade.exposureEv);
  gpu.contrast = grade.contrast;
  gpu.saturation = grade.saturation;
  const std::pair<const HueBand *, float> bands[] = {
      {&grade.greens, kGreensHue},
      {&grade.blues, kBluesHue},
      {&grade.browns, kBrownsHue}};
  for (int i = 0; i < 3; ++i) {
    gpu.bands[i][0] = bands[i].second;
    gpu.bands[i][1] = kHueBandHalfWidth;
    gpu.bands[i][2] = bands[i].first->hueShiftDegrees;
    gpu.bands[i][3] = bands[i].first->saturation;
  }
  return gpu;
}

// Largest difference between the baked LUT (RGBA16F) and the CPU reference.
float gradeLutDifference(const Grade &grade, const uint16_t *rgba) {
  const std::vector<std::array<float, 4>> reference = bakeGradeLut(grade);
  float worst = 0.0f;
  for (size_t i = 0; i < reference.size(); ++i) {
    for (size_t c = 0; c < 3; ++c) {
      const float d = std::abs(halfToFloat(rgba[i * 4 + c]) - reference[i][c]);
      if (std::isnan(d) || d > worst) { // NaN sticks, so it fails the check
        worst = d;
      }
    }
  }
  return worst;
}

} // namespace

// Writes the CPU instance list to the GPU instance table. The table is
// shared by both frames in flight, so the copy waits for the previous
// frame's reads.
void Renderer::Impl::uploadInstances(VkCommandBuffer cmd, FrameData &frame) {
  if (instances.empty()) {
    return;
  }
  const VkDeviceSize bytes = instances.size() * sizeof(InstanceData);
  std::memcpy(frame.instanceStaging.mapped, instances.data(), bytes);
  check(vmaFlushAllocation(allocator, frame.instanceStaging.allocation, 0, bytes),
        "vmaFlushAllocation");

  constexpr VkPipelineStageFlags2 kReaders =
      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
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
  for (auto &dispatch : initial.chunkDispatch) {
    dispatch[1] = dispatch[2] = 1;
  }
  for (auto &depths : initial.traverseDispatch) {
    for (auto &dispatch : depths) {
      dispatch[1] = dispatch[2] = 1;
    }
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

CullConstants Renderer::Impl::cullConstants(const FrameData &frame, uint32_t phase) const {
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
  constants.chunks = chunks.address + VkDeviceSize{phase} * kMaxChunks * 2 * sizeof(uint32_t);
  constants.nodeQueues = nodeQueues.address;
  return constants;
}

// One phase of the cell cull: placement cells (and the hand-placed
// instances) become chunks of instances for cullInstances. Phase 1 also
// waits on phase 0's visibility reads before writing.
void Renderer::Impl::cullCells(VkCommandBuffer cmd, const FrameData &frame, uint32_t phase) {
  const CullConstants constants = cullConstants(frame, phase);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cellCullPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullLayout, 0, 1, &cullSet, 0,
                          nullptr);
  vkCmdPushConstants(cmd, cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                     &constants);
  const uint32_t threads = placementGrid.columns * placementGrid.rows +
                           (static_cast<uint32_t>(instances.size()) + 63) / 64;
  vkCmdDispatch(cmd, std::max(1u, (threads + 63) / 64), 1, 1);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

// Culls on the GPU and fills one phase's cluster draw lists, in three steps
// (cullInstances, traverseClusters, cullClusters). Phase 0 takes last
// frame's visible instances (frustum test only); phase 1 tests all of them
// against the depth pyramid, adds the newly visible ones and records
// visibility for the next frame. In each phase the instance cull runs over
// cullCells' chunks and seeds the cluster BVH traversal, whose leaves emit
// work items the cluster cull LOD-selects and culls.
void Renderer::Impl::cullInstances(VkCommandBuffer cmd, const FrameData &frame,
                                   uint32_t phase) {
  CullConstants constants = cullConstants(frame, phase);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullLayout, 0, 1,
                          &cullSet, 0, nullptr);
  vkCmdPushConstants(cmd, cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(constants), &constants);
  vkCmdDispatchIndirect(cmd, drawCounters.buffer,
                        offsetof(DrawCounters, chunkDispatch) +
                            phase * sizeof(DrawCounters::chunkDispatch[0]));

  // Phase 1 also waits on phase 0's visibility reads before writing.
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

// One dispatch per BVH depth, each over the nodes the previous one pushed.
void Renderer::Impl::traverseClusters(VkCommandBuffer cmd, const FrameData &frame,
                                      uint32_t phase) {
  CullConstants constants = cullConstants(frame, phase);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, clusterTraversePipeline);
  for (uint32_t depth = 0; depth < traversalDepths; ++depth) {
    constants.depth = depth;
    vkCmdPushConstants(cmd, cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(constants), &constants);
    vkCmdDispatchIndirect(cmd, drawCounters.buffer,
                          offsetof(DrawCounters, traverseDispatch) +
                              (phase * kBvhDepths + depth) *
                                  sizeof(DrawCounters::traverseDispatch[0][0]));
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  }
}

void Renderer::Impl::cullClusters(VkCommandBuffer cmd, const FrameData &frame,
                                  uint32_t phase) {
  CullConstants constants = cullConstants(frame, phase);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, clusterCullPipeline);
  vkCmdPushConstants(cmd, cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(constants), &constants);
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
  int32_t sourceWidth = static_cast<int32_t>(renderExtent.width);
  int32_t sourceHeight = static_cast<int32_t>(renderExtent.height);
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

// Builds the TLAS over all instances (including those outside the view:
// they still cast shadows into it): the placed instances' records, written
// at setPlacement, then the hand-placed ones', copied from the frame's
// staging. Both frames in flight share it, so the build waits for earlier
// frames' ray queries and builds, and the barrier after it covers every
// later frame on the queue.
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

  VkMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                         VK_PIPELINE_STAGE_2_COPY_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                          VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                          VK_ACCESS_2_TRANSFER_WRITE_BIT;
  VkDependencyInfo dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dependency);
  if (!instances.empty()) {
    constexpr VkDeviceSize kRecord = sizeof(VkAccelerationStructureInstanceKHR);
    const VkBufferCopy copy{0, placedCount * kRecord, instances.size() * kRecord};
    vkCmdCopyBuffer(cmd, frame.instanceBuffer.buffer, tlasRecords.buffer, 1, &copy);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                  VK_ACCESS_2_SHADER_READ_BIT);
  }

  VkAccelerationStructureGeometryKHR geometry{};
  geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  geometry.geometry.instances.data.deviceAddress = tlasRecords.address;

  VkAccelerationStructureBuildGeometryInfoKHR build{};
  build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.dstAccelerationStructure = tlas.handle;
  build.geometryCount = 1;
  build.pGeometries = &geometry;
  build.scratchData.deviceAddress = tlasScratch.address;

  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = placedCount + static_cast<uint32_t>(instances.size());
  const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
  {
    DriverLeakScope driverLeak;
    dispatch.cmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);
  }

  barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  vkCmdPipelineBarrier2(cmd, &dependency);
}

void Renderer::beginOverlay() {
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  impl_->overlayPending = true;
}

float Renderer::cpuWaitMs() const { return impl_->cpuWaitMs; }

float Renderer::gpuFrameMs() const { return impl_->gpuFrameMs; }

std::span<const PassTime> Renderer::gpuPassTimes() const {
  return {impl_->passTimes.data(), impl_->passTimeCount};
}

uint32_t Renderer::drawnInstanceCount() const { return impl_->drawnInstances; }

uint32_t Renderer::drawnClusterCount() const { return impl_->drawnClusters; }

uint32_t Renderer::drawnTriangleCount() const { return impl_->drawnTriangles; }

uint32_t Renderer::droppedCullEntryCount() const { return impl_->droppedEntries; }

CullDrops Renderer::droppedCullEntries() const {
  const auto &d = impl_->droppedByList;
  return {d[0], d[1], d[2], d[3], d[4]};
}

uint32_t Renderer::drawnCellCount() const { return impl_->drawnCells; }

uint32_t Renderer::visitedBvhNodeCount() const { return impl_->visitedBvhNodes; }

void Renderer::setLodErrorPixels(float pixels) { impl_->lodErrorPixels = pixels; }

void Renderer::setGrade(const Grade &grade) {
  impl_->grade = grade;
  impl_->gradeDirty = true;
}

void Renderer::setBloom(const Bloom &bloom) {
  impl_->bloom = {std::clamp(bloom.intensity, 0.0f, 1.0f),
                  std::clamp(bloom.scatter, 0.0f, 1.0f)};
}

void Renderer::setOcclusionCulling(bool enabled) {
  impl_->occlusionCulling = enabled;
}

void Renderer::Impl::setAaSettings(const AaSettings &settings) {
  if (!(settings.renderScale > 0.0f && settings.renderScale <= 1.0f)) {
    throw std::invalid_argument("setAntiAliasing: renderScale must be in (0, 1]");
  }
  aaSettings = settings;
  if (aaSettings.method != AaMethod::Dlss || !upscalerProvider) {
    aaSettings.method = AaMethod::Taa;
    aaSettings.rayReconstruction = false;
    return;
  }
  if (aaSettings.rayReconstruction && !upscalerProvider->supports(aaSettings)) {
    std::cout << "DLSS Ray Reconstruction unavailable; using Super Resolution\n";
    aaSettings.rayReconstruction = false;
  }
  if (!upscalerProvider->supports(aaSettings)) {
    std::cout << "DLSS unavailable: NGX reports no Super Resolution; using TAA\n";
    aaSettings.method = AaMethod::Taa;
  }
}

void Renderer::setAntiAliasing(const AaSettings &settings) {
  check(vkDeviceWaitIdle(impl_->device.device), "vkDeviceWaitIdle");
  impl_->setAaSettings(settings);
  impl_->destroySwapchain();
  impl_->createSwapchain();
}

AaSettings Renderer::antiAliasing() const { return impl_->aaSettings; }

void Renderer::requestCapture(const std::filesystem::path &path) {
  impl_->capturePath = path;
}

uint32_t Renderer::validationErrorCount() const { return validationErrors; }

void Renderer::draw(const Camera &camera, const Lighting &lighting,
                    float deltaSeconds) {
  VkDevice dev = impl_->device.device;

  int width = 0;
  int height = 0;
  impl_->windowSize->framebufferSize(width, height);
  // An early return still has to close the ImGui frame beginOverlay() opened.
  auto abandonOverlay = [&] {
    if (impl_->overlayPending) {
      impl_->overlayPending = false;
      ImGui::EndFrame();
    }
  };
  if (width == 0 || height == 0) {
    abandonOverlay();
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
  const auto waitStart = std::chrono::steady_clock::now();
  check(vkWaitForFences(dev, 1, &frame.renderFence, VK_TRUE, UINT64_MAX),
        "vkWaitForFences");
  const auto fenceDone = std::chrono::steady_clock::now();

  // Once each slot has been submitted, the fence guarantees its previous
  // timestamps are written.
  uint64_t timestamps[kMaxPassTimes + 1] = {};
  const uint32_t slotPasses = impl_->slotPassCount[frameSlot];
  const uint32_t timestampCount = slotPasses + 1;
  if (impl_->frameNumber >= kFramesInFlight &&
      vkGetQueryPoolResults(dev, impl_->timestampPool,
                            frameSlot * (kMaxPassTimes + 1), timestampCount,
                            sizeof(timestamps), timestamps, sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
    const float msPerTick = impl_->timestampPeriodNs * 1e-6f;
    impl_->passTimeCount = slotPasses;
    for (uint32_t i = 0; i < slotPasses; ++i) {
      impl_->passTimes[i].name = impl_->slotPassNames[frameSlot][i];
      impl_->passTimes[i].ms =
          static_cast<float>(timestamps[i + 1] - timestamps[i]) * msPerTick;
    }
    impl_->gpuFrameMs =
        static_cast<float>(timestamps[slotPasses] - timestamps[0]) *
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
    impl_->drawnCells = counters->cells;
    impl_->visitedBvhNodes = counters->bvhNodes;
    std::copy(std::begin(counters->drops), std::end(counters->drops),
              impl_->droppedByList.begin());
    impl_->drawnClusters = 0;
    for (const auto &lists : counters->meshTasks) {
      for (const auto &tasks : lists) {
        impl_->drawnClusters += tasks[0];
      }
    }
  }

  uint32_t imageIndex = 0;
  const auto acquireStart = std::chrono::steady_clock::now();
  VkResult acquired = vkAcquireNextImageKHR(
      dev, impl_->swapchain.swapchain, UINT64_MAX, frame.imageAvailable,
      VK_NULL_HANDLE, &imageIndex);
  const auto acquireDone = std::chrono::steady_clock::now();
  impl_->cpuWaitMs = std::chrono::duration<float, std::milli>(
                         (fenceDone - waitStart) + (acquireDone - acquireStart))
                         .count();
  if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
    impl_->swapchainOutOfDate = true;
    abandonOverlay();
    return;
  }
  if (acquired != VK_SUBOPTIMAL_KHR) {
    check(acquired, "vkAcquireNextImageKHR");
  }

  // Reset only once we know we'll submit, or the next wait never returns.
  check(vkResetFences(dev, 1, &frame.renderFence), "vkResetFences");

  // The scene renders at renderExtent, the resolve and later passes at the
  // display size.
  const VkExtent2D extent = impl_->swapchain.extent;
  const VkExtent2D renderExtent = impl_->renderExtent;
  const Extent render{renderExtent.width, renderExtent.height};
  const Extent display{extent.width, extent.height};
  const float aspect =
      static_cast<float>(extent.width) / static_cast<float>(extent.height);

  // Sub-pixel jitter in render pixels, cycling through Halton(2,3). The
  // temporal resolve turns it into anti-aliasing (and DLSS will need it too).
  const uint32_t phase =
      static_cast<uint32_t>(impl_->frameNumber %
                            jitterPhaseCount(render, display,
                                             impl_->aaSettings.rayReconstruction)) +
      1;
  const float jitterX = halton(phase, 2) - 0.5f;
  const float jitterY = halton(phase, 3) - 0.5f;

  // Camera-relative: the matrices leave out the translation (see
  // SceneBuffer in common.glsl).
  const Mat4 view = camera.viewRotation();
  const Mat4 projection = camera.projection(aspect);
  Mat4 jitteredProjection = projection;
  jitteredProjection.m[2][0] += 2.0f * jitterX / static_cast<float>(renderExtent.width);
  jitteredProjection.m[2][1] += 2.0f * jitterY / static_cast<float>(renderExtent.height);

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
  // Ray Reconstruction wants uncorrelated noise (its integration guide).
  scene.whiteNoise = impl_->aaSettings.rayReconstruction ? 1 : 0;
  scene.sunDirection = lighting.sunDirection;
  scene.exposure = impl_->exposureBuffer.address;
  scene.sunIlluminance = lighting.sunIlluminance;
  scene.cameraPosition = camera.position;
  scene.sunAngularRadius = lighting.sunAngularRadius;
  scene.skyIrradiance = impl_->skyIrradianceBuffer.address;
  scene.inverseResolution[0] = 1.0f / static_cast<float>(renderExtent.width);
  scene.inverseResolution[1] = 1.0f / static_cast<float>(renderExtent.height);
  scene.mipBias = textureMipBias(render, display);
  scene.atmosphere = toGpu(lighting.atmosphere);
  scene.terrain = impl_->terrain;
  if (impl_->terrain.enabled) {
    const uint32_t firstRock = scene.terrain.roles[static_cast<int>(TerrainRole::Rock)] & 0xFFu;
    for (uint32_t i = 0; i < scene.terrain.layerCount; ++i) {
      GpuTerrainLayer &layer = scene.terrain.layers[i];
      const double period = kTerrainWrapTiles * static_cast<double>(layer.tileSize);
      // Rock (the last role) is triplanar; the rest are hex-tiled.
      const double periodZ =
          i < firstRock ? kTerrainHexWrapRows * std::sqrt(3.0) / 4.0 * layer.tileSize : period;
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
  scene.tlas = impl_->tlas.address;
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
  scene.clusterNodes = impl_->clusterNodePool.address;
  scene.clusterLevels = impl_->clusterLevelPool.address;
  scene.drawLists = impl_->drawLists.address;
  scene.placedInstances = impl_->placedInstances.address;
  scene.cells = impl_->cells.address;
  scene.cellGrid[0] = impl_->placementGrid.firstX;
  scene.cellGrid[1] = impl_->placementGrid.firstZ;
  scene.cellGridSize[0] = impl_->placementGrid.columns;
  scene.cellGridSize[1] = impl_->placementGrid.rows;
  scene.prototypes = impl_->prototypeTable.address;
  scene.prototypeMeshes = impl_->prototypeMeshTable.address;
  scene.placedCount = impl_->placedCount;
  scene.cellDrawDistance = impl_->cellDrawDistance;
  scene.cellMargin = impl_->cellMargin;
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
    impl_->slotPassNames[frameSlot][passCount] = name;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         impl_->timestampPool, firstQuery + ++passCount);
  };

  VkViewport viewport{};
  viewport.width = static_cast<float>(renderExtent.width);
  viewport.height = static_cast<float>(renderExtent.height);
  viewport.maxDepth = 1.0f;
  VkRect2D scissor = {{0, 0}, renderExtent};

  if (impl_->instancesDirty) {
    impl_->uploadInstances(cmd, frame);
    impl_->buildTlas(cmd, frame);
    impl_->instancesDirty = false;
  }
  endPass("upload + tlas");
  impl_->resetDrawCounters(cmd);
  impl_->selectTerrain(cmd, frame);
  endPass("terrain select");
  impl_->cullCells(cmd, frame, 0);
  endPass("cull cells");
  impl_->cullInstances(cmd, frame, 0);
  endPass("cull early");
  impl_->traverseClusters(cmd, frame, 0);
  endPass("traverse");
  impl_->cullClusters(cmd, frame, 0);
  endPass("cull clusters");

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
  const bool guides = impl_->diffuseAlbedo.image != VK_NULL_HANDLE;
  std::array<VkImage, 6> colorTargets = {
      impl_->hdr.image,           impl_->motion.image,
      impl_->transparencyMask.image, impl_->diffuseAlbedo.image,
      impl_->specularAlbedo.image, impl_->normalRoughness.image};
  const size_t colorTargetCount = guides ? colorTargets.size() : 3;
  for (VkImage target : std::span(colorTargets.data(), colorTargetCount)) {
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
  sceneRendering.renderArea = {{0, 0}, renderExtent};
  sceneRendering.layerCount = 1;
  VkRenderingAttachmentInfo motionAttachment{};
  motionAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  motionAttachment.imageView = impl_->motion.view;
  motionAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  motionAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  motionAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  // Flags blended pixels for the upscaler; nothing else writes it.
  VkRenderingAttachmentInfo maskAttachment{};
  maskAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  maskAttachment.imageView = impl_->transparencyMask.view;
  maskAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  maskAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  maskAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  // A null view discards the writes when the upscaler wants no guides.
  auto guideAttachment = [&](const Image &image) {
    VkRenderingAttachmentInfo attachment{};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = image.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    return attachment;
  };
  const VkRenderingAttachmentInfo sceneAttachments[] = {
      hdrAttachment,
      motionAttachment,
      maskAttachment,
      guideAttachment(impl_->diffuseAlbedo),
      guideAttachment(impl_->specularAlbedo),
      guideAttachment(impl_->normalRoughness)};
  sceneRendering.colorAttachmentCount = std::size(sceneAttachments);
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
  impl_->cullCells(cmd, frame, 1);
  impl_->cullInstances(cmd, frame, 1);
  impl_->traverseClusters(cmd, frame, 1);
  impl_->cullClusters(cmd, frame, 1);
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
  VkRenderingAttachmentInfo lateAttachments[std::size(sceneAttachments)];
  std::copy(std::begin(sceneAttachments), std::end(sceneAttachments),
            lateAttachments);
  for (VkRenderingAttachmentInfo &attachment : lateAttachments) {
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  }
  depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
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

  // --- Upscaler: render-sized scene -> display-sized output.

  for (VkImage target : std::span(colorTargets.data(), colorTargetCount)) {
    transitionImage(cmd, {target, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
  }
  transitionImage(cmd, {impl_->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kDepthStages,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});

  // The output is shared by both frames in flight: the previous frame's
  // upscaler must be done writing it, and its histogram, tonemap and capture
  // reading it.
  VkMemoryBarrier2 postBarrier{};
  postBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  postBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COPY_BIT;
  postBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  postBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  postBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  VkDependencyInfo postDependency{};
  postDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  postDependency.memoryBarrierCount = 1;
  postDependency.pMemoryBarriers = &postBarrier;
  vkCmdPipelineBarrier2(cmd, &postDependency);

  auto upscalerImage = [](const Image &image, VkFormat format, VkExtent2D size) {
    return UpscalerImage{image.image, image.view, format, {size.width, size.height}};
  };
  UpscalerFrame upscale;
  upscale.cmd = cmd;
  upscale.color = upscalerImage(impl_->hdr, kHdrFormat, renderExtent);
  upscale.depth = upscalerImage(impl_->depth, kDepthFormat, renderExtent);
  upscale.motion = upscalerImage(impl_->motion, kMotionFormat, renderExtent);
  upscale.transparencyMask =
      upscalerImage(impl_->transparencyMask, kTransparencyMaskFormat, renderExtent);
  if (guides) {
    upscale.diffuseAlbedo =
        upscalerImage(impl_->diffuseAlbedo, kAlbedoFormat, renderExtent);
    upscale.specularAlbedo =
        upscalerImage(impl_->specularAlbedo, kAlbedoFormat, renderExtent);
    upscale.normalRoughness = upscalerImage(
        impl_->normalRoughness, kNormalRoughnessFormat, renderExtent);
    upscale.specularHitDistance = upscalerImage(
        impl_->specularHitDistance, kHitDistanceFormat, renderExtent);
  }
  upscale.output = upscalerImage(impl_->output, kHdrFormat, extent);
  upscale.jitter[0] = jitterX;
  upscale.jitter[1] = jitterY;
  upscale.motionScale[0] = static_cast<float>(renderExtent.width);
  upscale.motionScale[1] = static_cast<float>(renderExtent.height);
  upscale.worldToView = view;
  upscale.viewToClip = projection;
  upscale.frameSeconds = deltaSeconds;
  upscale.reset = !impl_->historyValid;
  impl_->upscaler->evaluate(upscale);

  // Every pass below reads what the previous one wrote, so one barrier
  // shape (compute writes -> compute/fragment reads) serves all of them.
  postBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  postBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  postBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  postBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  vkCmdPipelineBarrier2(cmd, &postDependency);
  impl_->historyValid = true;
  endPass("aa");

  // --- Glare: blends a heavy-tailed blur of the output back into it, before
  // the exposure meter, which should see the glare as the eye does.
  if (impl_->bloom.intensity > 0.0f) {
    const BloomConstants bloomConstants{impl_->bloom.scatter,
                                        impl_->bloom.intensity};
    auto bloomPass = [&](VkPipeline pipeline, VkDescriptorSet set,
                         VkExtent2D size) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                              impl_->bloomLayout, 0, 1, &set, 0, nullptr);
      vkCmdPushConstants(cmd, impl_->bloomLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(bloomConstants), &bloomConstants);
      vkCmdDispatch(cmd, (size.width + 7) / 8, (size.height + 7) / 8, 1);
      vkCmdPipelineBarrier2(cmd, &postDependency);
    };
    const VkExtent2D chain = impl_->bloomExtent;
    auto levelSize = [&](size_t level) {
      return VkExtent2D{std::max(chain.width >> level, 1u),
                        std::max(chain.height >> level, 1u)};
    };
    const size_t levels = impl_->bloomLevels.size();
    for (size_t level = 0; level < levels; ++level) {
      bloomPass(impl_->bloomDownPipeline, impl_->bloomDownSets[level],
                levelSize(level));
    }
    for (size_t level = levels - 1; level-- > 0;) {
      bloomPass(impl_->bloomUpPipeline, impl_->bloomUpSets[level],
                levelSize(level));
    }
    bloomPass(impl_->bloomCompositePipeline, impl_->bloomCompositeSet, extent);
  }
  endPass("bloom");

  const VkDescriptorSet postSet = impl_->postSet;
  PostConstants postConstants;
  postConstants.deltaSeconds = deltaSeconds;
  postConstants.histogram = impl_->histogramBuffer.address;
  postConstants.exposure = impl_->exposureBuffer.address;
  postConstants.exposureCompensation = camera.exposureCompensation;

  // --- Auto-exposure: histogram of the resolved frame, then adaptation.
  // The new exposure is used by the next frame's scene pass.

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    impl_->histogramPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          impl_->postLayout, 0, 1, &postSet, 0, nullptr);
  vkCmdPushConstants(cmd, impl_->postLayout,
                     VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(postConstants), &postConstants);
  // One thread per 2x2 pixels (see histogram.comp).
  vkCmdDispatch(cmd, (extent.width + 31) / 32, (extent.height + 31) / 32, 1);
  vkCmdPipelineBarrier2(cmd, &postDependency);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    impl_->exposurePipeline);
  vkCmdDispatch(cmd, 1, 1, 1);
  vkCmdPipelineBarrier2(cmd, &postDependency);
  endPass("exposure");

  // --- Tonemap pass: resolved HDR -> swapchain image, through the grade
  // LUT, which is rebaked first when the grade changed.

  Buffer gradeReadback;
  if (impl_->gradeDirty) {
    // The LUT and its parameters are shared by both frames in flight: the
    // previous frame's tonemap and bake must be done with them.
    memoryBarrier(cmd,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT |
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const GpuGrade gpuGrade = toGpu(impl_->grade);
    vkCmdUpdateBuffer(cmd, impl_->gradeBuffer.buffer, 0, sizeof(gpuGrade),
                      &gpuGrade);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    const GradeConstants gradeConstants{impl_->gradeBuffer.address};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      impl_->gradeLutPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            impl_->gradeLayout, 0, 1, &postSet, 0, nullptr);
    vkCmdPushConstants(cmd, impl_->gradeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(gradeConstants), &gradeConstants);
    constexpr uint32_t kGroups = (kGradeLutSize + 3) / 4;
    vkCmdDispatch(cmd, kGroups, kGroups, kGroups);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                      VK_ACCESS_2_TRANSFER_READ_BIT);
    impl_->gradeDirty = false;

    if (kEnableValidation) {
      gradeReadback = impl_->createBuffer(
          VkDeviceSize{kGradeLutSize} * kGradeLutSize * kGradeLutSize * 8,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
              VMA_ALLOCATION_CREATE_MAPPED_BIT);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {kGradeLutSize, kGradeLutSize, kGradeLutSize};
      vkCmdCopyImageToBuffer(cmd, impl_->gradeLut.image, VK_IMAGE_LAYOUT_GENERAL,
                             gradeReadback.buffer, 1, &region);
    }
  }

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
  const VkViewport displayViewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                   static_cast<float>(extent.height), 0.0f, 1.0f};
  const VkRect2D displayScissor{{0, 0}, extent};
  vkCmdSetViewport(cmd, 0, 1, &displayViewport);
  vkCmdSetScissor(cmd, 0, 1, &displayScissor);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  endPass("tonemap");

  if (impl_->overlayPending) {
    impl_->overlayPending = false;
    ImGui::Render();
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    swapchainAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    vkCmdBeginRendering(cmd, &tonemapRendering);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
    endPass("overlay");
  } else {
    ImGui::GetIO().ClearEventsQueue();
  }
  impl_->slotPassCount[frameSlot] = passCount;

  // Optional capture: copy the finished frame into a CPU-readable buffer.
  // A .pfm capture takes the linear HDR image the tonemap pass read instead.
  Buffer captureBuffer;
  const bool capturing = impl_->capturePath.has_value();
  const bool hdrCapture =
      capturing && impl_->capturePath->extension() == ".pfm";
  if (capturing) {
    captureBuffer = impl_->createBuffer(
        VkDeviceSize{extent.width} * extent.height * (hdrCapture ? 8 : 4),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }
  if (hdrCapture) {
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, impl_->output.image, VK_IMAGE_LAYOUT_GENERAL,
                           captureBuffer.buffer, 1, &region);
  }
  if (capturing && !hdrCapture) {
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

  if (gradeReadback.buffer != VK_NULL_HANDLE) {
    check(vkWaitForFences(dev, 1, &frame.renderFence, VK_TRUE, UINT64_MAX),
          "vkWaitForFences");
    check(vmaInvalidateAllocation(impl_->allocator, gradeReadback.allocation, 0,
                                  VK_WHOLE_SIZE),
          "vmaInvalidateAllocation");
    const float difference = gradeLutDifference(
        impl_->grade, static_cast<const uint16_t *>(gradeReadback.mapped));
    impl_->destroyBuffer(gradeReadback);
    std::printf("grade LUT check: max diff %g\n", difference);
    if (!(difference <= 2e-3f)) {
      throw std::runtime_error("GPU grade LUT differs from the CPU reference");
    }
  }

  if (capturing) {
    check(vkWaitForFences(dev, 1, &frame.renderFence, VK_TRUE, UINT64_MAX),
          "vkWaitForFences");
    check(vmaInvalidateAllocation(impl_->allocator, captureBuffer.allocation, 0,
                                  VK_WHOLE_SIZE),
          "vmaInvalidateAllocation");
    bool saved = false;
    if (hdrCapture) {
      saved = savePfm(*impl_->capturePath, extent,
                      static_cast<const uint16_t *>(captureBuffer.mapped));
    } else {
      // The swapchain is BGRA8 sRGB, already display-encoded; PNG wants RGBA.
      basisu::image png(extent.width, extent.height);
      const auto *bgra = static_cast<const uint8_t *>(captureBuffer.mapped);
      for (uint32_t i = 0; i < extent.width * extent.height; ++i) {
        png.get_ptr()[i].set(bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4],
                             255);
      }
      saved = basisu::save_png(impl_->capturePath->string(), png);
    }
    if (!saved) {
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
