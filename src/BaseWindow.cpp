#include "../include/BaseWindow.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

// ===========================================================================
// Platform-independent members
// ===========================================================================
namespace DesktopWebview {

void BaseWindow::SetRenderCallback(RenderCallback callback) {
  m_render = std::move(callback);
}

void BaseWindow::SetKeyCallback(KeyCallback callback) {
  m_key = std::move(callback);
}

void BaseWindow::SetMouseCallback(MouseCallback callback) {
  m_mouse = std::move(callback);
}

void BaseWindow::SetSelectionText(const std::string &text) {
#if defined(_WIN32)
  if (text.empty() || !OpenClipboard(hwnd)) {
    return;
  }
  EmptyClipboard();
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
  if (mem) {
    char *dst = static_cast<char *>(GlobalLock(mem));
    std::memcpy(dst, text.c_str(), text.size() + 1);
    GlobalUnlock(mem);
    SetClipboardData(CF_TEXT, mem);
  }
  CloseClipboard();
#elif defined(__linux__) || defined(__gnu_linux__)
  // Become the owner of both selections; SelectionRequest events are then
  // served from m_selectionText in the event loop.
  m_selectionText = text;
  if (display && window) {
    XSetSelectionOwner(display, m_atomPrimary, window, CurrentTime);
    XSetSelectionOwner(display, m_atomClipboard, window, CurrentTime);
    XFlush(display);
  }
#else
  (void)text;
#endif
}

void BaseWindow::SetPasteCallback(PasteCallback callback) {
  m_paste = callback;
}

void BaseWindow::RequestPaste() {
#if defined(_WIN32)
  if (OpenClipboard(nullptr)) {
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h) {
      const char *p = static_cast<const char *>(GlobalLock(h));
      if (p) {
        m_pasteText = p;
        GlobalUnlock(h);
        if (m_paste) m_paste(m_pasteText);
      }
    }
    CloseClipboard();
  }
#elif defined(__linux__) || defined(__gnu_linux__)
  if (display && window && !m_pastePending) {
    m_pastePending = true;
    m_pasteText.clear();
    // Request the CLIPBOARD selection contents
    XConvertSelection(display, m_atomClipboard, m_atomUtf8, m_atomClipboard,
                      window, CurrentTime);
    XFlush(display);
  }
#endif
}

Paint::Canvas BaseWindow::RenderContent() {
  int w = m_width > 0 ? m_width : 1;
  int h = m_height > 0 ? m_height : 1;

  Paint::Canvas canvas = m_render ? m_render(w, h) : Paint::Canvas(w, h);
  if (!m_render) {
    canvas.clear(Paint::Color{255, 255, 255, 255});
  }

  // Debug hook: dump exactly what gets presented so the pipeline can be
  // verified headlessly.
  if (const char *dump = std::getenv("DWV_DUMP_PPM")) {
    canvas.savePPM(dump);
  }
  return canvas;
}

// ===========================================================================
// Vulkan helper methods (cross-platform)
// ===========================================================================

