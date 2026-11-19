#include <meridian/renderer.h>
#include <meridian/window.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>

#include <iostream>
#include <stdexcept>

namespace meridian {

struct Renderer::Impl {
  vkb::Instance instance;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  vkb::Device device;
};

Renderer::Renderer(Window &window) : impl_(std::make_unique<Impl>()) {
  auto instance = vkb::InstanceBuilder{}
                      .set_app_name("Meridian")
                      .request_validation_layers()
                      .use_default_debug_messenger()
                      .require_api_version(1, 3, 0)
                      .build();
  if (!instance) {
    throw std::runtime_error(instance.error().message());
  }
  impl_->instance = instance.value();

  if (glfwCreateWindowSurface(impl_->instance.instance, window.handle(),
                              nullptr, &impl_->surface) != VK_SUCCESS) {
    throw std::runtime_error("glfwCreateWindowSurface failed");
  }

  VkPhysicalDeviceVulkan13Features features13{};
  features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  features13.dynamicRendering = VK_TRUE;
  features13.synchronization2 = VK_TRUE;

  auto physical = vkb::PhysicalDeviceSelector{impl_->instance}
                      .set_minimum_version(1, 3)
                      .set_required_features_13(features13)
                      .set_surface(impl_->surface)
                      .select();
  if (!physical) {
    throw std::runtime_error(physical.error().message());
  }

  auto device = vkb::DeviceBuilder{physical.value()}.build();
  if (!device) {
    throw std::runtime_error(device.error().message());
  }
  impl_->device = device.value();

  std::cout << "GPU: " << physical.value().name << "\n";
}

Renderer::~Renderer() {
  vkb::destroy_device(impl_->device);
  vkb::destroy_surface(impl_->instance, impl_->surface);
  vkb::destroy_instance(impl_->instance);
}

} // namespace meridian
