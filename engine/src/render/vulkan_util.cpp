#include "vulkan_util.h"

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace meridian {

float halton(uint32_t index, uint32_t base) {
  float result = 0.0f;
  float fraction = 1.0f;
  while (index > 0) {
    fraction /= static_cast<float>(base);
    result += fraction * static_cast<float>(index % base);
    index /= base;
  }
  return result;
}

std::atomic<uint32_t> validationErrors{0};

VKAPI_ATTR VkBool32 VKAPI_CALL
debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
              VkDebugUtilsMessageTypeFlagsEXT,
              const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
  const bool error = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  if (error) {
    ++validationErrors;
  }
  std::fprintf(stderr, "[%s: Validation] %s\n", error ? "ERROR" : "WARNING",
               data->pMessage);
  return VK_FALSE;
}

void check(VkResult result, const char *what) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error(std::string(what) + " failed");
  }
}

void transitionImage(VkCommandBuffer cmd, const ImageBarrier &b) {
  VkImageMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  barrier.srcStageMask = b.srcStage;
  barrier.srcAccessMask = b.srcAccess;
  barrier.dstStageMask = b.dstStage;
  barrier.dstAccessMask = b.dstAccess;
  barrier.oldLayout = b.from;
  barrier.newLayout = b.to;
  barrier.image = b.image;
  barrier.subresourceRange.aspectMask = b.aspect;
  barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
  barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

  VkDependencyInfo dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dependency);
}

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage,
                   VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                   VkAccessFlags2 dstAccess) {
  VkMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  barrier.srcStageMask = srcStage;
  barrier.srcAccessMask = srcAccess;
  barrier.dstStageMask = dstStage;
  barrier.dstAccessMask = dstAccess;
  VkDependencyInfo dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dependency);
}

VkShaderModule loadShader(VkDevice device, const std::string &name) {
  std::string path = std::string(MERIDIAN_SHADER_DIR) + "/" + name + ".spv";
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error("cannot open shader " + path);
  }

  std::vector<uint32_t> code(static_cast<size_t>(file.tellg()) /
                             sizeof(uint32_t));
  file.seekg(0);
  file.read(reinterpret_cast<char *>(code.data()),
            static_cast<std::streamsize>(code.size() * sizeof(uint32_t)));

  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = code.size() * sizeof(uint32_t);
  info.pCode = code.data();

  VkShaderModule module = VK_NULL_HANDLE;
  check(vkCreateShaderModule(device, &info, nullptr, &module),
        "vkCreateShaderModule");
  return module;
}

VkPipeline createGraphicsPipeline(VkDevice device, const PipelineDesc &desc) {
  VkShaderModule vertModule = loadShader(device, desc.vertexShader);
  VkShaderModule fragModule = loadShader(device, desc.fragmentShader);

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertModule;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragModule;
  stages[1].pName = "main";

  // Vertices are read in the shader through a buffer address ("vertex
  // pulling"), so the fixed-function vertex input stays empty.
  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.cullMode = VK_CULL_MODE_NONE;
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Reverse-Z: nearer surfaces have larger depth values.
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  const bool depthTest =
      desc.depthTest && desc.depthFormat != VK_FORMAT_UNDEFINED;
  depthStencil.depthTestEnable = depthTest;
  depthStencil.depthWriteEnable = depthTest && !desc.alphaBlend;
  depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(
      desc.colorFormats.size(), blendAttachment);
  if (desc.alphaBlend) {
    VkPipelineColorBlendAttachmentState &color = blendAttachments[0];
    color.blendEnable = VK_TRUE;
    color.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    color.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    color.colorBlendOp = VK_BLEND_OP_ADD;
    color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    color.alphaBlendOp = VK_BLEND_OP_ADD;
    for (size_t i = 1; i < blendAttachments.size(); ++i) {
      blendAttachments[i].colorWriteMask = 0;
    }
  }

  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
  colorBlend.pAttachments = blendAttachments.data();

  // Viewport and scissor are set per frame, so a resize doesn't need a new
  // pipeline.
  VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                    VK_DYNAMIC_STATE_SCISSOR,
                                    VK_DYNAMIC_STATE_CULL_MODE};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = desc.dynamicCullMode ? 3 : 2;
  dynamicState.pDynamicStates = dynamicStates;

  VkPipelineRenderingCreateInfo renderingInfo{};
  renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingInfo.colorAttachmentCount =
      static_cast<uint32_t>(desc.colorFormats.size());
  renderingInfo.pColorAttachmentFormats = desc.colorFormats.data();
  renderingInfo.depthAttachmentFormat = desc.depthFormat;

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.pNext = &renderingInfo;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = stages;
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisample;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &colorBlend;
  pipelineInfo.pDynamicState = &dynamicState;
  pipelineInfo.layout = desc.layout;

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                              &pipelineInfo, nullptr, &pipeline);
  vkDestroyShaderModule(device, vertModule, nullptr);
  vkDestroyShaderModule(device, fragModule, nullptr);
  check(result, "vkCreateGraphicsPipelines");
  return pipeline;
}

VkPipeline createComputePipeline(VkDevice device, const char *shader,
                                 VkPipelineLayout layout) {
  VkShaderModule module = loadShader(device, shader);

  VkComputePipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = module;
  info.stage.pName = "main";
  info.layout = layout;

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info,
                                             nullptr, &pipeline);
  vkDestroyShaderModule(device, module, nullptr);
  check(result, "vkCreateComputePipelines");
  return pipeline;
}

} // namespace meridian