bool BaseWindow::InitVulkan() {
  // 1. Create Vulkan Instance
  VkApplicationInfo appInfo = {};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "DesktopWebview";
  appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.pEngineName = "No Engine";
  appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.apiVersion = VK_API_VERSION_1_0;

  std::vector<const char *> extensions = {VK_KHR_SURFACE_EXTENSION_NAME};
#if defined(_WIN32)
  extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(__linux__) || defined(__gnu_linux__)
  extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#endif

  VkInstanceCreateInfo createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  createInfo.ppEnabledExtensionNames = extensions.data();

  if (vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS) {
    std::cerr << "Failed to create Vulkan instance!" << std::endl;
    return false;
  }

  // 2. Create Window Surface
#if defined(_WIN32)
  VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {};
  surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
  surfaceCreateInfo.hwnd = hwnd;
  surfaceCreateInfo.hinstance = GetModuleHandle(nullptr);
  if (vkCreateWin32SurfaceKHR(m_instance, &surfaceCreateInfo, nullptr,
                              &m_surface) != VK_SUCCESS) {
    std::cerr << "Failed to create Win32 Vulkan surface!" << std::endl;
    return false;
  }
#elif defined(__linux__) || defined(__gnu_linux__)
  VkXlibSurfaceCreateInfoKHR surfaceCreateInfo = {};
  surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
  surfaceCreateInfo.dpy = display;
  surfaceCreateInfo.window = window;
  if (vkCreateXlibSurfaceKHR(m_instance, &surfaceCreateInfo, nullptr,
                             &m_surface) != VK_SUCCESS) {
    std::cerr << "Failed to create Xlib Vulkan surface!" << std::endl;
    return false;
  }
#endif

  // 3. Select Physical Device supporting swapchains and queues
  uint32_t deviceCount = 0;
  vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
  if (deviceCount == 0) {
    std::cerr << "Failed to find GPUs with Vulkan support!" << std::endl;
    return false;
  }
  std::vector<VkPhysicalDevice> devices(deviceCount);
  vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

  for (const auto &device : devices) {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
                                             nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
                                             queueFamilies.data());

    int graphicsFamily = -1;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
      VkBool32 presentSupport = false;
      vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface,
                                           &presentSupport);
      if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
          presentSupport) {
        graphicsFamily = i;
        break;
      }
    }

    if (graphicsFamily != -1) {
      uint32_t extensionCount;
      vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                           nullptr);
      std::vector<VkExtensionProperties> availableExtensions(extensionCount);
      vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                           availableExtensions.data());

      bool swapchainSupported = false;
      for (const auto &ext : availableExtensions) {
        if (std::string(ext.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME) {
          swapchainSupported = true;
          break;
        }
      }

      if (swapchainSupported) {
        m_physicalDevice = device;
        m_queueFamilyIndex = graphicsFamily;
        break;
      }
    }
  }

  if (m_physicalDevice == VK_NULL_HANDLE) {
    std::cerr << "Failed to find a suitable Vulkan physical device!"
              << std::endl;
    return false;
  }

  // 4. Create Logical Device
  float queuePriority = 1.0f;
  VkDeviceQueueCreateInfo queueCreateInfo = {};
  queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueCreateInfo.queueFamilyIndex = m_queueFamilyIndex;
  queueCreateInfo.queueCount = 1;
  queueCreateInfo.pQueuePriorities = &queuePriority;

  std::vector<const char *> deviceExtensions = {
      VK_KHR_SWAPCHAIN_EXTENSION_NAME};

  VkDeviceCreateInfo deviceCreateInfo = {};
  deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceCreateInfo.queueCreateInfoCount = 1;
  deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
  deviceCreateInfo.enabledExtensionCount =
      static_cast<uint32_t>(deviceExtensions.size());
  deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();

  if (vkCreateDevice(m_physicalDevice, &deviceCreateInfo, nullptr, &m_device) !=
      VK_SUCCESS) {
    std::cerr << "Failed to create Vulkan logical device!" << std::endl;
    return false;
  }

  vkGetDeviceQueue(m_device, m_queueFamilyIndex, 0, &m_graphicsQueue);

  // 5. Create Command Pool
  VkCommandPoolCreateInfo poolInfo = {};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = m_queueFamilyIndex;

  if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) !=
      VK_SUCCESS) {
    std::cerr << "Failed to create Vulkan command pool!" << std::endl;
    return false;
  }

  // 6. Allocate Command Buffer
  VkCommandBufferAllocateInfo allocInfo = {};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = m_commandPool;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  if (vkAllocateCommandBuffers(m_device, &allocInfo, &m_commandBuffer) !=
      VK_SUCCESS) {
    std::cerr << "Failed to allocate Vulkan command buffer!" << std::endl;
    return false;
  }

  // 7. Create Synchronization primitives
  VkSemaphoreCreateInfo semaphoreInfo = {};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fenceInfo = {};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr,
                        &m_imageAvailableSemaphore) != VK_SUCCESS ||
      vkCreateSemaphore(m_device, &semaphoreInfo, nullptr,
                        &m_renderFinishedSemaphore) != VK_SUCCESS ||
      vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFence) !=
          VK_SUCCESS) {
    std::cerr << "Failed to create Vulkan synchronization objects!"
              << std::endl;
    return false;
  }

  return true;
}

