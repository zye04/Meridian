// The Vulkan side of the Upscaler: the extension point through which
// anti-aliasing / upscaling backends (TAA in the core, DLSS in its own
// module) plug into the renderer.
#pragma once

#include <meridian/math/mat4.h>
#include <meridian/upscaler.h>

#include <vulkan/vulkan.h>

#include <memory>
#include <vector>

namespace meridian {

struct UpscalerImage {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  Extent extent;
};

// Everything one evaluate call reads and writes. Inputs are in
// SHADER_READ_ONLY_OPTIMAL, `output` in GENERAL; earlier accesses to all of
// them are complete. Backends write `output` from compute shaders, and the
// caller makes those writes visible to what follows. Guide images
// (albedos, normal + roughness, specular hit distance) are null unless the
// backend wantsGuides().
struct UpscalerFrame {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  UpscalerImage color, depth, motion, transparencyMask, diffuseAlbedo,
      specularAlbedo, normalRoughness, specularHitDistance, output;
  // Render pixels. The projection gets 2 * jitter / renderSize added to
  // m[2][0] and m[2][1]; with w = -z_view and Vulkan's y-down framebuffer,
  // the image moves by -jitter pixels in x and y, so each pixel samples the
  // scene at its center + jitter.
  float jitter[2] = {};
  float motionScale[2] = {}; // render size: motion (UV units) to pixels
  Mat4 worldToView, viewToClip; // camera-relative, unjittered
  float frameSeconds = 0.0f;
  bool reset = false; // history is invalid (first frame, resize, cut)
};

class Upscaler {
public:
  virtual ~Upscaler() = default;
  virtual void evaluate(const UpscalerFrame &frame) = 0;
  virtual bool wantsGuides() const = 0;
};

struct UpscalerDevice {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
  PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
};

// Supplies an optional backend (DLSS). The renderer enables its instance
// extensions and those of its device extensions the device has before
// creating each, calls init once the device exists, and shutdown before it
// destroys the device, once every Upscaler from create is gone.
class UpscalerProvider {
public:
  virtual ~UpscalerProvider() = default;
  virtual std::vector<const char *> instanceExtensions() = 0;
  virtual std::vector<const char *> deviceExtensions(VkInstance,
                                                     VkPhysicalDevice) = 0;
  virtual void init(const UpscalerDevice &) = 0;
  virtual void shutdown() = 0;
  virtual bool supports(const AaSettings &) const = 0;
  virtual Extent renderExtent(const AaSettings &, Extent display) const = 0;
  virtual std::unique_ptr<Upscaler> create(const AaSettings &, Extent maxRender,
                                           Extent display,
                                           VkCommandBuffer cmd) = 0;
};

} // namespace meridian
