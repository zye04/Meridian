#include "renderer_impl.h"

#include <basisu_enc.h>

#include <bit>
#include <cstring>

namespace meridian {

namespace {

constexpr const char *kViewNames[] = {"none", "base-color", "lod"};
static_assert(std::size(kViewNames) == size_t(DebugView::Count));

constexpr VkImageUsageFlags kDebugTargetUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                VK_IMAGE_USAGE_SAMPLED_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

// Must match Push in debug_view.frag.
struct DebugViewPush {
  VkDeviceAddress clusterLevels = 0;
  uint32_t view = 0;
  uint32_t pad = 0;
  uint32_t renderSize[2] = {};
  uint32_t targetSize[2] = {};
};
static_assert(sizeof(DebugViewPush) == 32);

} // namespace

const char *debugViewName(DebugView view) {
  return view < DebugView::Count ? kViewNames[size_t(view)] : "?";
}

std::optional<DebugView> parseDebugView(std::string_view name) {
  for (size_t i = 0; i < std::size(kViewNames); ++i) {
    if (name == kViewNames[i]) {
      return DebugView(i);
    }
  }
  return std::nullopt;
}

const char *pickKindName(PickKind kind) {
  switch (kind) {
  case PickKind::Sky: return "sky";
  case PickKind::Triangles: return "triangles";
  case PickKind::Voxels: return "voxels";
  case PickKind::FarCell: return "far cell";
  case PickKind::Terrain: return "terrain";
  }
  return "?";
}

void Renderer::Impl::updateDebugTargets() {
  const bool want = debugTargetsWanted();
  const VkDeviceSize levelBytes = std::max<VkDeviceSize>(4, (clusterLevels.size() + 3) / 4 * 4);
  if (want == (debugId.image != VK_NULL_HANDLE) &&
      (!want || debugClusterLevels.size == levelBytes)) {
    return;
  }
  VkDevice dev = device.device;
  check(vkDeviceWaitIdle(dev), "vkDeviceWaitIdle");
  destroyDebugTargets();
  if (!want) {
    return;
  }
  destroyScenePipelines();
  createScenePipelines(true);
  debugPipelines = true;
  debugId = createRenderImage(kDebugIdFormat, kDebugTargetUsage, VK_IMAGE_ASPECT_COLOR_BIT);
  if (diffuseAlbedo.image == VK_NULL_HANDLE) {
    debugAlbedo = createRenderImage(kAlbedoFormat, kDebugTargetUsage, VK_IMAGE_ASPECT_COLOR_BIT);
  }
  debugClusterLevels = createBuffer(levelBytes, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                        VMA_ALLOCATION_CREATE_MAPPED_BIT);
  std::memset(debugClusterLevels.mapped, 0, levelBytes);
  std::memcpy(debugClusterLevels.mapped, clusterLevels.data(), clusterLevels.size());
  check(vmaFlushAllocation(allocator, debugClusterLevels.allocation, 0, VK_WHOLE_SIZE),
        "vmaFlushAllocation");

  VkDescriptorSetLayoutBinding bindings[2]{};
  for (uint32_t b = 0; b < 2; ++b) {
    bindings[b].binding = b;
    bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[b].descriptorCount = 1;
    bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 2;
  layoutInfo.pBindings = bindings;
  check(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &debugSetLayout),
        "vkCreateDescriptorSetLayout");
  const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &poolSize;
  check(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &debugPool), "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc.descriptorPool = debugPool;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &debugSetLayout;
  check(vkAllocateDescriptorSets(dev, &alloc, &debugSet), "vkAllocateDescriptorSets");
  const VkImageView albedoView =
      diffuseAlbedo.image != VK_NULL_HANDLE ? diffuseAlbedo.view : debugAlbedo.view;
  const VkDescriptorImageInfo infos[2] = {
      {nearestSampler, debugId.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {nearestSampler, albedoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
  VkWriteDescriptorSet writes[2]{};
  for (uint32_t b = 0; b < 2; ++b) {
    writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[b].dstSet = debugSet;
    writes[b].dstBinding = b;
    writes[b].descriptorCount = 1;
    writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[b].pImageInfo = &infos[b];
  }
  vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);

  const VkPushConstantRange pushRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DebugViewPush)};
  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &debugSetLayout;
  pipelineLayoutInfo.pushConstantRangeCount = 1;
  pipelineLayoutInfo.pPushConstantRanges = &pushRange;
  check(vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &debugLayout),
        "vkCreatePipelineLayout");
  const VkFormat targetFormat[] = {swapchain.image_format};
  debugPipeline = createGraphicsPipeline(dev, {.vertexShader = "fullscreen.vert",
                                               .fragmentShader = "debug_view.frag",
                                               .layout = debugLayout,
                                               .colorFormats = targetFormat,
                                               .depthTest = false});
}

