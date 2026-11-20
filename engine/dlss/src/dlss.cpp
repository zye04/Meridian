#include <meridian/dlss.h>

#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/lsan_interface.h>
#define MERIDIAN_HAS_LSAN 1
#endif
#endif

namespace meridian::dlss {
namespace {

// The NVIDIA driver leaks small one-time allocations in NGX's extension
// queries, init, feature creation and first evaluate, as it does around
// device creation (the core's DriverLeakScope in gpu_types.h).
#ifdef MERIDIAN_HAS_LSAN
using DriverLeakScope = __lsan::ScopedDisabler;
#else
struct DriverLeakScope {
  DriverLeakScope() {} // user-provided: no unused-variable warnings
};
#endif

// NGX identifies applications without an NVIDIA application id by a
// GUID-like project id and the custom engine type.
constexpr const char *kProjectId = "dfa85430-38bb-43eb-9b8a-b8d837c39331";
constexpr const char *kEngineVersion = "1.0";

std::string resultString(NVSDK_NGX_Result result) {
  char text[16];
  std::snprintf(text, sizeof text, "0x%08x", static_cast<unsigned>(result));
  return text;
}

void check(NVSDK_NGX_Result result, const char *what) {
  if (NVSDK_NGX_FAILED(result)) {
    throw std::runtime_error(std::string(what) + " failed: " + resultString(result));
  }
}

NVSDK_NGX_PerfQuality_Value perfQuality(DlssMode mode) {
  switch (mode) {
  case DlssMode::Dlaa:
    return NVSDK_NGX_PerfQuality_Value_DLAA;
  case DlssMode::Quality:
    return NVSDK_NGX_PerfQuality_Value_MaxQuality;
  case DlssMode::Balanced:
    return NVSDK_NGX_PerfQuality_Value_Balanced;
  case DlssMode::Performance:
    return NVSDK_NGX_PerfQuality_Value_MaxPerf;
  }
  return NVSDK_NGX_PerfQuality_Value_DLAA;
}

const char *presetParameter(DlssMode mode) {
  switch (mode) {
  case DlssMode::Dlaa:
    return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
  case DlssMode::Quality:
    return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
  case DlssMode::Balanced:
    return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
  case DlssMode::Performance:
    return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
  }
  return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
}

// The Super Resolution preset per mode when --dlss-preset gives none; 0
// keeps NGX's default. SDK 310.9.16 defaults DLAA, Quality and Balanced to
// preset K, which smears forward motion (DLAA, 1440p: forward 5 m/s
// Laplacian ratio 0.44, SSIM 0.926); preset M, the second-generation
// transformer, measured 0.96 / 0.993 (Quality 0.82 vs 0.49, Balanced 0.78
// vs 0.49). Performance already defaults to M. Re-check the newest preset
// on every SDK bump (decisions.md).
char defaultPreset(DlssMode mode) {
  return mode == DlssMode::Performance ? 0 : 'M';
}

NVSDK_NGX_Resource_VK resource(const UpscalerImage &image, VkImageAspectFlags aspect,
                               bool readWrite) {
  return NVSDK_NGX_Create_ImageView_Resource_VK(
      image.view, image.image, {aspect, 0, 1, 0, 1}, image.format, image.extent.width,
      image.extent.height, readWrite);
}

class DlssUpscaler final : public Upscaler {
public:
  DlssUpscaler(NVSDK_NGX_Handle *feature, NVSDK_NGX_Parameter *parameters)
      : feature_(feature), parameters_(parameters) {}
  ~DlssUpscaler() override { NVSDK_NGX_VULKAN_ReleaseFeature(feature_); }
  DlssUpscaler(const DlssUpscaler &) = delete;
  DlssUpscaler &operator=(const DlssUpscaler &) = delete;

