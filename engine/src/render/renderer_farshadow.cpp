// The far shadow map (farfield.h, farfield_shadow.comp): the far receivers'
// sun shadow, an orthographic depth map of the bricks along the sun. The
// first map is built in one go on the first frame; when the sun turns, a
// second map is rebuilt a slice of texels per frame and the two swap on
// completion, so no frame stalls (farShadowStep decides each frame).
#include "renderer_impl.h"

#include <meridian/farfield.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace meridian {
namespace {

constexpr double kMiB = 1.0 / (1 << 20);
// Workgroups per dispatch (x at most 65535 guaranteed), 64 words each.
constexpr uint32_t kWordsPerDispatch = 65535 * 64;
// Canopy voxels reach past the cells' recorded extent (farCanopyPad).
constexpr float kBoxPad = 16.0f;

} // namespace

VkDeviceSize Renderer::Impl::createFarShadow(const FarFieldGrid &grid,
                                             const std::vector<FarCell> &farCells) {
  Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (uint32_t c = 0; c < farCells.size(); ++c) {
    if (farCells[c].bottom > farCells[c].top) {
      continue;
    }
    const float x = grid.originX + static_cast<float>(c % grid.cellsX) * kCellSize;
    const float z = grid.originZ + static_cast<float>(c / grid.cellsX) * kCellSize;
    lo = {std::min(lo.x, x), std::min(lo.y, farCells[c].bottom - kBoxPad), std::min(lo.z, z)};
    hi = {std::max(hi.x, x + kCellSize), std::max(hi.y, farCells[c].top + kBoxPad),
          std::max(hi.z, z + kCellSize)};
  }
  if (lo.x > hi.x) {
    return 0;
  }
  farShadowBox = {lo, hi};
  farShadowCapacity = farShadowMaxTexels(farShadowBox);
  for (Buffer &map : farShadowMaps) {
    map = createBuffer(VkDeviceSize{farShadowCapacity} * 2,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       0);
  }
  const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FarShadowConstants)};
  VkPipelineLayoutCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  info.pushConstantRangeCount = 1;
  info.pPushConstantRanges = &range;
  check(vkCreatePipelineLayout(device.device, &info, nullptr, &farShadowLayout),
        "vkCreatePipelineLayout");
  farShadowPipeline = createComputePipeline(device.device, "farfield_shadow.comp", farShadowLayout);
  farShadowReady = true;
  const FarShadowGrid sample = farShadowGrid(farShadowBox, normalize({0.5f, 0.55f, -0.4f}),
                                             farShadowCapacity);
  std::printf("Far shadow map: box %.0f x %.0f x %.0f m, up to %u texels (%.1f MiB per map, "
              "2 maps), %.0f m texels at the default sun (%ux%u)\n",
              hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, farShadowCapacity,
              farShadowMaps[0].size * kMiB, sample.texel, sample.width, sample.height);
  return farShadowMaps[0].size * 2;
}

void Renderer::Impl::destroyFarShadow() {
  for (Buffer &map : farShadowMaps) {
    destroyBuffer(map);
  }
  if (farShadowPipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(device.device, farShadowPipeline, nullptr);
    vkDestroyPipelineLayout(device.device, farShadowLayout, nullptr);
    farShadowPipeline = VK_NULL_HANDLE;
    farShadowLayout = VK_NULL_HANDLE;
  }
  farShadowReady = false;
  farShadowState = {};
  farShadowBuilt = 0;
  farShadowPending = {};
}

// Texels [first, first + count) of map `index` (even), in dispatches of at
// most kWordsPerDispatch words.
void Renderer::Impl::dispatchFarShadow(VkCommandBuffer cmd, uint32_t index, uint32_t first,
                                       uint32_t count) {
  const FarShadowGrid &grid = farShadowGrids[index];
  FarShadowConstants constants;
  constants.field = farFieldInfo.address;
  constants.texels = farShadowMaps[index].address;
  const Vec3 origin = grid.u * grid.minU + grid.v * grid.minV + grid.d * grid.minD;
  const auto copy = [](float (&to)[3], Vec3 from) {
    to[0] = from.x;
    to[1] = from.y;
    to[2] = from.z;
  };
  copy(constants.origin, origin);
  copy(constants.u, grid.u);
  copy(constants.v, grid.v);
  copy(constants.d, grid.d);
  constants.texel = grid.texel;
  constants.depth = grid.depth;
  constants.width = grid.width;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, farShadowPipeline);
  for (uint32_t word = first / 2; word < (first + count) / 2; word += kWordsPerDispatch) {
    constants.firstWord = word;
    constants.wordCount = std::min(kWordsPerDispatch, (first + count) / 2 - word);
    vkCmdPushConstants(cmd, farShadowLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                       &constants);
    vkCmdDispatch(cmd, (constants.wordCount + 63) / 64, 1, 1);
  }
}

