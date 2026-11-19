#include <meridian/renderer.h>
#include <meridian/window.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/lsan_interface.h>
#define MERIDIAN_HAS_LSAN 1
#endif
#endif

#include <algorithm>
#include <array>
#include <optional>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace meridian {

namespace {

// Two frames in flight: the CPU records frame N+1 while the GPU renders N.
constexpr uint32_t kFramesInFlight = 2;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
// 16-bit float keeps smooth gradients across the full HDR range.
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Screen-space motion vectors for temporal anti-aliasing (and later DLSS).
constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
// Halton(2,3) jitter cycle length.
constexpr uint32_t kJitterPhases = 8;
// Exposure for the very first frame, before any histogram exists: a sunny day.
constexpr float kInitialEv100 = 15.0f;
// Capacities of the bindless material and texture tables.
constexpr uint32_t kMaxMaterials = 16384;
constexpr uint32_t kMaxTextures = 16384;
constexpr uint32_t kDfgLutSize = 128;
// Upper bound on draw items per frame, sizing each frame's TLAS.
constexpr uint32_t kMaxInstances = 4096;

#ifdef NDEBUG
constexpr bool kEnableValidation = false;
#else
constexpr bool kEnableValidation = true;
#endif

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VkDeviceAddress address = 0;
  void *mapped = nullptr;
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VkImageView view = VK_NULL_HANDLE;
};

struct AccelerationStructure {
  VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
  Buffer buffer;
  VkDeviceAddress address = 0;
};

struct GpuMesh {
  Buffer vertices;
  Buffer indices;
  uint32_t indexCount = 0;
  uint32_t materialIndex = 0;
  AlphaMode alphaMode = AlphaMode::Opaque;
  bool doubleSided = false;
  // Bottom-level acceleration structure: the mesh's triangles for ray tracing.
  AccelerationStructure blas;
};

// Must match AtmosphereParameters in common.glsl (std430).
struct GpuAtmosphere {
  Vec3 rayleighScattering;
  float bottomRadius = 0.0f;
  Vec3 mieScattering;
  float topRadius = 0.0f;
  Vec3 mieExtinction;
  float rayleighDensityExpScale = 0.0f;
  Vec3 absorptionExtinction;
  float mieDensityExpScale = 0.0f;
  Vec3 groundAlbedo;
  float miePhaseG = 0.0f;
  float absorptionLayerWidth = 0.0f;
  float absorption0Linear = 0.0f;
  float absorption0Constant = 0.0f;
  float absorption1Linear = 0.0f;
  float absorption1Constant = 0.0f;
  float pad[3] = {};
};
static_assert(sizeof(GpuAtmosphere) == 112);

GpuAtmosphere toGpu(const Atmosphere &a) {
  GpuAtmosphere g;
  g.bottomRadius = a.bottomRadius;
  g.topRadius = a.topRadius;
  g.rayleighScattering = a.rayleighScattering;
  g.rayleighDensityExpScale = -1.0f / a.rayleighScaleHeight;
  g.mieScattering = a.mieScattering;
  g.mieExtinction = a.mieExtinction;
  g.mieDensityExpScale = -1.0f / a.mieScaleHeight;
  g.miePhaseG = a.miePhaseG;
  g.absorptionExtinction = a.ozoneAbsorption;
  g.groundAlbedo = a.groundAlbedo;
  // Earth's ozone: density rises linearly to a peak at 25 km, then falls
  // linearly to zero at 40 km.
  g.absorptionLayerWidth = 25.0f;
  g.absorption0Linear = 1.0f / 15.0f;
  g.absorption0Constant = -2.0f / 3.0f;
  g.absorption1Linear = -1.0f / 15.0f;
  g.absorption1Constant = 8.0f / 3.0f;
  return g;
}

// Must match SceneBuffer in the shaders (std430: each vec3 takes 16 bytes).
struct SceneData {
  Mat4 viewProjection;
  Mat4 currentViewProjection;
  Mat4 previousViewProjection;
  Vec3 sunDirection;
  float pad0 = 0.0f;
  Vec3 sunIlluminance;
  uint32_t frameIndex = 0;
  Vec3 cameraPosition;
  float sunAngularRadius = 0.0f;
  VkDeviceAddress tlas = 0;
  VkDeviceAddress exposure = 0;
  VkDeviceAddress skyIrradiance = 0;
  float inverseResolution[2] = {};
  VkDeviceAddress instances = 0;
  uint64_t pad2 = 0;
  GpuAtmosphere atmosphere;
};
static_assert(sizeof(SceneData) == 400);

// Must match InstanceInfo in common.glsl.
struct InstanceInfo {
  VkDeviceAddress vertexBuffer = 0;
  VkDeviceAddress indexBuffer = 0;
  uint32_t materialIndex = 0;
  uint32_t pad = 0;
};

// TLAS instance masks: shadow rays only test shadow casters.
constexpr uint8_t kShadowCasterMask = 0x01;
constexpr uint8_t kNonCasterMask = 0x02;

// Must match ExposureBuffer in exposure.glsl.
struct ExposureState {
  float exposure = 0.0f;
  float ev100 = 0.0f;
};

constexpr uint32_t kHistogramBins = 256;

// Must match the push_constant block in post.glsl.
struct PostConstants {
  uint32_t historyValid = 0;
  float deltaSeconds = 0.0f;
  VkDeviceAddress histogram = 0;
  VkDeviceAddress exposure = 0;
  float exposureCompensation = 0.0f;
};
static_assert(sizeof(PostConstants) == 32);

// Must match the push_constant block in the shaders.
struct PushConstants {
  Mat4 model;
  VkDeviceAddress vertexBuffer = 0;
  VkDeviceAddress sceneBuffer = 0;
  VkDeviceAddress materialBuffer = 0;
  uint32_t materialIndex = 0;
  uint32_t pad = 0;
};
static_assert(sizeof(PushConstants) == 96);

// Must match Material in common.glsl.
constexpr uint32_t kNoTexture = 0xFFFFFFFFu;
constexpr uint32_t kMaterialProceduralTerrain = 1u;
constexpr uint32_t kMaterialAlphaMask = 2u;