  void evaluate(const UpscalerFrame &frame) override {
    NVSDK_NGX_Resource_VK color = resource(frame.color, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK output = resource(frame.output, VK_IMAGE_ASPECT_COLOR_BIT, true);
    NVSDK_NGX_Resource_VK depth = resource(frame.depth, VK_IMAGE_ASPECT_DEPTH_BIT, false);
    NVSDK_NGX_Resource_VK motion = resource(frame.motion, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK transparency =
        resource(frame.transparencyMask, VK_IMAGE_ASPECT_COLOR_BIT, false);

    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &color;
    eval.Feature.pInOutput = &output;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion;
    eval.pInTransparencyMask = &transparency;
    // NGX takes the offset the jitter moved the image by: the negative of
    // the sample offset UpscalerFrame carries (flipping either sign measured
    // blurrier and less stable, decisions.md).
    eval.InJitterOffsetX = -frame.jitter[0];
    eval.InJitterOffsetY = -frame.jitter[1];
    eval.InRenderSubrectDimensions = {frame.color.extent.width, frame.color.extent.height};
    eval.InReset = frame.reset ? 1 : 0;
    eval.InMVScaleX = frame.motionScale[0];
    eval.InMVScaleY = frame.motionScale[1];
    eval.InFrameTimeDeltaInMsec = frame.frameSeconds * 1000.0f;
    DriverLeakScope driverLeak;
    check(NGX_VULKAN_EVALUATE_DLSS_EXT(frame.cmd, feature_, parameters_, &eval),
          "DLSS evaluate");
  }

  bool wantsGuides() const override { return false; }

private:
  NVSDK_NGX_Handle *feature_;
  NVSDK_NGX_Parameter *parameters_;
};

class DlssProvider final : public UpscalerProvider {
public:
  explicit DlssProvider(const std::filesystem::path &featureDirectory)
      : featurePath_(featureDirectory.wstring()),
        dataPath_(std::filesystem::temp_directory_path().wstring()) {
    featurePaths_[0] = featurePath_.data();
    common_.PathListInfo = {featurePaths_, 1};
    discovery_.SDKVersion = NVSDK_NGX_Version_API;
    discovery_.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    discovery_.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery_.Identifier.v.ProjectDesc = {kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                           kEngineVersion};
    discovery_.ApplicationDataPath = dataPath_.c_str();
    discovery_.FeatureInfo = &common_;
  }
  ~DlssProvider() override { shutdown(); }
  DlssProvider(const DlssProvider &) = delete;
  DlssProvider &operator=(const DlssProvider &) = delete;

  std::vector<const char *> instanceExtensions() override {
    uint32_t count = 0;
    VkExtensionProperties *properties = nullptr;
    DriverLeakScope driverLeak;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
            &discovery_, &count, &properties))) {
      return {};
    }
    return names(instanceExtensions_, properties, count);
  }