void Renderer::Impl::updateFarShadow(Vec3 sun, SceneData &scene) {
  farShadowPending = {};
  if (!farShadowReady || scene.voxelFarShadows == 0) {
    return;
  }
  const FarShadowGrid grid = farShadowGrid(farShadowBox, sun, farShadowCapacity);
  const uint32_t total = farShadowTexelCount(grid);
  if (!farShadowState.hasBuilt) {
    const uint32_t target = 1 - farShadowBuilt;
    farShadowGrids[target] = grid;
    const auto start = std::chrono::steady_clock::now();
    submitNow([&](VkCommandBuffer cmd) {
      dispatchFarShadow(cmd, target, 0, total);
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT);
    });
    farShadowState = farShadowStep(farShadowState, sun, total, total).state;
    farShadowBuilt = target;
    std::printf("Far shadow map: first map %ux%u texels (%.0f m, %.1f MiB used) built in "
                "%.0f ms\n",
                grid.width, grid.height, grid.texel, total * 2 * kMiB,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
  }
  const FarShadowStep step = farShadowStep(farShadowState, sun, total, kFarShadowTexelBudget);
  farShadowState = step.state;
  if (step.count > 0) {
    if (step.first == 0) {
      farShadowGrids[1 - farShadowBuilt] = grid;
    }
    farShadowPending = step;
  }
  if (step.useMap) {
    const FarShadowGrid &built = farShadowGrids[farShadowBuilt];
    scene.farShadowTexels = farShadowMaps[farShadowBuilt].address;
    const auto copy = [](float (&to)[3], Vec3 from) {
      to[0] = from.x;
      to[1] = from.y;
      to[2] = from.z;
    };
    copy(scene.farShadowU, built.u);
    copy(scene.farShadowV, built.v);
    copy(scene.farShadowD, built.d);
    scene.farShadowMinU = built.minU;
    scene.farShadowMinV = built.minV;
    scene.farShadowMinD = built.minD;
    scene.farShadowTexel = built.texel;
    scene.farShadowDepth = built.depth;
    scene.farShadowWidth = built.width;
    scene.farShadowHeight = built.height;
    scene.farShadowMode = 1;
  }
}

void Renderer::Impl::recordFarShadow(VkCommandBuffer cmd) {
  if (farShadowPending.count == 0) {
    return;
  }
  const uint32_t target = 1 - farShadowBuilt;
  // The target held the map frames in flight may still read (the previous
  // build's replaced map).
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  dispatchFarShadow(cmd, target, farShadowPending.first, farShadowPending.count);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  if (farShadowPending.completes) {
    farShadowBuilt = target;
  }
  farShadowPending = {};
}

// Debug: sample texels of a map built along two suns against the CPU
// replica on the same bricks.
void Renderer::Impl::checkFarShadow(const FarFieldCpu &cpu) {
  if (!farShadowReady) {
    return;
  }
  const Vec3 suns[] = {normalize({0.9f, 0.05f, -0.4f}), normalize({0.3f, 0.8f, 0.2f})};
  uint32_t compared = 0, hits = 0, emptyMismatches = 0;
  uint64_t stepCaps = 0;
  float largest = 0.0f;
  for (const Vec3 &sun : suns) {
    const FarShadowGrid grid = farShadowGrid(farShadowBox, sun, farShadowCapacity);
    farShadowGrids[1] = grid;
    const uint32_t total = farShadowTexelCount(grid);
    Buffer readback = createBuffer(VkDeviceSize{total} * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                       VMA_ALLOCATION_CREATE_MAPPED_BIT);
    submitNow([&](VkCommandBuffer cmd) {
      dispatchFarShadow(cmd, 1, 0, total);
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
      const VkBufferCopy copy{0, 0, VkDeviceSize{total} * 2};
      vkCmdCopyBuffer(cmd, farShadowMaps[1].buffer, readback.buffer, 1, &copy);
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    });
    check(vmaInvalidateAllocation(allocator, readback.allocation, 0, VK_WHOLE_SIZE),
          "vmaInvalidateAllocation");
    const auto *texels = static_cast<const uint16_t *>(readback.mapped);
    for (uint32_t i = 0; i < grid.width * grid.height; ++i) {
      stepCaps += texels[i] == kFarShadowStepCap ? 1 : 0;
    }
    // Mostly texels over forest: a stride through the map plus a random
    // sample of the lot.
    for (uint32_t s = 0; s < 4096; ++s) {
      const uint32_t index = placementHash(s, 0x7368, 0) % (grid.width * grid.height);
      const uint32_t x = index % grid.width, y = index / grid.width;
      const Vec3 origin = grid.u * (grid.minU + (static_cast<float>(x) + 0.5f) * grid.texel) +
                          grid.v * (grid.minV + (static_cast<float>(y) + 0.5f) * grid.texel) +
                          grid.d * grid.minD;
      const float expected =
          farFieldShadowDistanceCpu(cpu, kFarShadowLevel, origin, grid.d, grid.depth);
      const uint32_t q = texels[index];
      ++compared;
      if ((q >= kFarShadowStepCap) != (expected < 0.0f)) {
        ++emptyMismatches;
        continue;
      }
      if (expected >= 0.0f) {
        ++hits;
        largest = std::max(largest, std::abs(dequantizeFarShadow(q, grid.depth) - expected));
      }
    }
    destroyBuffer(readback);
  }
  std::printf("far shadow map check: %u texels (%u with a hit) compared with the CPU replica, "
              "max difference %.3g m, %u hit/empty mismatches, %llu texels out of steps\n",
              compared, hits, largest, emptyMismatches, static_cast<unsigned long long>(stepCaps));
  if (largest > 1.0f || emptyMismatches > compared / 100 || stepCaps > 0) {
    throw std::runtime_error("GPU far shadow map differs from the CPU replica");
  }
}

} // namespace meridian