void Renderer::Impl::destroyDebugTargets() {
  VkDevice dev = device.device;
  if (debugPipelines) {
    destroyScenePipelines();
    createScenePipelines(false);
    debugPipelines = false;
  }
  vkDestroyPipeline(dev, debugPipeline, nullptr);
  vkDestroyPipelineLayout(dev, debugLayout, nullptr);
  vkDestroyDescriptorPool(dev, debugPool, nullptr);
  vkDestroyDescriptorSetLayout(dev, debugSetLayout, nullptr);
  debugPipeline = VK_NULL_HANDLE;
  debugLayout = VK_NULL_HANDLE;
  debugPool = VK_NULL_HANDLE;
  debugSet = VK_NULL_HANDLE;
  debugSetLayout = VK_NULL_HANDLE;
  if (debugClusterLevels.buffer != VK_NULL_HANDLE) {
    destroyBuffer(debugClusterLevels);
  }
  debugClusterLevels = {};
  destroyImage(debugAlbedo);
  destroyImage(debugId);
}

void Renderer::Impl::drawDebugView(VkCommandBuffer cmd, DebugView view, VkExtent2D extent) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, debugPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, debugLayout, 0, 1, &debugSet,
                          0, nullptr);
  const VkViewport viewport{0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f};
  const VkRect2D scissor{{0, 0}, extent};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  DebugViewPush push;
  push.clusterLevels = debugClusterLevels.address;
  push.view = uint32_t(view);
  push.renderSize[0] = renderExtent.width;
  push.renderSize[1] = renderExtent.height;
  push.targetSize[0] = extent.width;
  push.targetSize[1] = extent.height;
  vkCmdPushConstants(cmd, debugLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
  vkCmdDraw(cmd, 3, 1, 0, 0);
}

void Renderer::setDebugView(DebugView view) { impl_->debugView = view; }

DebugView Renderer::debugView() const { return impl_->debugView; }

void Renderer::keepDebugTargets(bool keep) { impl_->debugTargetsKept = keep; }

uint32_t Renderer::renderWidth() const { return impl_->renderExtent.width; }

uint32_t Renderer::renderHeight() const { return impl_->renderExtent.height; }

void Renderer::setMeshName(MeshId mesh, std::string name) {
  if (impl_->meshNames.size() <= mesh.index) {
    impl_->meshNames.resize(mesh.index + 1);
  }
  impl_->meshNames[mesh.index] = std::move(name);
}

void Renderer::setMaterialName(MaterialId material, std::string name) {
  if (impl_->materialNames.size() <= material.index) {
    impl_->materialNames.resize(material.index + 1);
  }
  impl_->materialNames[material.index] = std::move(name);
}