void BaseWindow::CleanupVulkan() {
  CleanupStagingBuffer();
  CleanupSwapchain();

  if (m_device != VK_NULL_HANDLE) {
    if (m_imageAvailableSemaphore != VK_NULL_HANDLE) {
      vkDestroySemaphore(m_device, m_imageAvailableSemaphore, nullptr);
      m_imageAvailableSemaphore = VK_NULL_HANDLE;
    }
    if (m_renderFinishedSemaphore != VK_NULL_HANDLE) {
      vkDestroySemaphore(m_device, m_renderFinishedSemaphore, nullptr);
      m_renderFinishedSemaphore = VK_NULL_HANDLE;
    }
    if (m_inFlightFence != VK_NULL_HANDLE) {
      vkDestroyFence(m_device, m_inFlightFence, nullptr);
      m_inFlightFence = VK_NULL_HANDLE;
    }
    if (m_commandPool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(m_device, m_commandPool, nullptr);
      m_commandPool = VK_NULL_HANDLE;
    }
    vkDestroyDevice(m_device, nullptr);
    m_device = VK_NULL_HANDLE;
  }

  if (m_instance != VK_NULL_HANDLE) {
    if (m_surface != VK_NULL_HANDLE) {
      vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
      m_surface = VK_NULL_HANDLE;
    }
    vkDestroyInstance(m_instance, nullptr);
    m_instance = VK_NULL_HANDLE;
  }
}

bool BaseWindow::CreateSwapchain(int width, int height) {
  uint32_t formatCount;
  vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface,
                                       &formatCount, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(formatCount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface,
                                       &formatCount, formats.data());

  VkSurfaceFormatKHR surfaceFormat = formats[0];
  for (const auto &f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      surfaceFormat = f;
      break;
    }
  }
  m_swapchainFormat = surfaceFormat.format;

  VkSurfaceCapabilitiesKHR capabilities;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface,
                                            &capabilities);

  VkExtent2D extent = {static_cast<uint32_t>(width),
                       static_cast<uint32_t>(height)};
  extent.width =
      std::max(capabilities.minImageExtent.width,
               std::min(capabilities.maxImageExtent.width, extent.width));
  extent.height =
      std::max(capabilities.minImageExtent.height,
               std::min(capabilities.maxImageExtent.height, extent.height));
  m_swapchainExtent = extent;

  uint32_t imageCount = capabilities.minImageCount + 1;
  if (capabilities.maxImageCount > 0 &&
      imageCount > capabilities.maxImageCount) {
    imageCount = capabilities.maxImageCount;
  }

  VkSwapchainCreateInfoKHR swapchainCreateInfo = {};
  swapchainCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  swapchainCreateInfo.surface = m_surface;
  swapchainCreateInfo.minImageCount = imageCount;
  swapchainCreateInfo.imageFormat = m_swapchainFormat;
  swapchainCreateInfo.imageColorSpace = surfaceFormat.colorSpace;
  swapchainCreateInfo.imageExtent = m_swapchainExtent;
  swapchainCreateInfo.imageArrayLayers = 1;
  swapchainCreateInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  swapchainCreateInfo.preTransform = capabilities.currentTransform;
  swapchainCreateInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  swapchainCreateInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  swapchainCreateInfo.clipped = VK_TRUE;
  swapchainCreateInfo.oldSwapchain = VK_NULL_HANDLE;

  if (vkCreateSwapchainKHR(m_device, &swapchainCreateInfo, nullptr,
                           &m_swapchain) != VK_SUCCESS) {
    std::cerr << "Failed to create Vulkan swapchain!" << std::endl;
    return false;
  }

  vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
  m_swapchainImages.resize(imageCount);
  vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount,
                          m_swapchainImages.data());

  m_swapchainImageViews.resize(imageCount);
  for (size_t i = 0; i < imageCount; ++i) {
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_swapchainImages[i];
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_swapchainFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr,
                          &m_swapchainImageViews[i]) != VK_SUCCESS) {
      std::cerr << "Failed to create Vulkan swapchain image view!" << std::endl;
      return false;
    }
  }

  return true;
}

void BaseWindow::CleanupSwapchain() {
  if (m_device != VK_NULL_HANDLE) {
    for (auto imageView : m_swapchainImageViews) {
      if (imageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, imageView, nullptr);
      }
    }
    m_swapchainImageViews.clear();

    if (m_swapchain != VK_NULL_HANDLE) {
      vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
      m_swapchain = VK_NULL_HANDLE;
    }
  }
}

