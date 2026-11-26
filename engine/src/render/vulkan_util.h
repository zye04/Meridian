// Small Vulkan helpers shared by the renderer's source files.
#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <span>
#include <string>

namespace meridian {

// Low-discrepancy sequence used for sub-pixel jitter; returns [0, 1).
float halton(uint32_t index, uint32_t base);

// Counts validation errors (and prints every message, like vk-bootstrap's
// default messenger) so automated runs can fail on them.
extern std::atomic<uint32_t> validationErrors;
VKAPI_ATTR VkBool32 VKAPI_CALL
debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
              VkDebugUtilsMessageTypeFlagsEXT type,
              const VkDebugUtilsMessengerCallbackDataEXT *data, void *user);

void check(VkResult result, const char *what);

struct ImageBarrier {
  VkImage image;
  VkImageAspectFlags aspect;
  VkImageLayout from;
  VkImageLayout to;
  VkPipelineStageFlags2 srcStage;
  VkAccessFlags2 srcAccess;
  VkPipelineStageFlags2 dstStage;
  VkAccessFlags2 dstAccess;
};

void transitionImage(VkCommandBuffer cmd, const ImageBarrier &b);

// A global memory barrier: everything before (srcStage/srcAccess) happens
// and is visible before everything after (dstStage/dstAccess).
void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage,
                   VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                   VkAccessFlags2 dstAccess);

// Loads build/shaders/<name>.spv (see add_shader() in engine/CMakeLists.txt).
VkShaderModule loadShader(VkDevice device, const std::string &name);

struct PipelineDesc {
  // Exactly one of the two: a vertex shader, or a mesh shader (no task
  // shader, no vertex input).
  const char *vertexShader = nullptr;
  const char *meshShader = nullptr;
  // Null: depth only.
  const char *fragmentShader = nullptr;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  // Scene pipelines: 0 HDR, 1 motion, 2 transparency mask (alphaBlend
  // writes it, see createGraphicsPipeline), 3-5 Ray Reconstruction guides,
  // 6 sun light (opaque and masked only), 7 debug id (debug_id.glsl).
  std::span<const VkFormat> colorFormats;
  VkFormat depthFormat = VK_FORMAT_UNDEFINED;
  // Cull mode is set per draw (double-sided materials).
  bool dynamicCullMode = false;
  bool depthTest = true;
  // Premultiplied-alpha blending into the first attachment, no depth or
  // motion writes: the surface behind keeps its depth and motion.
  bool alphaBlend = false;
  // Depth bias set per draw (vkCmdSetDepthBias).
  bool dynamicDepthBias = false;
  // Masked prepass: depth only, written without colour. Masked shading:
  // tested EQUAL against the prepass, not written.
  bool noColorWrites = false;
  bool depthEqualNoWrite = false;
};

VkPipeline createGraphicsPipeline(VkDevice device, const PipelineDesc &desc);
VkPipeline createComputePipeline(VkDevice device, const char *shader,
                                 VkPipelineLayout layout);

} // namespace meridian
