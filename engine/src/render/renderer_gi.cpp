#include "renderer_impl.h"

#include <meridian/gi.h>

namespace meridian {

void Renderer::setGiMode(GiMode mode) {
  impl_->giReset = impl_->giReset || mode != impl_->giMode;
  impl_->giMode = mode;
}

// Created on the first frame with GI on: the probe table starts zeroed
// (valid = 0), so every probe is traced fresh.
void Renderer::Impl::createGi() {
  constexpr VkDeviceSize kProbes =
      VkDeviceSize{kGiLevels} * kGiColumns * kGiColumns * kGiSlices;
  giBuffer = createBuffer(sizeof(GpuGiHeader) + kProbes * kGiProbeBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          0);
  submitNow([&](VkCommandBuffer cmd) {
    vkCmdFillBuffer(cmd, giBuffer.buffer, 0, VK_WHOLE_SIZE, 0);
  });
  giProbesPipeline = createComputePipeline(device.device, "gi_probes.comp", sceneLayout);
  giReset = true;
}

void Renderer::Impl::destroyGi() {
  if (giProbesPipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(device.device, giProbesPipeline, nullptr);
    giProbesPipeline = VK_NULL_HANDLE;
  }
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
  header.frame = frameIndex;
  header.reset = giReset ? 1 : 0;
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

} // namespace meridian