  std::vector<const char *> deviceExtensions(VkInstance instance,
                                             VkPhysicalDevice physicalDevice) override {
    uint32_t count = 0;
    VkExtensionProperties *properties = nullptr;
    DriverLeakScope driverLeak;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
            instance, physicalDevice, &discovery_, &count, &properties))) {
      return {};
    }
    return names(deviceExtensions_, properties, count);
  }

  void init(const UpscalerDevice &device) override {
    DriverLeakScope driverLeak;
    const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, dataPath_.c_str(),
        device.instance, device.physicalDevice, device.device, device.getInstanceProcAddr,
        device.getDeviceProcAddr, &common_);
    if (NVSDK_NGX_FAILED(result)) {
      std::printf("DLSS: NGX init failed (%s)\n", resultString(result).c_str());
      return;
    }
    device_ = device.device;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters_))) {
      std::printf("DLSS: NGX capability parameters unavailable\n");
      shutdown();
      return;
    }
    int superSampling = 0, rayReconstruction = 0, initResult = 0;
    NVSDK_NGX_Parameter_GetI(parameters_, NVSDK_NGX_Parameter_SuperSampling_Available,
                             &superSampling);
    NVSDK_NGX_Parameter_GetI(parameters_, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult,
                             &initResult);
    NVSDK_NGX_Parameter_GetI(parameters_, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available,
                             &rayReconstruction);
    superSampling_ = superSampling != 0;
    rayReconstruction_ = rayReconstruction != 0;
    const std::string superSamplingStatus =
        superSampling_ ? "available"
                       : "unavailable (" +
                             resultString(static_cast<NVSDK_NGX_Result>(initResult)) + ")";
    std::printf("DLSS: Super Resolution %s, Ray Reconstruction %s\n",
                superSamplingStatus.c_str(), rayReconstruction_ ? "available" : "unavailable");
  }

  void shutdown() override {
    if (!device_) {
      return;
    }
    if (parameters_) {
      NVSDK_NGX_VULKAN_DestroyParameters(parameters_);
      parameters_ = nullptr;
    }
    NVSDK_NGX_VULKAN_Shutdown1(device_);
    device_ = VK_NULL_HANDLE;
    superSampling_ = rayReconstruction_ = false;
  }

  bool supports(const AaSettings &settings) const override {
    return settings.method == AaMethod::Dlss && superSampling_ && !settings.rayReconstruction;
  }

  Extent renderExtent(const AaSettings &settings, Extent display) const override {
    if (settings.mode == DlssMode::Dlaa) {
      return display;
    }
    unsigned width = 0, height = 0, maxWidth = 0, maxHeight = 0, minWidth = 0, minHeight = 0;
    float sharpness = 0.0f;
    check(NGX_DLSS_GET_OPTIMAL_SETTINGS(parameters_, display.width, display.height,
                                        perfQuality(settings.mode), &width, &height,
                                        &maxWidth, &maxHeight, &minWidth, &minHeight,
                                        &sharpness),
          "DLSS optimal settings");
    return {width, height};
  }

  std::unique_ptr<Upscaler> create(const AaSettings &settings, Extent maxRender,
                                   Extent display, VkCommandBuffer cmd) override {
    // Letters map to NVSDK_NGX_DLSS_Hint_Render_Preset (A = 1); 0 keeps
    // NGX's per-mode default.
    const char preset = settings.preset ? settings.preset : defaultPreset(settings.mode);
    NVSDK_NGX_Parameter_SetUI(parameters_, presetParameter(settings.mode),
                              preset ? preset - 'A' + 1 : 0);
    NVSDK_NGX_DLSS_Create_Params params{};
    params.Feature.InWidth = maxRender.width;
    params.Feature.InHeight = maxRender.height;
    params.Feature.InTargetWidth = display.width;
    params.Feature.InTargetHeight = display.height;
    params.Feature.InPerfQualityValue = perfQuality(settings.mode);
    // Motion vectors at the render size and without jitter; reverse-Z.
    // Colour is pre-exposed, so a constant exposure of 1 would be exact;
    // DLSS's own auto-exposure measured the same (decisions.md) and needs
    // no texture, and presets L and M always use it.
    params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                  NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    NVSDK_NGX_Handle *feature = nullptr;
    {
      DriverLeakScope driverLeak;
      check(NGX_VULKAN_CREATE_DLSS_EXT1(device_, cmd, 1, 1, &feature, parameters_, &params),
            "DLSS feature creation");
    }
    return std::make_unique<DlssUpscaler>(feature, parameters_);
  }

private:
  static std::vector<const char *> names(std::vector<std::string> &storage,
                                         const VkExtensionProperties *properties,
                                         uint32_t count) {
    storage.clear();
    for (uint32_t i = 0; i < count; ++i) {
      storage.emplace_back(properties[i].extensionName);
    }
    std::vector<const char *> result;
    for (const std::string &name : storage) {
      result.push_back(name.c_str());
    }
    return result;
  }

  std::wstring featurePath_;
  std::wstring dataPath_;
  wchar_t *featurePaths_[1] = {};
  NVSDK_NGX_FeatureCommonInfo common_{};
  NVSDK_NGX_FeatureDiscoveryInfo discovery_{};
  std::vector<std::string> instanceExtensions_, deviceExtensions_;
  VkDevice device_ = VK_NULL_HANDLE;
  NVSDK_NGX_Parameter *parameters_ = nullptr;
  bool superSampling_ = false;
  bool rayReconstruction_ = false;
};

} // namespace

#ifdef MERIDIAN_HAS_LSAN
// VK_NVX_binary_import (an NGX device extension) makes the driver start
// CUDA, which maps memory in AddressSanitizer's shadow gap.
extern "C" const char *__asan_default_options() { return "protect_shadow_gap=0"; }
#endif

std::unique_ptr<UpscalerProvider> createProvider(const std::filesystem::path &featureDirectory) {
  return std::make_unique<DlssProvider>(featureDirectory);
}

const char *attribution() {
  return "NVIDIA DLSS: this product uses the NVIDIA DLSS SDK 310.9.16. NVIDIA, the "
         "NVIDIA logo, DLSS and GeForce RTX are trademarks or registered trademarks of "
         "NVIDIA Corporation in the U.S. and other countries.";
}

} // namespace meridian::dlss
