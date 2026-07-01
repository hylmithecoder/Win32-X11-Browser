#ifndef BASEWINDOW_HPP
#define BASEWINDOW_HPP

#include "Paint.hpp"

#include <functional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winsock2.h>
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__linux__) || defined(__gnu_linux__)
#include <X11/Xlib.h>
#define VK_USE_PLATFORM_XLIB_KHR
#endif

#include <vulkan/vulkan.h>

namespace DesktopWebview {

// A native top-level window (X11 on Linux, Win32 on Windows) that presents a
// software-rendered Paint::Canvas via Vulkan. Content is produced on demand
// by a render callback so the window can re-render on resize.
class BaseWindow {
public:
  BaseWindow();
  ~BaseWindow();

  // Produces the page contents for a given client area size (width, height in
  // pixels). Invoked on first paint and whenever the window is resized.
  using RenderCallback = std::function<Paint::Canvas(int width, int height)>;

  void SetRenderCallback(RenderCallback callback);

  // A keyboard event delivered to the application (e.g. the address bar).
  struct Key {
    enum Kind { Char, Backspace, Enter, Left, Right, Up, Down, Tab, Delete };
    Kind kind = Char;
    char ch = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
  };
  // Invoked on text input / editing keys. Returning true requests a repaint.
  using KeyCallback = std::function<bool(const Key &)>;
  void SetKeyCallback(KeyCallback callback);

  // A mouse event delivered to the application (e.g. for link clicking and
  // drag-to-select). Move is only delivered while the primary button is held.
  struct MouseEvent {
    enum Kind { ButtonDown, ButtonUp, Move, ScrollUp, ScrollDown };
    Kind kind = ButtonDown;
    int x = 0;
    int y = 0;
  };
  using MouseCallback = std::function<bool(const MouseEvent &)>;
  void SetMouseCallback(MouseCallback callback);

  // Offer `text` as the contents of the system clipboard (X11 PRIMARY and
  // CLIPBOARD selections; the Windows clipboard). Called when a text selection
  // completes so the user can paste it elsewhere.
  void SetSelectionText(const std::string &text);

  // Open the window and run the event loop until the user closes it or presses
  // Escape. Blocks until the window closes.
  void Run();

private:
  RenderCallback m_render;
  KeyCallback m_key;
  MouseCallback m_mouse;
  int m_width = 1024;
  int m_height = 720;

  // Render the current content at the window size, honouring the DWV_DUMP_PPM
  // debug hook. Returns a canvas sized to the client area.
  Paint::Canvas RenderContent();

  // Vulkan graphics state
  VkInstance m_instance = VK_NULL_HANDLE;
  VkSurfaceKHR m_surface = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkDevice m_device = VK_NULL_HANDLE;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  uint32_t m_queueFamilyIndex = 0;
  VkCommandPool m_commandPool = VK_NULL_HANDLE;
  VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;

  // Swapchain state
  VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
  VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
  VkExtent2D m_swapchainExtent = {0, 0};
  std::vector<VkImage> m_swapchainImages;
  std::vector<VkImageView> m_swapchainImageViews;

  // Synchronization
  VkSemaphore m_imageAvailableSemaphore = VK_NULL_HANDLE;
  VkSemaphore m_renderFinishedSemaphore = VK_NULL_HANDLE;
  VkFence m_inFlightFence = VK_NULL_HANDLE;

  // Staging buffer for copy-to-screen presentation
  VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory m_stagingBufferMemory = VK_NULL_HANDLE;
  VkDeviceSize m_stagingBufferSize = 0;

  // Vulkan initialisation helpers
  bool InitVulkan();
  void CleanupVulkan();
  bool CreateSwapchain(int width, int height);
  void CleanupSwapchain();
  bool CreateStagingBuffer(VkDeviceSize size);
  void CleanupStagingBuffer();
  uint32_t FindMemoryType(uint32_t typeFilter,
                          VkMemoryPropertyFlags properties);

#if defined(_WIN32)
  HWND hwnd = nullptr;
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam);
  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  void Present(const Paint::Canvas &canvas);
#elif defined(__linux__) || defined(__gnu_linux__)
  Display *display = nullptr;
  Window window = 0;
  void Present(const Paint::Canvas &canvas);

  // Clipboard: we take ownership of the PRIMARY and CLIPBOARD selections and
  // serve `m_selectionText` in response to SelectionRequest events.
  std::string m_selectionText;
  Atom m_atomPrimary = 0;
  Atom m_atomClipboard = 0;
  Atom m_atomTargets = 0;
  Atom m_atomUtf8 = 0;
#endif
};

} // namespace DesktopWebview

#endif // BASEWINDOW_HPP