struct GpuMaterial {
  float baseColorFactor[4] = {};
  Vec3 emissiveFactor;
  float metallicFactor = 0.0f;
  float roughnessFactor = 0.0f;
  float normalScale = 0.0f;
  float occlusionStrength = 0.0f;
  float alphaCutoff = 0.0f;
  uint32_t baseColorTexture = kNoTexture;
  uint32_t metallicRoughnessTexture = kNoTexture;
  uint32_t normalTexture = kNoTexture;
  uint32_t occlusionTexture = kNoTexture;
  uint32_t emissiveTexture = kNoTexture;
  uint32_t flags = 0;
  uint32_t pad[2] = {};
};
static_assert(sizeof(GpuMaterial) == 80);

uint32_t textureIndex(const std::optional<TextureId> &texture) {
  return texture ? texture->index : kNoTexture;
}

GpuMaterial toGpu(const MaterialData &m) {
  GpuMaterial g;
  std::copy(std::begin(m.baseColorFactor), std::end(m.baseColorFactor),
            g.baseColorFactor);
  g.emissiveFactor = m.emissiveFactor;
  g.metallicFactor = m.metallicFactor;
  g.roughnessFactor = m.roughnessFactor;
  g.normalScale = m.normalScale;
  g.occlusionStrength = m.occlusionStrength;
  g.baseColorTexture = textureIndex(m.baseColorTexture);
  g.metallicRoughnessTexture = textureIndex(m.metallicRoughnessTexture);
  g.normalTexture = textureIndex(m.normalTexture);
  g.occlusionTexture = textureIndex(m.occlusionTexture);
  g.emissiveTexture = textureIndex(m.emissiveTexture);
  g.alphaCutoff = m.alphaCutoff;
  g.flags = (m.proceduralTerrain ? kMaterialProceduralTerrain : 0u) |
            (m.alphaMode == AlphaMode::Mask ? kMaterialAlphaMask : 0u);
  return g;
}
constexpr VkShaderStageFlags kScenePushStages = VK_SHADER_STAGE_VERTEX_BIT |
                                                VK_SHADER_STAGE_FRAGMENT_BIT |
                                                VK_SHADER_STAGE_COMPUTE_BIT;

struct FrameData {
  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  VkFence renderFence = VK_NULL_HANDLE;
  VkSemaphore imageAvailable = VK_NULL_HANDLE;
  Buffer sceneBuffer;
  // Top-level acceleration structure over this frame's draw items, rebuilt
  // every frame from the instance buffer.
  Buffer instanceBuffer;
  // InstanceInfo per TLAS instance, for alpha-tested shadow rays.
  Buffer instanceInfoBuffer;
  AccelerationStructure tlas;
  Buffer tlasScratch;
};

// The NVIDIA driver leaks a few KB during device creation and on the first
// acceleration structure build in each command buffer. LeakSanitizer ignores
// allocations made while one of these lives; elsewhere it's a no-op.
#ifdef MERIDIAN_HAS_LSAN
using DriverLeakScope = __lsan::ScopedDisabler;
#else
struct DriverLeakScope {};
#endif

// Low-discrepancy sequence used for sub-pixel jitter; returns [0, 1).
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

void check(VkResult result, const char *what) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error(std::string(what) + " failed");
  }
}

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

struct PipelineDesc {
  const char *vertexShader = nullptr;
  const char *fragmentShader = nullptr;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  std::span<const VkFormat> colorFormats;
  VkFormat depthFormat = VK_FORMAT_UNDEFINED;
  // Cull mode is set per draw (double-sided materials).
  bool dynamicCullMode = false;
  bool depthTest = true;
  // Premultiplied-alpha blending into the first attachment, no depth or
  // motion writes: the surface behind keeps its depth and motion.
  bool alphaBlend = false;
};

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

} // namespace

struct Renderer::Impl {
  GLFWwindow *window = nullptr;
  vkb::Instance instance;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  vkb::Device device;
  // Device functions from extensions (ray tracing), which the loader doesn't
  // export directly.
  vkb::DispatchTable dispatch;
  VmaAllocator allocator = nullptr;
  VkDeviceSize scratchAlignment = 0;

  VkQueue graphicsQueue = VK_NULL_HANDLE;
  uint32_t graphicsQueueFamily = 0;

  vkb::Swapchain swapchain;
  std::vector<VkImage> swapchainImages;
  std::vector<VkImageView> swapchainImageViews;
  // One per swapchain image: the presentation engine may still be reading a
  // semaphore after our fence signals, so it can't be shared across images.
  std::vector<VkSemaphore> renderFinished;
  bool swapchainOutOfDate = false;

  // Sized like the swapchain and recreated with it.
  Image depth;
  Image hdr;
  Image motion;
  // Temporal anti-aliasing history, ping-ponged: each frame reads one and
  // writes the other. Always in VK_IMAGE_LAYOUT_GENERAL.
  std::array<Image, 2> history;
  bool historyValid = false;
  Mat4 previousViewProjection;

  std::array<FrameData, kFramesInFlight> frames;
  uint64_t frameNumber = 0;

  // Two GPU timestamps per frame in flight: start and end of the frame.
  VkQueryPool timestampPool = VK_NULL_HANDLE;
  float timestampPeriodNs = 0.0f;
  float gpuFrameMs = 0.0f;

  VkCommandPool uploadPool = VK_NULL_HANDLE;
  VkCommandBuffer uploadCommandBuffer = VK_NULL_HANDLE;
  VkFence uploadFence = VK_NULL_HANDLE;

  // Shared by everything that reads the scene buffer: meshes, the sky and
  // the atmosphere compute passes.
  VkPipelineLayout sceneLayout = VK_NULL_HANDLE;
  VkPipeline meshPipeline = VK_NULL_HANDLE;
  VkPipeline maskedMeshPipeline = VK_NULL_HANDLE;
  VkPipeline blendedMeshPipeline = VK_NULL_HANDLE;
  // Draw order scratch lists, reused every frame to avoid allocations.
  std::vector<uint32_t> opaqueDraws;
  std::vector<uint32_t> maskedDraws;
  std::vector<std::pair<float, uint32_t>> blendedDraws;

  VkSampler nearestSampler = VK_NULL_HANDLE;
  VkSampler linearSampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout postSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  // postSets[i] reads history[i] and writes history[1 - i].
  std::array<VkDescriptorSet, 2> postSets{};
  VkPipelineLayout postLayout = VK_NULL_HANDLE;
  VkPipeline taaPipeline = VK_NULL_HANDLE;
  VkPipeline histogramPipeline = VK_NULL_HANDLE;
  VkPipeline exposurePipeline = VK_NULL_HANDLE;
  VkPipeline skyPipeline = VK_NULL_HANDLE;

