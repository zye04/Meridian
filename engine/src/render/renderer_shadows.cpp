// Raster sun shadows: cascaded depth maps of the placed and hand-placed
// geometry, rendered through the cluster cull chain from the sun and cached
// until the camera leaves a cascade's window, the sun moves or the geometry
// changes. See docs/superpowers/specs/2026-11-20-raster-sun-shadows-design.md.
#include "renderer_impl.h"

#include <cmath>
#include <cstring>

namespace meridian {
namespace {

// The highest world y that casts a shadow into a cascade: terrain relief
// plus the tallest placed object.
constexpr double kCasterTop = 2500.0;
// Reverse depth: a negative slope bias pushes the casters away from the sun.
constexpr float kShadowDepthBiasConstant = 0.0f;
constexpr float kShadowDepthBiasSlope = -1.5f;

DVec3 toDouble(Vec3 v) { return {v.x, v.y, v.z}; }

void toFloat3(float out[3], DVec3 v) {
  out[0] = static_cast<float>(v.x);
  out[1] = static_cast<float>(v.y);
  out[2] = static_cast<float>(v.z);
}

} // namespace

void Renderer::setShadowMode(ShadowMode mode) { impl_->shadowMode = mode; }

uint32_t Renderer::sunShadowRenderCount() const { return impl_->shadowRenderCount; }

void Renderer::Impl::createSunShadows() {
  VkDevice dev = device.device;
  if (textures.size() + kShadowCascades > kMaxTextures) {
    throw std::runtime_error("texture table is full");
  }
  for (uint32_t i = 0; i < kShadowCascades; ++i) {
    shadowMaps[i] = createImage(VK_FORMAT_D32_SFLOAT, {kShadowMapSize, kShadowMapSize, 1},
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
    shadowTextures[i] = static_cast<uint32_t>(textures.size());
    textures.emplace_back();
    writeTexture(shadowTextures[i], shadowMaps[i].view, nearestSampler,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  for (auto &slot : shadowScenes) {
    for (Buffer &buffer : slot) {
      buffer = createBuffer(sizeof(SceneData),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }
  }
  shadowCounters = createBuffer(
      sizeof(DrawCounters),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      0);
  shadowDrawLists = createBuffer(VkDeviceSize{kDrawBuckets} * kMaxDrawnClusters * 2 *
                                     sizeof(uint32_t),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                 0);
  PipelineDesc desc{.meshShader = "shadow.mesh",
                    .layout = sceneLayout,
                    .depthFormat = kDepthFormat,
                    .dynamicDepthBias = true};
  shadowPipeline = createGraphicsPipeline(dev, desc);
  desc.fragmentShader = "shadow_masked.frag";
  shadowMaskedPipeline = createGraphicsPipeline(dev, desc);
  desc.meshShader = "voxel_shadow.mesh";
  desc.fragmentShader = nullptr;
  voxelShadowPipeline = createGraphicsPipeline(dev, desc);
}

void Renderer::Impl::destroySunShadows() {
  VkDevice dev = device.device;
  vkDestroyPipeline(dev, voxelShadowPipeline, nullptr);
  vkDestroyPipeline(dev, shadowMaskedPipeline, nullptr);
  vkDestroyPipeline(dev, shadowPipeline, nullptr);
  destroyBuffer(shadowDrawLists);
  destroyBuffer(shadowCounters);
  for (auto &slot : shadowScenes) {
    for (Buffer &buffer : slot) {
      destroyBuffer(buffer);
    }
  }
  for (Image &image : shadowMaps) {
    destroyImage(image);
  }
}

// Every cascade that needs it is rendered this frame, so each one always
// covers the camera.
void Renderer::Impl::planSunShadows(SceneData &scene, uint32_t frameSlot, Vec3 camera,
                                    Vec3 sunDirection) {
  shadowPlanned.fill(false);
  scene.shadowMode = shadowMode == ShadowMode::Rt ? 1 : 0;
  if (shadowMode == ShadowMode::Rt) {
    shadowRendered.fill(false);
    return;
  }
  const bool sunMoved = sunDirection.x != shadowSun.x || sunDirection.y != shadowSun.y ||
                        sunDirection.z != shadowSun.z;
  const bool full = shadowStale || sunMoved;
  shadowSun = sunDirection;
  shadowStale = false;
  shadowBasis = lightBasis(sunDirection);
  const DVec3 eye = toDouble(camera);
  GpuSunShadows &out = scene.sunShadows;
  toFloat3(out.u, shadowBasis.u);
  toFloat3(out.v, shadowBasis.v);
  toFloat3(out.d, shadowBasis.d);
  for (uint32_t i = 0; i < kShadowCascades; ++i) {
    if (full || !shadowRendered[i] || needsRecenter(i, shadowWindows[i], shadowBasis, eye)) {
      shadowWindows[i] = cascadeWindow(i, shadowBasis, eye, kCasterTop, sunDirection);
      shadowRendered[i] = true;
      shadowPlanned[i] = true;
      ++shadowRenderCount;

      const CascadeWindow &window = shadowWindows[i];
      SceneData copy = scene;
      copy.cameraPosition = {static_cast<float>(window.center.x),
                             static_cast<float>(window.center.y),
                             static_cast<float>(window.center.z)};
      const auto planes = cascadePlanes(window, shadowBasis);
      for (size_t p = 0; p < 6; ++p) {
        std::copy(planes[p].begin(), planes[p].end(), copy.frustumPlanes[p]);
      }
      copy.viewProjection = cascadeProjection(window, shadowBasis);
      copy.drawLists = shadowDrawLists.address;
      copy.shadowView = 1;
      copy.orthoPixelsPerMeter = static_cast<float>(1.0 / window.texel);
      copy.lodErrorPixels = 1.0f;
      copy.shadowDrawMargin =
          static_cast<float>(0.5 * (cascadeHalfSize(i) - cascadeRadius(i)) * std::sqrt(3.0));
      Buffer &buffer = shadowScenes[frameSlot][i];
      std::memcpy(buffer.mapped, &copy, sizeof(copy));
      check(vmaFlushAllocation(allocator, buffer.allocation, 0, VK_WHOLE_SIZE),
            "vmaFlushAllocation");
    }
    const CascadeWindow &window = shadowWindows[i];
    GpuSunShadowCascade &cascade = out.cascades[i];
    toFloat3(cascade.origin, window.center - eye);
    cascade.halfSize = static_cast<float>(window.halfSize);
    cascade.texel = static_cast<float>(window.texel);
    cascade.depthNear = static_cast<float>(window.depthNear);
    cascade.depthFar = static_cast<float>(window.depthFar);
    cascade.texture = shadowTextures[i];
    cascade.radius = static_cast<float>(cascadeRadius(i));
    cascade.valid = 1;
  }
}

void Renderer::Impl::recordSunShadows(VkCommandBuffer cmd, const FrameData &frame) {
  const uint32_t frameSlot = static_cast<uint32_t>(&frame - frames.data());
  const VkExtent2D extent{kShadowMapSize, kShadowMapSize};
  VkViewport viewport{};
  viewport.width = viewport.height = static_cast<float>(kShadowMapSize);
  viewport.maxDepth = 1.0f;
  const VkRect2D scissor{{0, 0}, extent};
  for (uint32_t i = 0; i < kShadowCascades; ++i) {
    if (!shadowPlanned[i]) {
      continue;
    }
    const Buffer &sceneBuffer = shadowScenes[frameSlot][i];
    resetDrawCounters(cmd, shadowCounters);
    const ShadowCull cull{sceneBuffer.address, &shadowCounters};
    cullCells(cmd, frame, 1, &cull);
    cullInstances(cmd, frame, 1, &cull);
    expandVoxelBricks(cmd, frame, 1, &cull);
    traverseClusters(cmd, frame, 1, &cull);
    cullClusters(cmd, frame, 1, &cull);

    // Contents are cleared, so the old layout is discarded; earlier frames'
    // reads of the map finish first.
    transitionImage(cmd, {shadowMaps[i].image, VK_IMAGE_ASPECT_DEPTH_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT});
    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = shadowMaps[i].view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = scissor;
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(cmd, &rendering);
    const VkDescriptorSet sets[] = {atmosphereSet, materialSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout, 0, 2, sets, 0,
                            nullptr);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdSetDepthBias(cmd, kShadowDepthBiasConstant, 0.0f, kShadowDepthBiasSlope);
    for (uint32_t bucket = 0; bucket < kDrawBuckets; ++bucket) {
      PushConstants push;
      push.sceneBuffer = sceneBuffer.address;
      push.drawList = bucket;
      vkCmdPushConstants(cmd, sceneLayout, kScenePushStages, 0, sizeof(push), &push);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        bucket >= 2 ? shadowMaskedPipeline : shadowPipeline);
      dispatch.cmdDrawMeshTasksIndirectEXT(
          cmd, shadowCounters.buffer,
          offsetof(DrawCounters, meshTasks) + bucket * sizeof(DrawCounters::meshTasks[0][0]), 1,
          sizeof(DrawCounters::meshTasks[0][0]));
    }
    // Trees past the voxel switch cast as their voxels (phase 1's list; a
    // cascade has no switch band).
    if (protoVoxelInfo.buffer != VK_NULL_HANDLE) {
      PushConstants push;
      push.sceneBuffer = sceneBuffer.address;
      push.drawList = 2;
      vkCmdPushConstants(cmd, sceneLayout, kScenePushStages, 0, sizeof(push), &push);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, voxelShadowPipeline);
      dispatch.cmdDrawMeshTasksIndirectEXT(
          cmd, shadowCounters.buffer,
          offsetof(DrawCounters, voxelTasks) + sizeof(DrawCounters::voxelTasks[0]), 1,
          sizeof(DrawCounters::voxelTasks[0][0]));
    }
    vkCmdEndRendering(cmd);
    transitionImage(cmd, {shadowMaps[i].image, VK_IMAGE_ASPECT_DEPTH_BIT,
                          VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
  }
}

} // namespace meridian