bool BaseWindow::CreateStagingBuffer(VkDeviceSize size) {
  CleanupStagingBuffer();

  VkBufferCreateInfo bufferInfo = {};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_stagingBuffer) !=
      VK_SUCCESS) {
    std::cerr << "Failed to create Vulkan staging buffer!" << std::endl;
    return false;
  }

  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(m_device, m_stagingBuffer, &memRequirements);

  VkMemoryAllocateInfo allocInfo = {};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = FindMemoryType(
      memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_stagingBufferMemory) !=
      VK_SUCCESS) {
    std::cerr << "Failed to allocate Vulkan staging buffer memory!"
              << std::endl;
    return false;
  }

  vkBindBufferMemory(m_device, m_stagingBuffer, m_stagingBufferMemory, 0);
  m_stagingBufferSize = size;
  return true;
}

void BaseWindow::CleanupStagingBuffer() {
  if (m_device != VK_NULL_HANDLE) {
    if (m_stagingBuffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
      m_stagingBuffer = VK_NULL_HANDLE;
    }
    if (m_stagingBufferMemory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, m_stagingBufferMemory, nullptr);
      m_stagingBufferMemory = VK_NULL_HANDLE;
    }
  }
  m_stagingBufferSize = 0;
}

uint32_t BaseWindow::FindMemoryType(uint32_t typeFilter,
                                    VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties memProperties;
  vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

  for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
    if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags &
                                    properties) == properties) {
      return i;
    }
  }
  throw std::runtime_error("Failed to find suitable Vulkan memory type!");
}

void BaseWindow::Present(const Paint::Canvas &canvas) {
  int w = canvas.width();
  int h = canvas.height();
  if (w <= 0 || h <= 0) {
    return;
  }

  if (m_swapchain == VK_NULL_HANDLE || w != m_swapchainExtent.width ||
      h != m_swapchainExtent.height) {
    if (m_device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(m_device);
    }
    CleanupSwapchain();
    if (!CreateSwapchain(w, h)) {
      return;
    }
  }

  VkDeviceSize requiredSize = w * h * sizeof(std::uint32_t);
  if (m_stagingBuffer == VK_NULL_HANDLE || requiredSize > m_stagingBufferSize) {
    if (!CreateStagingBuffer(requiredSize)) {
      return;
    }
  }

  std::vector<std::uint32_t> packed = Paint::toPackedPixels(canvas);
  void *data = nullptr;
  if (vkMapMemory(m_device, m_stagingBufferMemory, 0, requiredSize, 0, &data) ==
      VK_SUCCESS) {
    std::memcpy(data, packed.data(), requiredSize);
    vkUnmapMemory(m_device, m_stagingBufferMemory);
  } else {
    std::cerr << "Failed to map staging buffer memory!" << std::endl;
    return;
  }

  vkWaitForFences(m_device, 1, &m_inFlightFence, VK_TRUE, UINT64_MAX);
  vkResetFences(m_device, 1, &m_inFlightFence);

  uint32_t imageIndex;
  VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
                                          m_imageAvailableSemaphore,
                                          VK_NULL_HANDLE, &imageIndex);
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
    vkDeviceWaitIdle(m_device);
    CleanupSwapchain();
    CreateSwapchain(w, h);
    return;
  } else if (result != VK_SUCCESS) {
    std::cerr << "Failed to acquire next swapchain image!" << std::endl;
    return;
  }

  vkResetCommandBuffer(m_commandBuffer, 0);
  VkCommandBufferBeginInfo beginInfo = {};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
    std::cerr << "Failed to begin command buffer recording!" << std::endl;
    return;
  }

  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = m_swapchainImages[imageIndex];
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

  vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);

  VkBufferImageCopy region = {};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};

  vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer,
                         m_swapchainImages[imageIndex],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = 0;

  vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);

  if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
    std::cerr << "Failed to end command buffer recording!" << std::endl;
    return;
  }

  VkSubmitInfo submitInfo = {};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  VkSemaphore waitSemaphores[] = {m_imageAvailableSemaphore};
  VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_TRANSFER_BIT};
  submitInfo.waitSemaphoreCount = 1;
  submitInfo.pWaitSemaphores = waitSemaphores;
  submitInfo.pWaitDstStageMask = waitStages;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &m_commandBuffer;
  VkSemaphore signalSemaphores[] = {m_renderFinishedSemaphore};
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = signalSemaphores;

  if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFence) !=
      VK_SUCCESS) {
    std::cerr << "Failed to submit Vulkan queue commands!" << std::endl;
    return;
  }

  VkPresentInfoKHR presentInfo = {};
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = signalSemaphores;
  VkSwapchainKHR swapchains[] = {m_swapchain};
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = swapchains;
  presentInfo.pImageIndices = &imageIndex;

  vkQueuePresentKHR(m_graphicsQueue, &presentInfo);
}

} // namespace DesktopWebview