  // Atmosphere LUTs (Hillaire 2020), always in GENERAL layout: written by
  // compute passes, sampled by the sky, terrain and later passes.
  Image transmittanceLut;
  Image multiScatteringLut;
  Image skyViewLut;
  Image aerialPerspectiveLut;
  VkDescriptorSetLayout atmosphereSetLayout = VK_NULL_HANDLE;
  VkDescriptorSet atmosphereSet = VK_NULL_HANDLE;
  VkPipeline transmittanceLutPipeline = VK_NULL_HANDLE;
  VkPipeline multiScatteringLutPipeline = VK_NULL_HANDLE;
  VkPipeline skyViewLutPipeline = VK_NULL_HANDLE;
  VkPipeline aerialPerspectiveLutPipeline = VK_NULL_HANDLE;
  VkPipeline skyIrradiancePipeline = VK_NULL_HANDLE;
  Buffer skyIrradianceBuffer;
  // The transmittance and multiple-scattering LUTs depend only on the
  // atmosphere; they're rebuilt when it changes.
  std::optional<Atmosphere> builtAtmosphere;

  // Materials and textures, bound once per frame as set 1 ("bindless").
  Buffer materialBuffer;
  uint32_t materialCount = 0;
  std::vector<Image> textures;
  // CPU copy of what draw ordering and acceleration structures need.
  std::vector<MaterialData> materials;
  Image dfgLut;
  VkSampler textureSampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool materialPool = VK_NULL_HANDLE;
  VkDescriptorSet materialSet = VK_NULL_HANDLE;

  // Auto-exposure: a luminance histogram and the adapted exposure, both
  // GPU-only. Each frame shades with the exposure the previous frame computed.
  Buffer histogramBuffer;
  Buffer exposureBuffer;
  std::chrono::steady_clock::time_point lastDrawTime;
  VkPipeline tonemapPipeline = VK_NULL_HANDLE;

  std::vector<GpuMesh> meshes;

  void createSwapchain();
  void destroySwapchain();
  void createFrames();
  void createDescriptors();
  void createPipelines();
  void createExposureBuffers();
  void createAtmosphere();
  void createMaterialSystem();
  void computeDfgLut();

  Image createImage(VkFormat format, VkExtent3D extent,
                    VkImageUsageFlags usage, VkImageAspectFlags aspect,
                    uint32_t mipLevels = 1);
  Image createScreenImage(VkFormat format, VkImageUsageFlags usage,
                          VkImageAspectFlags aspect);
  void destroyImage(Image &image);

  Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VmaAllocationCreateFlags flags,
                      VkDeviceSize alignment = 0);
  void destroyBuffer(Buffer &buffer);

  AccelerationStructure
  createAccelerationStructure(VkAccelerationStructureTypeKHR type,
                              VkDeviceSize size);
  void destroyAccelerationStructure(AccelerationStructure &as);
  void buildTlas(VkCommandBuffer cmd, FrameData &frame,
                 std::span<const DrawItem> items);

  template <typename Record> void submitNow(Record &&record);
};

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