std::optional<PixelPick> Renderer::pick(uint32_t x, uint32_t y) {
  Impl &r = *impl_;
  if (r.debugId.image == VK_NULL_HANDLE || x >= r.renderExtent.width ||
      y >= r.renderExtent.height) {
    return std::nullopt;
  }
  check(vkDeviceWaitIdle(r.device.device), "vkDeviceWaitIdle");
  const Image &albedo = r.diffuseAlbedo.image != VK_NULL_HANDLE ? r.diffuseAlbedo : r.debugAlbedo;
  Buffer readback = r.createBuffer(32, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                       VMA_ALLOCATION_CREATE_MAPPED_BIT);
  r.submitNow([&](VkCommandBuffer cmd) {
    VkDeviceSize offset = 0;
    for (VkImage image : {r.debugId.image, albedo.image}) {
      transitionImage(cmd, {image, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT});
      VkBufferImageCopy region{};
      region.bufferOffset = offset;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageOffset = {int32_t(x), int32_t(y), 0};
      region.imageExtent = {1, 1, 1};
      vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer,
                             1, &region);
      transitionImage(cmd, {image, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0});
      offset += 16;
    }
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  });
  check(vmaInvalidateAllocation(r.allocator, readback.allocation, 0, VK_WHOLE_SIZE),
        "vmaInvalidateAllocation");
  uint32_t id[4];
  uint8_t rgba[4];
  std::memcpy(id, readback.mapped, sizeof(id));
  std::memcpy(rgba, static_cast<const uint8_t *>(readback.mapped) + 16, sizeof(rgba));
  r.destroyBuffer(readback);

  PixelPick pick;
  pick.x = x;
  pick.y = y;
  pick.kind = PickKind(id[0] >> 24);
  pick.instance = id[0] & 0xFFFFFFu;
  pick.placed = pick.instance >= kPlacedBase;
  pick.distance = std::bit_cast<float>(id[2]);
  for (int c = 0; c < 3; ++c) {
    pick.baseColor[c] = rgba[c] / 255.0f;
  }
  if (pick.kind == PickKind::Triangles || pick.kind == PickKind::Voxels) {
    pick.cluster = id[1];
    if (id[1] < r.clusterLevels.size()) {
      pick.lodLevel = r.clusterLevels[id[1]];
      pick.mesh = r.clusterMeshes[id[1]];
    }
  } else if (pick.kind == PickKind::FarCell) {
    pick.farCell = id[1] & 0x3FFFFFFFu;
    pick.lodLevel = id[1] >> 30;
  }
  if (pick.mesh) {
    pick.material = id[3] != ~0u ? id[3] : r.meshes[*pick.mesh].materialIndex;
    if (*pick.mesh < r.meshNames.size()) {
      pick.meshName = r.meshNames[*pick.mesh];
    }
  }
  if (pick.material && *pick.material < r.materialNames.size()) {
    pick.materialName = r.materialNames[*pick.material];
  }
  return pick;
}

bool Renderer::saveDebugView(DebugView view, const std::filesystem::path &path) {
  Impl &r = *impl_;
  if (r.debugId.image == VK_NULL_HANDLE || view == DebugView::None || view >= DebugView::Count) {
    return false;
  }
  check(vkDeviceWaitIdle(r.device.device), "vkDeviceWaitIdle");
  const VkExtent2D extent = r.swapchain.extent;
  Image target = r.createDisplayImage(r.swapchain.image_format,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                      VK_IMAGE_ASPECT_COLOR_BIT);
  Buffer readback = r.createBuffer(VkDeviceSize{extent.width} * extent.height * 4,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                       VMA_ALLOCATION_CREATE_MAPPED_BIT);
  r.submitNow([&](VkCommandBuffer cmd) {
    transitionImage(cmd, {target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_NONE, 0,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
    VkRenderingAttachmentInfo attachment{};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = target.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(cmd, &rendering);
    r.drawDebugView(cmd, view, extent);
    vkCmdEndRendering(cmd);
    transitionImage(cmd, {target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT});
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1, &region);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  });
  check(vmaInvalidateAllocation(r.allocator, readback.allocation, 0, VK_WHOLE_SIZE),
        "vmaInvalidateAllocation");
  // The target has the swapchain's format: BGRA8 sRGB; PNG wants RGBA.
  basisu::image png(extent.width, extent.height);
  const auto *bgra = static_cast<const uint8_t *>(readback.mapped);
  for (uint32_t i = 0; i < extent.width * extent.height; ++i) {
    png.get_ptr()[i].set(bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4], 255);
  }
  const bool saved = basisu::save_png(path.string(), png);
  r.destroyBuffer(readback);
  r.destroyImage(target);
  return saved;
}

} // namespace meridian