// ===========================================================================
// Windows (Win32 + Vulkan) implementation
// ===========================================================================
#if defined(_WIN32)
namespace DesktopWebview {

BaseWindow::BaseWindow() {}

BaseWindow::~BaseWindow() {
  if (m_device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(m_device);
  }
  CleanupVulkan();
}

LRESULT CALLBACK BaseWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam) {
  BaseWindow *self = nullptr;
  if (msg == WM_NCCREATE) {
    CREATESTRUCT *create = reinterpret_cast<CREATESTRUCT *>(lParam);
    self = reinterpret_cast<BaseWindow *>(create->lpCreateParams);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self =
        reinterpret_cast<BaseWindow *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  }
  if (self) {
    return self->HandleMessage(hwnd, msg, wParam, lParam);
  }
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT BaseWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam) {
  switch (msg) {
  case WM_SIZE:
    m_width = LOWORD(lParam);
    m_height = HIWORD(lParam);
    InvalidateRect(hwnd, nullptr, FALSE);
    return 0;

  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    m_width = rc.right - rc.left;
    m_height = rc.bottom - rc.top;

    Present(RenderContent());

    EndPaint(hwnd, &ps);

    if (std::getenv("DWV_AUTO_CLOSE")) {
      DestroyWindow(hwnd);
    }
    return 0;
  }

  case WM_KEYDOWN:
    if (wParam == VK_ESCAPE) {
      DestroyWindow(hwnd);
    } else if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP ||
               wParam == VK_DOWN) {
      if (m_key) {
        Key e;
        if (wParam == VK_LEFT)
          e.kind = Key::Left;
        else if (wParam == VK_RIGHT)
          e.kind = Key::Right;
        else if (wParam == VK_UP)
          e.kind = Key::Up;
        else if (wParam == VK_DOWN)
          e.kind = Key::Down;
        if (m_key(e)) {
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
    }
    return 0;

  case WM_CHAR: {
    if (!m_key) {
      return 0;
    }
    Key e;
    char c = static_cast<char>(wParam);
    if (c == '\r') {
      e.kind = Key::Enter;
    } else if (c == '\b') {
      e.kind = Key::Backspace;
    } else if (c >= 32 && c < 127) {
      e.kind = Key::Char;
      e.ch = c;
    } else {
      return 0;
    }
    if (m_key(e)) {
      InvalidateRect(hwnd, nullptr, FALSE);
    }
    return 0;
  }

  case WM_LBUTTONDOWN: {
    if (m_mouse) {
      MouseEvent e;
      e.kind = MouseEvent::ButtonDown;
      e.x = LOWORD(lParam);
      e.y = HIWORD(lParam);
      if (m_mouse(e)) {
        InvalidateRect(hwnd, nullptr, FALSE);
      }
    }
    return 0;
  }

  case WM_MOUSEWHEEL: {
    if (m_mouse) {
      int delta = GET_WHEEL_DELTA_WPARAM(wParam);
      MouseEvent e;
      e.kind = (delta > 0) ? MouseEvent::ScrollUp : MouseEvent::ScrollDown;
      if (m_mouse(e)) {
        InvalidateRect(hwnd, nullptr, FALSE);
      }
    }
    return 0;
  }

  case WM_TIMER:
    InvalidateRect(hwnd, nullptr, FALSE);
    return 0;

  case WM_CLOSE:
    DestroyWindow(hwnd);
    return 0;

  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

void BaseWindow::Run() {
  HINSTANCE inst = GetModuleHandle(nullptr);

  WNDCLASSW wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = L"BaseWindowClass";
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

  if (!RegisterClassW(&wc)) {
    std::cerr << "Failed to register window class!" << std::endl;
    return;
  }

  hwnd = CreateWindowExW(0, L"BaseWindowClass", L"DesktopWebview",
                         WS_OVERLAPPEDWINDOW, 100, 100, m_width, m_height,
                         nullptr, nullptr, inst, this);
  if (!hwnd) {
    std::cerr << "Failed to create window!" << std::endl;
    return;
  }

  if (!InitVulkan()) {
    std::cerr << "Failed to initialize Vulkan!" << std::endl;
    DestroyWindow(hwnd);
    return;
  }

  ShowWindow(hwnd, SW_SHOWNORMAL);
  UpdateWindow(hwnd);

  SetTimer(hwnd, 1, 33, nullptr);
  MSG msg = {};
  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  KillTimer(hwnd, 1);
}

} // namespace DesktopWebview

// ===========================================================================
// Linux (X11 + Vulkan) implementation
// ===========================================================================
#elif defined(__linux__) || defined(__gnu_linux__)
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <chrono>
#include <thread>

namespace DesktopWebview {

BaseWindow::BaseWindow() : display(nullptr), window(0) {}

BaseWindow::~BaseWindow() {
  if (m_device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(m_device);
  }
  CleanupVulkan();
  if (display) {
    if (window) {
      XDestroyWindow(display, window);
    }
    XCloseDisplay(display);
    display = nullptr;
    window = 0;
  }
}

void BaseWindow::Run() {
  display = XOpenDisplay(nullptr);
  if (!display) {
    std::cerr << "Cannot open X display" << std::endl;
    return;
  }

  int screen = DefaultScreen(display);
  Window root = RootWindow(display, screen);
  window = XCreateSimpleWindow(display, root, 100, 100, m_width, m_height, 0,
                               BlackPixel(display, screen),
                               WhitePixel(display, screen));

  XStoreName(display, window, "DesktopWebview");
  XSelectInput(display, window,
               ExposureMask | KeyPressMask | ButtonPressMask |
                   ButtonReleaseMask | Button1MotionMask | StructureNotifyMask);

  // Clipboard selection atoms (for drag-to-select copy).
  m_atomPrimary = XA_PRIMARY;
  m_atomClipboard = XInternAtom(display, "CLIPBOARD", False);
  m_atomTargets = XInternAtom(display, "TARGETS", False);
  m_atomUtf8 = XInternAtom(display, "UTF8_STRING", False);

  Atom wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(display, window, &wmDelete, 1);

  XMapWindow(display, window);

  if (!InitVulkan()) {
    std::cerr << "Failed to initialize Vulkan!" << std::endl;
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    display = nullptr;
    window = 0;
    return;
  }

  const bool autoClose = std::getenv("DWV_AUTO_CLOSE") != nullptr;
  bool running = true;
  bool painted = false;
  XEvent event;

  while (running) {
    while (XPending(display)) {
      XNextEvent(display, &event);
      switch (event.type) {
      case Expose:
        if (event.xexpose.count == 0) {
          Present(RenderContent());
          if (autoClose) {
            running = false;
          }
        }
        break;

      case ConfigureNotify: {
        int nw = event.xconfigure.width;
        int nh = event.xconfigure.height;
        if (nw != m_width || nh != m_height) {
          m_width = nw;
          m_height = nh;
        }
        break;
      }

      case KeyPress: {
        char buf[8] = {0};
        KeySym ks = 0;
        int n = XLookupString(&event.xkey, buf, sizeof(buf), &ks, nullptr);
        if (ks == XK_Escape) {
          running = false;
        } else if (m_key) {
          Key e;
          e.ctrl = (event.xkey.state & ControlMask) != 0;
          e.shift = (event.xkey.state & ShiftMask) != 0;
          e.alt = (event.xkey.state & Mod1Mask) != 0;
          bool deliver = true;
          if (ks == XK_Return || ks == XK_KP_Enter) {
            e.kind = Key::Enter;
          } else if (ks == XK_BackSpace) {
            e.kind = Key::Backspace;
          } else if (ks == XK_Tab) {
            e.kind = Key::Tab;
          } else if (ks == XK_Delete) {
            e.kind = Key::Delete;
          } else if (ks == XK_Home) {
            e.kind = Key::Home;
          } else if (ks == XK_End) {
            e.kind = Key::End;
          } else if (ks == XK_Left) {
            e.kind = Key::Left;
          } else if (ks == XK_Right) {
            e.kind = Key::Right;
          } else if (ks == XK_Up) {
            e.kind = Key::Up;
          } else if (ks == XK_Down) {
            e.kind = Key::Down;
          } else if (e.ctrl) {
            // Ctrl+key: extract the base letter from keysym
            // XK_a=0x61 .. XK_z=0x7a, XK_t=0x74 etc.
            if (ks >= 0x61 && ks <= 0x7a) {
              e.kind = Key::Char;
              e.ch = static_cast<char>(ks);
            } else {
              deliver = false;
            }
          } else if (n == 1 && buf[0] >= 32 && buf[0] < 127) {
            e.kind = Key::Char;
            e.ch = buf[0];
          } else {
            deliver = false;
          }
          if (deliver) {
            m_key(e);
          }
        }
        break;
      }

      case ButtonPress: {
        if (m_mouse) {
          MouseEvent e;
          if (event.xbutton.button == Button1) {
            e.kind = MouseEvent::ButtonDown;
            e.x = event.xbutton.x;
            e.y = event.xbutton.y;
            m_mouse(e);
          } else if (event.xbutton.button == 4) {
            e.kind = MouseEvent::ScrollUp;
            m_mouse(e);
          } else if (event.xbutton.button == 5) {
            e.kind = MouseEvent::ScrollDown;
            m_mouse(e);
          }
        }
        break;
      }

      case ButtonRelease: {
        if (m_mouse && event.xbutton.button == Button1) {
          MouseEvent e;
          e.kind = MouseEvent::ButtonUp;
          e.x = event.xbutton.x;
          e.y = event.xbutton.y;
          m_mouse(e);
        }
        break;
      }

      case MotionNotify: {
        if (m_mouse) {
          // Coalesce queued motion events; only the latest position matters.
          while (XPending(display)) {
            XEvent next;
            XPeekEvent(display, &next);
            if (next.type != MotionNotify) {
              break;
            }
            XNextEvent(display, &event);
          }
          MouseEvent e;
          e.kind = MouseEvent::Move;
          e.x = event.xmotion.x;
          e.y = event.xmotion.y;
          m_mouse(e);
        }
        break;
      }

      case SelectionNotify: {
        // Response to our XConvertSelection (paste request)
        if (event.xselection.selection == m_atomClipboard &&
            event.xselection.property != None) {
          Atom actualType;
          int actualFormat;
          unsigned long itemCount, bytesAfter;
          unsigned char *data = nullptr;
          if (XGetWindowProperty(display, window, event.xselection.property, 0,
                                 1024 * 1024, True, AnyPropertyType,
                                 &actualType, &actualFormat, &itemCount,
                                 &bytesAfter, &data) == Success &&
              data && itemCount > 0) {
            m_pasteText.assign(reinterpret_cast<char *>(data), itemCount);
            if (m_paste) m_paste(m_pasteText);
          }
          if (data) XFree(data);
          XDeleteProperty(display, window, event.xselection.property);
        }
        m_pastePending = false;
        break;
      }

      case SelectionRequest: {
        // We own PRIMARY/CLIPBOARD: serve our selection text to the requestor.
        const XSelectionRequestEvent &req = event.xselectionrequest;
        XSelectionEvent resp = {};
        resp.type = SelectionNotify;
        resp.display = req.display;
        resp.requestor = req.requestor;
        resp.selection = req.selection;
        resp.target = req.target;
        resp.time = req.time;
        resp.property = req.property ? req.property : req.target;
        if (req.target == m_atomTargets) {
          Atom targets[] = {m_atomTargets, m_atomUtf8, XA_STRING};
          XChangeProperty(display, req.requestor, resp.property, XA_ATOM, 32,
                          PropModeReplace,
                          reinterpret_cast<unsigned char *>(targets),
                          sizeof(targets) / sizeof(targets[0]));
        } else if (req.target == m_atomUtf8 || req.target == XA_STRING) {
          XChangeProperty(
              display, req.requestor, resp.property, req.target, 8,
              PropModeReplace,
              reinterpret_cast<const unsigned char *>(m_selectionText.data()),
              static_cast<int>(m_selectionText.size()));
        } else {
          resp.property = None; // unsupported target
        }
        XSendEvent(display, req.requestor, True, NoEventMask,
                   reinterpret_cast<XEvent *>(&resp));
        break;
      }

      case ClientMessage:
        if (static_cast<Atom>(event.xclient.data.l[0]) == wmDelete) {
          running = false;
        }
        break;
      }
    }

    if (running) {
      Present(RenderContent());
      std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
  }
}

} // namespace DesktopWebview
#endif