void Renderer::Impl::buildTlas(VkCommandBuffer cmd, FrameData &frame,
                               std::span<const DrawItem> items) {
  if (items.size() > kMaxInstances) {
    throw std::runtime_error("too many draw items for the TLAS");
  }

  auto *instances = static_cast<VkAccelerationStructureInstanceKHR *>(
      frame.instanceBuffer.mapped);
  auto *infos = static_cast<InstanceInfo *>(frame.instanceInfoBuffer.mapped);
  for (size_t i = 0; i < items.size(); ++i) {
    const GpuMesh &mesh = meshes[items[i].mesh.index];
    infos[i] = {mesh.vertices.address, mesh.indices.address, mesh.materialIndex};
    const Mat4 &m = items[i].transform;
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
    instances[i] = instance;
  }
  check(vmaFlushAllocation(allocator, frame.instanceBuffer.allocation, 0,
                           VK_WHOLE_SIZE),
        "vmaFlushAllocation");
  check(vmaFlushAllocation(allocator, frame.instanceInfoBuffer.allocation, 0,
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
  range.primitiveCount = static_cast<uint32_t>(items.size());
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

// Records commands with `record`, submits them and waits until the GPU is
// done. Only for loading, never per frame.
template <typename Record> void Renderer::Impl::submitNow(Record &&record) {
  VkDevice dev = device.device;
  check(vkResetFences(dev, 1, &uploadFence), "vkResetFences");
  check(vkResetCommandBuffer(uploadCommandBuffer, 0), "vkResetCommandBuffer");

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  check(vkBeginCommandBuffer(uploadCommandBuffer, &beginInfo),
        "vkBeginCommandBuffer");
  record(uploadCommandBuffer);
  check(vkEndCommandBuffer(uploadCommandBuffer), "vkEndCommandBuffer");

  VkCommandBufferSubmitInfo cmdInfo{};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cmdInfo.commandBuffer = uploadCommandBuffer;

  VkSubmitInfo2 submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmdInfo;
  check(vkQueueSubmit2(graphicsQueue, 1, &submit, uploadFence),
        "vkQueueSubmit2");
  check(vkWaitForFences(dev, 1, &uploadFence, VK_TRUE, UINT64_MAX),
        "vkWaitForFences");
}

void Renderer::Impl::createSwapchain() {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window, &width, &height);

  auto built =
      vkb::SwapchainBuilder{device}
          .set_desired_format({VK_FORMAT_B8G8R8A8_SRGB,
                               VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
          .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
          .set_desired_extent(width, height)
          .build();
  if (!built) {
    throw std::runtime_error(built.error().message());
  }
  swapchain = built.value();
  swapchainImages = swapchain.get_images().value();
  swapchainImageViews = swapchain.get_image_views().value();

  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  renderFinished.resize(swapchainImages.size());
  for (VkSemaphore &semaphore : renderFinished) {
    check(vkCreateSemaphore(device.device, &semaphoreInfo, nullptr, &semaphore),
          "vkCreateSemaphore");
  }

  depth = createScreenImage(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT);
  hdr = createScreenImage(kHdrFormat,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
  motion = createScreenImage(kMotionFormat,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
  for (Image &image : history) {
    image = createScreenImage(kHdrFormat,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
  }
  submitNow([&](VkCommandBuffer cmd) {
    for (Image &image : history) {
      transitionImage(cmd, {image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, 0,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    }
  });
  historyValid = false;

  // Point the post-processing sets at the new images. Safe because the
  // device is idle whenever the swapchain is (re)created.
  for (int i = 0; i < 2; ++i) {
    const Image &previous = history[i];
    const Image &next = history[1 - i];
    VkDescriptorImageInfo images[] = {
        {nearestSampler, hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {nearestSampler, motion.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {linearSampler, previous.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, next.view, VK_IMAGE_LAYOUT_GENERAL},
        {nearestSampler, next.view, VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t b = 0; b < 5; ++b) {
      writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[b].dstSet = postSets[i];
      writes[b].dstBinding = b;
      writes[b].descriptorCount = 1;
      writes[b].descriptorType = b == 3
                                     ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                     : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[b].pImageInfo = &images[b];
    }
    vkUpdateDescriptorSets(device.device, 5, writes, 0, nullptr);
  }

  swapchainOutOfDate = false;

  std::cout << "Swapchain: " << swapchainImages.size() << " images, "
            << swapchain.extent.width << "x" << swapchain.extent.height
            << "\n";
}

void Renderer::Impl::destroySwapchain() {
  for (Image &image : history) {
    destroyImage(image);
  }
  destroyImage(motion);
  destroyImage(hdr);
  destroyImage(depth);

  for (VkSemaphore semaphore : renderFinished) {
    vkDestroySemaphore(device.device, semaphore, nullptr);
  }
  renderFinished.clear();
  swapchain.destroy_image_views(swapchainImageViews);
  vkb::destroy_swapchain(swapchain);
}

void Renderer::Impl::createFrames() {
  VkDevice dev = device.device;

  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = graphicsQueueFamily;

  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  for (FrameData &frame : frames) {
    check(vkCreateCommandPool(dev, &poolInfo, nullptr, &frame.commandPool),
          "vkCreateCommandPool");
    allocInfo.commandPool = frame.commandPool;
    check(vkAllocateCommandBuffers(dev, &allocInfo, &frame.commandBuffer),
          "vkAllocateCommandBuffers");
    check(vkCreateFence(dev, &fenceInfo, nullptr, &frame.renderFence),
          "vkCreateFence");
    check(vkCreateSemaphore(dev, &semaphoreInfo, nullptr,
                            &frame.imageAvailable),
          "vkCreateSemaphore");
    frame.sceneBuffer = createBuffer(
        sizeof(SceneData),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    frame.instanceInfoBuffer = createBuffer(
        kMaxInstances * sizeof(InstanceInfo),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    frame.instanceBuffer = createBuffer(
        kMaxInstances * sizeof(VkAccelerationStructureInstanceKHR),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }

  // Size each TLAS for the maximum instance count once, so per-frame
  // rebuilds never allocate.
  VkAccelerationStructureGeometryKHR tlasGeometry{};
  tlasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  tlasGeometry.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;

  VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{};
  tlasBuild.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  tlasBuild.geometryCount = 1;
  tlasBuild.pGeometries = &tlasGeometry;

  VkAccelerationStructureBuildSizesInfoKHR tlasSizes{};
  tlasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  dispatch.getAccelerationStructureBuildSizesKHR(
      VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild,
      &kMaxInstances, &tlasSizes);

  for (FrameData &frame : frames) {
    frame.tlas = createAccelerationStructure(
        VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        tlasSizes.accelerationStructureSize);
    frame.tlasScratch = createBuffer(
        tlasSizes.buildScratchSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        0, scratchAlignment);
  }

  check(vkCreateCommandPool(dev, &poolInfo, nullptr, &uploadPool),
        "vkCreateCommandPool");
  allocInfo.commandPool = uploadPool;
  check(vkAllocateCommandBuffers(dev, &allocInfo, &uploadCommandBuffer),
        "vkAllocateCommandBuffers");
  fenceInfo.flags = 0;
  check(vkCreateFence(dev, &fenceInfo, nullptr, &uploadFence),
        "vkCreateFence");

  VkQueryPoolCreateInfo queryInfo{};
  queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
  queryInfo.queryCount = kFramesInFlight * 2;
  check(vkCreateQueryPool(dev, &queryInfo, nullptr, &timestampPool),
        "vkCreateQueryPool");
}

void Renderer::Impl::createDescriptors() {
  VkDevice dev = device.device;

  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter = VK_FILTER_NEAREST;
  samplerInfo.minFilter = VK_FILTER_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  check(vkCreateSampler(dev, &samplerInfo, nullptr, &nearestSampler),
        "vkCreateSampler");
  samplerInfo.magFilter = VK_FILTER_LINEAR;
  samplerInfo.minFilter = VK_FILTER_LINEAR;
  check(vkCreateSampler(dev, &samplerInfo, nullptr, &linearSampler),
        "vkCreateSampler");

  // Must match post.glsl.
  constexpr VkShaderStageFlags kPostStages =
      VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutBinding bindings[5]{};
  for (uint32_t b = 0; b < 5; ++b) {
    bindings[b].binding = b;
    bindings[b].descriptorType = b == 3
                                     ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                     : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[b].descriptorCount = 1;
    bindings[b].stageFlags = kPostStages;
  }

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 5;
  layoutInfo.pBindings = bindings;
  check(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &postSetLayout),
        "vkCreateDescriptorSetLayout");

  // Must match atmosphere.glsl and the *_lut.comp shaders: four sampled
  // LUTs, then the same four as storage images.
  VkDescriptorSetLayoutBinding atmosphereBindings[8]{};
  for (uint32_t b = 0; b < 8; ++b) {
    atmosphereBindings[b].binding = b;
    atmosphereBindings[b].descriptorType =
        b < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    atmosphereBindings[b].descriptorCount = 1;
    atmosphereBindings[b].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo atmosphereLayoutInfo{};
  atmosphereLayoutInfo.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  atmosphereLayoutInfo.bindingCount = 8;
  atmosphereLayoutInfo.pBindings = atmosphereBindings;
  check(vkCreateDescriptorSetLayout(dev, &atmosphereLayoutInfo, nullptr,
                                    &atmosphereSetLayout),
        "vkCreateDescriptorSetLayout");

  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6},
  };
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 3;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = poolSizes;
  check(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &descriptorPool),
        "vkCreateDescriptorPool");

  VkDescriptorSetLayout setLayouts[] = {postSetLayout, postSetLayout};
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = descriptorPool;
  allocInfo.descriptorSetCount = 2;
  allocInfo.pSetLayouts = setLayouts;
  check(vkAllocateDescriptorSets(dev, &allocInfo, postSets.data()),
        "vkAllocateDescriptorSets");

  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &atmosphereSetLayout;
  check(vkAllocateDescriptorSets(dev, &allocInfo, &atmosphereSet),
        "vkAllocateDescriptorSets");
}

void Renderer::Impl::createAtmosphere() {
  constexpr VkFormat kLutFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
  constexpr VkImageUsageFlags kLutUsage =
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  // Sizes must match atmosphere.glsl. 32-bit floats: the reference notes
  // visible artifacts with 16-bit LUTs.
  transmittanceLut = createImage(kLutFormat, {256, 64, 1}, kLutUsage,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
  multiScatteringLut = createImage(kLutFormat, {32, 32, 1}, kLutUsage,
                                   VK_IMAGE_ASPECT_COLOR_BIT);
  skyViewLut = createImage(kLutFormat, {192, 108, 1}, kLutUsage,
                           VK_IMAGE_ASPECT_COLOR_BIT);
  aerialPerspectiveLut = createImage(kLutFormat, {32, 32, 32}, kLutUsage,
                                     VK_IMAGE_ASPECT_COLOR_BIT);
  skyIrradianceBuffer = createBuffer(
      16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      0);

  const Image *luts[] = {&transmittanceLut, &multiScatteringLut, &skyViewLut,
                         &aerialPerspectiveLut};
  submitNow([&](VkCommandBuffer cmd) {
    for (const Image *lut : luts) {
      transitionImage(cmd, {lut->image, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, 0,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    }
  });

  VkDescriptorImageInfo images[8]{};
  VkWriteDescriptorSet writes[8]{};
  for (uint32_t b = 0; b < 8; ++b) {
    const bool sampled = b < 4;
    images[b] = {sampled ? linearSampler : VK_NULL_HANDLE, luts[b % 4]->view,
                 VK_IMAGE_LAYOUT_GENERAL};
    writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[b].dstSet = atmosphereSet;
    writes[b].dstBinding = b;
    writes[b].descriptorCount = 1;
    writes[b].descriptorType = sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                       : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[b].pImageInfo = &images[b];
  }
  vkUpdateDescriptorSets(device.device, 8, writes, 0, nullptr);
}

void Renderer::Impl::createExposureBuffers() {
  constexpr VkBufferUsageFlags kUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  histogramBuffer =
      createBuffer(kHistogramBins * sizeof(uint32_t), kUsage, 0);
  exposureBuffer = createBuffer(sizeof(ExposureState), kUsage, 0);

  ExposureState initial;
  initial.ev100 = kInitialEv100;
  initial.exposure = 1.0f / (1.2f * std::exp2(kInitialEv100));
  submitNow([&](VkCommandBuffer cmd) {
    vkCmdFillBuffer(cmd, histogramBuffer.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdUpdateBuffer(cmd, exposureBuffer.buffer, 0, sizeof(initial),
                      &initial);

    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
  });
}

void Renderer::Impl::createMaterialSystem() {
  VkDevice dev = device.device;

  materialBuffer = createBuffer(
      kMaxMaterials * sizeof(GpuMaterial),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      0);

  // Trilinear with 16x anisotropy: textures stay sharp at grazing angles.
  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter = VK_FILTER_LINEAR;
  samplerInfo.minFilter = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.anisotropyEnable = VK_TRUE;
  samplerInfo.maxAnisotropy = 16.0f;
  samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
  check(vkCreateSampler(dev, &samplerInfo, nullptr, &textureSampler),
        "vkCreateSampler");

  // Must match material.glsl and dfg_lut.comp.
  VkDescriptorSetLayoutBinding bindings[3]{};
  bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures,
                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  // Textures can be added while frames using the set are in flight, and
  // unused slots never need to be valid.
  VkDescriptorBindingFlags bindingFlags[3] = {
      0,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
          VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
      0};
  VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
  flagsInfo.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
  flagsInfo.bindingCount = 3;
  flagsInfo.pBindingFlags = bindingFlags;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.pNext = &flagsInfo;
  layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  layoutInfo.bindingCount = 3;
  layoutInfo.pBindings = bindings;
  check(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr,
                                    &materialSetLayout),
        "vkCreateDescriptorSetLayout");

  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures + 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
  };
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = poolSizes;
  check(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &materialPool),
        "vkCreateDescriptorPool");

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = materialPool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &materialSetLayout;
  check(vkAllocateDescriptorSets(dev, &allocInfo, &materialSet),
        "vkAllocateDescriptorSets");

  dfgLut = createImage(VK_FORMAT_R16G16_SFLOAT, {kDfgLutSize, kDfgLutSize, 1},
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
  VkDescriptorImageInfo dfgSampled{linearSampler, dfgLut.view,
                                   VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo dfgStorage{VK_NULL_HANDLE, dfgLut.view,
                                   VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[2]{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = materialSet;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[0].pImageInfo = &dfgSampled;
  writes[1] = writes[0];
  writes[1].dstBinding = 2;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[1].pImageInfo = &dfgStorage;
  vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
}

// The DFG LUT depends on nothing but the BRDF, so it's computed once.
void Renderer::Impl::computeDfgLut() {
  VkPipeline pipeline =
      createComputePipeline(device.device, "dfg_lut.comp", sceneLayout);
  submitNow([&](VkCommandBuffer cmd) {
    transitionImage(cmd, {dfgLut.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_NONE, 0,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sceneLayout,
                            1, 1, &materialSet, 0, nullptr);
    vkCmdDispatch(cmd, kDfgLutSize / 8, kDfgLutSize / 8, 1);
    transitionImage(cmd, {dfgLut.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
  });
  vkDestroyPipeline(device.device, pipeline, nullptr);
}

void Renderer::Impl::createPipelines() {
  VkDevice dev = device.device;

  VkPushConstantRange pushRange{};
  pushRange.stageFlags = kScenePushStages;
  pushRange.size = sizeof(PushConstants);

  VkPipelineLayoutCreateInfo sceneLayoutInfo{};
  sceneLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  const VkDescriptorSetLayout sceneSetLayouts[] = {atmosphereSetLayout,
                                                   materialSetLayout};
  sceneLayoutInfo.setLayoutCount = 2;
  sceneLayoutInfo.pSetLayouts = sceneSetLayouts;
  sceneLayoutInfo.pushConstantRangeCount = 1;
  sceneLayoutInfo.pPushConstantRanges = &pushRange;
  check(vkCreatePipelineLayout(dev, &sceneLayoutInfo, nullptr, &sceneLayout),
        "vkCreatePipelineLayout");

  const VkFormat sceneFormats[] = {kHdrFormat, kMotionFormat};
  PipelineDesc meshDesc{.vertexShader = "mesh.vert",
                        .fragmentShader = "mesh.frag",
                        .layout = sceneLayout,
                        .colorFormats = sceneFormats,
                        .depthFormat = kDepthFormat,
                        .dynamicCullMode = true};
  meshPipeline = createGraphicsPipeline(dev, meshDesc);
  // Alpha testing (discard) disables early depth testing, so masked
  // materials get their own pipeline instead of slowing down opaque ones.
  meshDesc.fragmentShader = "mesh_masked.frag";
  maskedMeshPipeline = createGraphicsPipeline(dev, meshDesc);
  meshDesc.fragmentShader = "mesh_blended.frag";
  meshDesc.alphaBlend = true;
  blendedMeshPipeline = createGraphicsPipeline(dev, meshDesc);

  VkPushConstantRange postPushRange{};
  postPushRange.stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  postPushRange.size = sizeof(PostConstants);

  VkPipelineLayoutCreateInfo postLayoutInfo{};
  postLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  postLayoutInfo.setLayoutCount = 1;
  postLayoutInfo.pSetLayouts = &postSetLayout;
  postLayoutInfo.pushConstantRangeCount = 1;
  postLayoutInfo.pPushConstantRanges = &postPushRange;
  check(vkCreatePipelineLayout(dev, &postLayoutInfo, nullptr, &postLayout),
        "vkCreatePipelineLayout");

  taaPipeline = createComputePipeline(dev, "taa.comp", postLayout);
  histogramPipeline =
      createComputePipeline(dev, "histogram.comp", postLayout);
  exposurePipeline = createComputePipeline(dev, "exposure.comp", postLayout);

  transmittanceLutPipeline =
      createComputePipeline(dev, "transmittance_lut.comp", sceneLayout);
  multiScatteringLutPipeline =
      createComputePipeline(dev, "multiscattering_lut.comp", sceneLayout);
  skyViewLutPipeline =
      createComputePipeline(dev, "sky_view_lut.comp", sceneLayout);
  aerialPerspectiveLutPipeline =
      createComputePipeline(dev, "aerial_perspective_lut.comp", sceneLayout);
  skyIrradiancePipeline =
      createComputePipeline(dev, "sky_irradiance.comp", sceneLayout);

  skyPipeline = createGraphicsPipeline(dev, {.vertexShader = "sky.vert",
                                             .fragmentShader = "sky.frag",
                                             .layout = sceneLayout,
                                             .colorFormats = sceneFormats,
                                             .depthFormat = kDepthFormat,
                                             .depthTest = false});

  const VkFormat swapchainFormat[] = {swapchain.image_format};
  tonemapPipeline =
      createGraphicsPipeline(dev, {.vertexShader = "fullscreen.vert",
                                   .fragmentShader = "tonemap.frag",
                                   .layout = postLayout,
                                   .colorFormats = swapchainFormat});
}

Renderer::Renderer(Window &window) : impl_(std::make_unique<Impl>()) {
  impl_->window = window.handle();

  vkb::InstanceBuilder instanceBuilder;
  instanceBuilder.set_app_name("Meridian").require_api_version(1, 3, 0);
  if (kEnableValidation) {
    instanceBuilder.request_validation_layers().use_default_debug_messenger();
  }
  auto instance = instanceBuilder.build();
  if (!instance) {
    throw std::runtime_error(instance.error().message());
  }
  impl_->instance = instance.value();

  check(glfwCreateWindowSurface(impl_->instance.instance, impl_->window,
                                nullptr, &impl_->surface),
        "glfwCreateWindowSurface");

  VkPhysicalDeviceFeatures features{};
  features.samplerAnisotropy = VK_TRUE;
  // Blended surfaces blend color but leave motion vectors untouched.
  features.independentBlend = VK_TRUE;

  VkPhysicalDeviceVulkan12Features features12{};
  features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  features12.bufferDeviceAddress = VK_TRUE;
  // Bindless textures: one large, partially filled array indexed per pixel.
  features12.descriptorIndexing = VK_TRUE;
  features12.runtimeDescriptorArray = VK_TRUE;
  features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
  features12.descriptorBindingPartiallyBound = VK_TRUE;
  features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
  features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;

  VkPhysicalDeviceVulkan13Features features13{};
  features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  features13.dynamicRendering = VK_TRUE;
  features13.synchronization2 = VK_TRUE;
  // discard (alpha masking) compiles to demote-to-helper in SPIR-V 1.6.
  features13.shaderDemoteToHelperInvocation = VK_TRUE;

  // Hardware ray tracing is required: shadows (and later reflections and GI)
  // are traced with ray queries.
  VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures{};
  accelerationFeatures.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
  accelerationFeatures.accelerationStructure = VK_TRUE;

  VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
  rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
  rayQueryFeatures.rayQuery = VK_TRUE;

  auto physical =
      vkb::PhysicalDeviceSelector{impl_->instance}
          .set_minimum_version(1, 3)
          .set_required_features(features)
          .set_required_features_12(features12)
          .set_required_features_13(features13)
          .add_required_extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
          .add_required_extension(VK_KHR_RAY_QUERY_EXTENSION_NAME)
          .add_required_extension(
              VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)
          .add_required_extension_features(accelerationFeatures)
          .add_required_extension_features(rayQueryFeatures)
          .set_surface(impl_->surface)
          .select();
  if (!physical) {
    throw std::runtime_error(physical.error().message());
  }

  auto device = [&] {
    DriverLeakScope driverLeak;
    return vkb::DeviceBuilder{physical.value()}.build();
  }();
  if (!device) {
    throw std::runtime_error(device.error().message());
  }
  impl_->device = device.value();
  impl_->dispatch = impl_->device.make_table();

  VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties{};
  accelerationProperties.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &accelerationProperties;
  vkGetPhysicalDeviceProperties2(physical.value().physical_device, &properties);
  impl_->scratchAlignment =
      accelerationProperties.minAccelerationStructureScratchOffsetAlignment;

  impl_->graphicsQueue =
      impl_->device.get_queue(vkb::QueueType::graphics).value();
  impl_->graphicsQueueFamily =
      impl_->device.get_queue_index(vkb::QueueType::graphics).value();

  VmaAllocatorCreateInfo allocatorInfo{};
  allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  allocatorInfo.physicalDevice = physical.value().physical_device;
  allocatorInfo.device = impl_->device.device;
  allocatorInfo.instance = impl_->instance.instance;
  allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
  check(vmaCreateAllocator(&allocatorInfo, &impl_->allocator),
        "vmaCreateAllocator");

  impl_->timestampPeriodNs = physical.value().properties.limits.timestampPeriod;
  impl_->createFrames();
  impl_->createDescriptors();
  impl_->createSwapchain();
  impl_->createMaterialSystem();
  impl_->createPipelines();
  impl_->computeDfgLut();
  impl_->createExposureBuffers();
  impl_->createAtmosphere();
  impl_->lastDrawTime = std::chrono::steady_clock::now();

  std::cout << "GPU: " << physical.value().name << "\n";
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
  const VkDeviceSize vertexBytes = data.vertices.size() * sizeof(Vertex);
  const VkDeviceSize indexBytes = data.indices.size() * sizeof(uint32_t);

  GpuMesh mesh;
  mesh.indexCount = static_cast<uint32_t>(data.indices.size());
  mesh.materialIndex = material.index;
  mesh.alphaMode = impl_->materials[material.index].alphaMode;
  mesh.doubleSided = impl_->materials[material.index].doubleSided;
  constexpr VkBufferUsageFlags kRayTracingInput =
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  mesh.vertices = impl_->createBuffer(
      vertexBytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          kRayTracingInput,
      0);
  mesh.indices = impl_->createBuffer(
      indexBytes,
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          kRayTracingInput,
      0);

  // GPU-only memory is fastest to read but the CPU can't write it, so the
  // data goes through a temporary CPU-visible staging buffer.
  Buffer staging = impl_->createBuffer(
      vertexBytes + indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  std::memcpy(staging.mapped, data.vertices.data(), vertexBytes);
  std::memcpy(static_cast<char *>(staging.mapped) + vertexBytes,
              data.indices.data(), indexBytes);
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
  triangles.vertexData.deviceAddress = mesh.vertices.address;
  triangles.vertexStride = sizeof(Vertex);
  triangles.maxVertex = static_cast<uint32_t>(data.vertices.size() - 1);
  triangles.indexType = VK_INDEX_TYPE_UINT32;
  triangles.indexData.deviceAddress = mesh.indices.address;

  VkAccelerationStructureBuildGeometryInfoKHR build{};
  build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geometry;

  const uint32_t triangleCount = mesh.indexCount / 3;
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

  impl_->submitNow([&](VkCommandBuffer cmd) {
    VkBufferCopy vertexCopy{0, 0, vertexBytes};
    vkCmdCopyBuffer(cmd, staging.buffer, mesh.vertices.buffer, 1, &vertexCopy);
    VkBufferCopy indexCopy{vertexBytes, 0, indexBytes};
    vkCmdCopyBuffer(cmd, staging.buffer, mesh.indices.buffer, 1, &indexCopy);

    VkMemoryBarrier2 copied{};
    copied.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    copied.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    copied.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    copied.dstStageMask =
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
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
  return {static_cast<uint32_t>(impl_->meshes.size() - 1)};
}

float Renderer::gpuFrameMs() const { return impl_->gpuFrameMs; }

void Renderer::draw(const Camera &camera, const Lighting &lighting,
                    std::span<const DrawItem> items) {
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
  uint64_t timestamps[2] = {};
  if (impl_->frameNumber >= kFramesInFlight &&
      vkGetQueryPoolResults(dev, impl_->timestampPool, frameSlot * 2, 2,
                            sizeof(timestamps), timestamps, sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
    impl_->gpuFrameMs = static_cast<float>(timestamps[1] - timestamps[0]) *
                        impl_->timestampPeriodNs * 1e-6f;
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

  const Mat4 view = camera.view();
  const Mat4 projection = camera.projection(aspect);
  Mat4 jitteredProjection = projection;
  jitteredProjection.m[2][0] += 2.0f * jitterX / static_cast<float>(extent.width);
  jitteredProjection.m[2][1] += 2.0f * jitterY / static_cast<float>(extent.height);

  SceneData scene;
  scene.viewProjection = jitteredProjection * view;
  scene.currentViewProjection = projection * view;
  scene.previousViewProjection = impl_->historyValid
                                     ? impl_->previousViewProjection
                                     : scene.currentViewProjection;
  scene.frameIndex = static_cast<uint32_t>(impl_->frameNumber);
  impl_->previousViewProjection = scene.currentViewProjection;
  scene.sunDirection = lighting.sunDirection;
  scene.exposure = impl_->exposureBuffer.address;
  scene.sunIlluminance = lighting.sunIlluminance;
  scene.cameraPosition = camera.position;
  scene.sunAngularRadius = lighting.sunAngularRadius;
  scene.skyIrradiance = impl_->skyIrradianceBuffer.address;
  scene.inverseResolution[0] = 1.0f / static_cast<float>(extent.width);
  scene.inverseResolution[1] = 1.0f / static_cast<float>(extent.height);
  scene.atmosphere = toGpu(lighting.atmosphere);
  scene.tlas = frame.tlas.address;
  scene.instances = frame.instanceInfoBuffer.address;
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

  vkCmdResetQueryPool(cmd, impl_->timestampPool, frameSlot * 2, 2);
  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_NONE, impl_->timestampPool,
                       frameSlot * 2);

  VkViewport viewport{};
  viewport.width = static_cast<float>(extent.width);
  viewport.height = static_cast<float>(extent.height);
  viewport.maxDepth = 1.0f;
  VkRect2D scissor = {{0, 0}, extent};

  impl_->buildTlas(cmd, frame, items);

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
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kDepthStages,
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
  depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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

  vkCmdBeginRendering(cmd, &sceneRendering);
  const VkDescriptorSet sceneSets[] = {impl_->atmosphereSet, impl_->materialSet};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          impl_->sceneLayout, 0, 2, sceneSets, 0, nullptr);
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->skyPipeline);
  vkCmdPushConstants(cmd, impl_->sceneLayout, kScenePushStages, 0,
                     sizeof(scenePush), &scenePush);
  vkCmdDraw(cmd, 3, 1, 0, 0);

  // Opaque first, then alpha-masked, then blended back to front (each
  // blended surface must composite over everything behind it).
  impl_->opaqueDraws.clear();
  impl_->maskedDraws.clear();
  impl_->blendedDraws.clear();
  for (uint32_t i = 0; i < items.size(); ++i) {
    switch (impl_->meshes[items[i].mesh.index].alphaMode) {
    case AlphaMode::Opaque:
      impl_->opaqueDraws.push_back(i);
      break;
    case AlphaMode::Mask:
      impl_->maskedDraws.push_back(i);
      break;
    case AlphaMode::Blend: {
      const Mat4 &t = items[i].transform;
      const Vec3 position = {t.m[3][0], t.m[3][1], t.m[3][2]};
      const Vec3 offset = position - camera.position;
      impl_->blendedDraws.emplace_back(dot(offset, offset), i);
      break;
    }
    }
  }
  std::sort(impl_->blendedDraws.begin(), impl_->blendedDraws.end(),
            [](const auto &a, const auto &b) { return a.first > b.first; });

  auto drawItem = [&](uint32_t index) {
    const DrawItem &item = items[index];
    const GpuMesh &mesh = impl_->meshes[item.mesh.index];

    PushConstants push;
    push.model = item.transform;
    push.vertexBuffer = mesh.vertices.address;
    push.sceneBuffer = frame.sceneBuffer.address;
    push.materialBuffer = impl_->materialBuffer.address;
    push.materialIndex = mesh.materialIndex;
    vkCmdPushConstants(cmd, impl_->sceneLayout, kScenePushStages, 0,
                       sizeof(push), &push);
    vkCmdSetCullMode(cmd, mesh.doubleSided ? VK_CULL_MODE_NONE
                                           : VK_CULL_MODE_BACK_BIT);
    vkCmdBindIndexBuffer(cmd, mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
  };

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->meshPipeline);
  for (uint32_t index : impl_->opaqueDraws) {
    drawItem(index);
  }
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    impl_->maskedMeshPipeline);
  for (uint32_t index : impl_->maskedDraws) {
    drawItem(index);
  }
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    impl_->blendedMeshPipeline);
  for (const auto &[distance, index] : impl_->blendedDraws) {
    drawItem(index);
  }

  vkCmdEndRendering(cmd);

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
  const auto now = std::chrono::steady_clock::now();
  PostConstants postConstants;
  postConstants.historyValid = impl_->historyValid ? 1 : 0;
  postConstants.deltaSeconds = std::min(
      std::chrono::duration<float>(now - impl_->lastDrawTime).count(), 0.1f);
  postConstants.histogram = impl_->histogramBuffer.address;
  postConstants.exposure = impl_->exposureBuffer.address;
  postConstants.exposureCompensation = camera.exposureCompensation;
  impl_->lastDrawTime = now;

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

  transitionImage(cmd, {swapchainImage, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_NONE, 0});

  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       impl_->timestampPool, frameSlot * 2 + 1);

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

Renderer::~Renderer() {
  VkDevice dev = impl_->device.device;
  vkDeviceWaitIdle(dev);

  for (GpuMesh &mesh : impl_->meshes) {
    impl_->destroyAccelerationStructure(mesh.blas);
    impl_->destroyBuffer(mesh.vertices);
    impl_->destroyBuffer(mesh.indices);
  }

  vkDestroyPipeline(dev, impl_->tonemapPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->skyPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->skyIrradiancePipeline, nullptr);
  vkDestroyPipeline(dev, impl_->aerialPerspectiveLutPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->skyViewLutPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->multiScatteringLutPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->transmittanceLutPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->exposurePipeline, nullptr);
  vkDestroyPipeline(dev, impl_->histogramPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->taaPipeline, nullptr);
  vkDestroyPipelineLayout(dev, impl_->postLayout, nullptr);
  vkDestroyPipeline(dev, impl_->meshPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->maskedMeshPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->blendedMeshPipeline, nullptr);
  vkDestroyPipelineLayout(dev, impl_->sceneLayout, nullptr);

  vkDestroyQueryPool(dev, impl_->timestampPool, nullptr);
  vkDestroyFence(dev, impl_->uploadFence, nullptr);
  vkDestroyCommandPool(dev, impl_->uploadPool, nullptr);
  for (FrameData &frame : impl_->frames) {
    impl_->destroyAccelerationStructure(frame.tlas);
    impl_->destroyBuffer(frame.tlasScratch);
    impl_->destroyBuffer(frame.instanceBuffer);
    impl_->destroyBuffer(frame.instanceInfoBuffer);
    impl_->destroyBuffer(frame.sceneBuffer);
    vkDestroySemaphore(dev, frame.imageAvailable, nullptr);
    vkDestroyFence(dev, frame.renderFence, nullptr);
    vkDestroyCommandPool(dev, frame.commandPool, nullptr);
  }

  for (Image &texture : impl_->textures) {
    impl_->destroyImage(texture);
  }
  impl_->destroyImage(impl_->dfgLut);
  impl_->destroyBuffer(impl_->materialBuffer);
  vkDestroyDescriptorPool(dev, impl_->materialPool, nullptr);
  vkDestroyDescriptorSetLayout(dev, impl_->materialSetLayout, nullptr);
  vkDestroySampler(dev, impl_->textureSampler, nullptr);
  impl_->destroyBuffer(impl_->skyIrradianceBuffer);
  impl_->destroyImage(impl_->aerialPerspectiveLut);
  impl_->destroyImage(impl_->skyViewLut);
  impl_->destroyImage(impl_->multiScatteringLut);
  impl_->destroyImage(impl_->transmittanceLut);
  impl_->destroyBuffer(impl_->exposureBuffer);
  impl_->destroyBuffer(impl_->histogramBuffer);
  impl_->destroySwapchain();
  vkDestroyDescriptorPool(dev, impl_->descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(dev, impl_->atmosphereSetLayout, nullptr);
  vkDestroyDescriptorSetLayout(dev, impl_->postSetLayout, nullptr);
  vkDestroySampler(dev, impl_->linearSampler, nullptr);
  vkDestroySampler(dev, impl_->nearestSampler, nullptr);
  vmaDestroyAllocator(impl_->allocator);

  vkb::destroy_device(impl_->device);
  vkb::destroy_surface(impl_->instance, impl_->surface);
  vkb::destroy_instance(impl_->instance);
}

} // namespace meridian
