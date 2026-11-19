#include <meridian/renderer.h>
#include <meridian/window.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>

#include <iostream>
#include <stdexcept>
#include <vector>

namespace meridian {

struct Renderer::Impl {
  vkb::Instance instance;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  vkb::Device device;

  VkQueue graphicsQueue = VK_NULL_HANDLE;
  uint32_t graphicsQueueFamily = 0;

  vkb::Swapchain swapchain;
  std::vector<VkImage> swapchainImages;
  std::vector<VkImageView> swapchainImageViews;

  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

  VkFence renderFence = VK_NULL_HANDLE;
  VkSemaphore imageAvailable = VK_NULL_HANDLE;
  // One per swapchain image: the presentation engine may still be reading a
  // semaphore after our fence signals, so it can't be shared across images.
  std::vector<VkSemaphore> renderFinished;
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

  impl_->graphicsQueue =
      impl_->device.get_queue(vkb::QueueType::graphics).value();
  impl_->graphicsQueueFamily =
      impl_->device.get_queue_index(vkb::QueueType::graphics).value();

  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window.handle(), &width, &height);

  auto swapchain =
      vkb::SwapchainBuilder{impl_->device}
          .set_desired_format({VK_FORMAT_B8G8R8A8_SRGB,
                               VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
          .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
          .set_desired_extent(width, height)
          .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
          .build();
  if (!swapchain) {
    throw std::runtime_error(swapchain.error().message());
  }
  impl_->swapchain = swapchain.value();
  impl_->swapchainImages = impl_->swapchain.get_images().value();
  impl_->swapchainImageViews = impl_->swapchain.get_image_views().value();

  VkDevice dev = impl_->device.device;

  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = impl_->graphicsQueueFamily;
  if (vkCreateCommandPool(dev, &poolInfo, nullptr, &impl_->commandPool) !=
      VK_SUCCESS) {
    throw std::runtime_error("vkCreateCommandPool failed");
  }

  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = impl_->commandPool;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(dev, &allocInfo, &impl_->commandBuffer) !=
      VK_SUCCESS) {
    throw std::runtime_error("vkAllocateCommandBuffers failed");
  }

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  if (vkCreateFence(dev, &fenceInfo, nullptr, &impl_->renderFence) !=
      VK_SUCCESS) {
    throw std::runtime_error("vkCreateFence failed");
  }

  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  if (vkCreateSemaphore(dev, &semaphoreInfo, nullptr,
                        &impl_->imageAvailable) != VK_SUCCESS) {
    throw std::runtime_error("vkCreateSemaphore failed");
  }
  impl_->renderFinished.resize(impl_->swapchainImages.size());
  for (VkSemaphore &semaphore : impl_->renderFinished) {
    if (vkCreateSemaphore(dev, &semaphoreInfo, nullptr, &semaphore) !=
        VK_SUCCESS) {
      throw std::runtime_error("vkCreateSemaphore failed");
    }
  }

  std::cout << "GPU: " << physical.value().name << "\n";
  std::cout << "Swapchain: " << impl_->swapchainImages.size() << " images, "
            << impl_->swapchain.extent.width << "x"
            << impl_->swapchain.extent.height << "\n";
}

Renderer::~Renderer() {
  VkDevice dev = impl_->device.device;
  vkDeviceWaitIdle(dev);

  for (VkSemaphore semaphore : impl_->renderFinished) {
    vkDestroySemaphore(dev, semaphore, nullptr);
  }
  vkDestroySemaphore(dev, impl_->imageAvailable, nullptr);
  vkDestroyFence(dev, impl_->renderFence, nullptr);
  vkDestroyCommandPool(dev, impl_->commandPool, nullptr);

  impl_->swapchain.destroy_image_views(impl_->swapchainImageViews);
  vkb::destroy_swapchain(impl_->swapchain);

  vkb::destroy_device(impl_->device);
  vkb::destroy_surface(impl_->instance, impl_->surface);
  vkb::destroy_instance(impl_->instance);
}

} // namespace meridian
