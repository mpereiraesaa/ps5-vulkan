// DXVK v2.6.2 native WSI adapter for the fixed PS5 VideoOut display.
// The Vulkan driver must implement VK_KHR_surface, VK_KHR_display and
// VK_KHR_swapchain before this adapter can present anything.
#include "wsi_platform.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dxvk::wsi {

namespace {

constexpr uint32_t Width = 1920;
constexpr uint32_t Height = 1080;
constexpr uint32_t RefreshMilliHz = 60000;

HMONITOR monitor() {
  return reinterpret_cast<HMONITOR>(uintptr_t(1));
}

bool validMonitor(HMONITOR value) {
  return value == monitor();
}

WsiMode fixedMode() {
  return WsiMode{Width, Height, {RefreshMilliHz, 1000}, 32, false};
}

template<typename T>
T command(PFN_vkGetInstanceProcAddr get, VkInstance instance, const char* name) {
  return reinterpret_cast<T>(get(instance, name));
}

class Ps5WsiDriver final : public WsiDriver {
public:
  std::vector<const char*> getInstanceExtensions() override {
    return {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
  }

  HMONITOR getDefaultMonitor() override { return monitor(); }
  HMONITOR enumMonitors(uint32_t index) override {
    return index == 0 ? monitor() : nullptr;
  }
  HMONITOR enumMonitors(const LUID*[], uint32_t, uint32_t index) override {
    return enumMonitors(index);
  }
  bool getDisplayName(HMONITOR value, WCHAR (&name)[32]) override {
    if (!validMonitor(value)) return false;
    std::fill(std::begin(name), std::end(name), WCHAR(0));
    constexpr char label[] = "PS5 VideoOut";
    for (size_t i = 0; i < sizeof(label) - 1; ++i)
      name[i] = WCHAR(label[i]);
    return true;
  }
  bool getDesktopCoordinates(HMONITOR value, RECT* rect) override {
    if (!validMonitor(value) || !rect) return false;
    *rect = RECT{0, 0, LONG(Width), LONG(Height)};
    return true;
  }
  bool getDisplayMode(HMONITOR value, uint32_t number, WsiMode* mode) override {
    if (!validMonitor(value) || number != 0 || !mode) return false;
    *mode = fixedMode();
    return true;
  }
  bool getCurrentDisplayMode(HMONITOR value, WsiMode* mode) override {
    return getDisplayMode(value, 0, mode);
  }
  bool getDesktopDisplayMode(HMONITOR value, WsiMode* mode) override {
    return getDisplayMode(value, 0, mode);
  }
  WsiEdidData getMonitorEdid(HMONITOR) override { return {}; }

  void getWindowSize(HWND window, uint32_t* width, uint32_t* height) override {
    if (width) *width = window ? Width : 0;
    if (height) *height = window ? Height : 0;
  }
  void resizeWindow(HWND, DxvkWindowState*, uint32_t, uint32_t) override {
    // VideoOut has one fixed extent. A later size query returns that extent.
  }
  bool setWindowMode(HMONITOR value, HWND window, DxvkWindowState*,
                     const WsiMode& mode) override {
    return validMonitor(value) && window && mode.width == Width &&
           mode.height == Height && mode.refreshRate.denominator &&
           uint64_t(mode.refreshRate.numerator) * 1000 ==
               uint64_t(RefreshMilliHz) * mode.refreshRate.denominator;
  }
  bool enterFullscreenMode(HMONITOR value, HWND window, DxvkWindowState*,
                           bool) override {
    return validMonitor(value) && window;
  }
  bool leaveFullscreenMode(HWND, DxvkWindowState*, bool) override {
    return false; // VideoOut cannot switch to a windowed mode.
  }
  bool restoreDisplayMode() override { return true; }
  HMONITOR getWindowMonitor(HWND window) override {
    return window ? monitor() : nullptr;
  }
  bool isWindow(HWND window) override { return window != nullptr; }
  bool isMinimized(HWND) override { return false; }
  bool isOccluded(HWND) override { return false; }
  void updateFullscreenWindow(HMONITOR, HWND, bool) override { }

  VkResult createSurface(HWND window, PFN_vkGetInstanceProcAddr get,
                         VkInstance instance, VkSurfaceKHR* surface) override {
    if (!window || !get || !instance || !surface)
      return VK_ERROR_INITIALIZATION_FAILED;
    *surface = VK_NULL_HANDLE;

    const auto physicalDevices = command<PFN_vkEnumeratePhysicalDevices>(
        get, instance, "vkEnumeratePhysicalDevices");
    const auto displays = command<PFN_vkGetPhysicalDeviceDisplayPropertiesKHR>(
        get, instance, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    const auto modes = command<PFN_vkGetDisplayModePropertiesKHR>(
        get, instance, "vkGetDisplayModePropertiesKHR");
    const auto planes = command<PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR>(
        get, instance, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR");
    const auto supported = command<PFN_vkGetDisplayPlaneSupportedDisplaysKHR>(
        get, instance, "vkGetDisplayPlaneSupportedDisplaysKHR");
    const auto capabilities = command<PFN_vkGetDisplayPlaneCapabilitiesKHR>(
        get, instance, "vkGetDisplayPlaneCapabilitiesKHR");
    const auto create = command<PFN_vkCreateDisplayPlaneSurfaceKHR>(
        get, instance, "vkCreateDisplayPlaneSurfaceKHR");
    if (!physicalDevices || !displays || !modes || !planes || !supported ||
        !capabilities || !create)
      return VK_ERROR_EXTENSION_NOT_PRESENT;

    uint32_t count = 0;
    VkResult result = physicalDevices(instance, &count, nullptr);
    if (result != VK_SUCCESS || count != 1)
      return VK_ERROR_INITIALIZATION_FAILED;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    result = physicalDevices(instance, &count, &physical);
    if (result != VK_SUCCESS || count != 1 || !physical)
      return VK_ERROR_INITIALIZATION_FAILED;

    result = displays(physical, &count, nullptr);
    if (result != VK_SUCCESS || !count)
      return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkDisplayPropertiesKHR> displayProperties(count);
    result = displays(physical, &count, displayProperties.data());
    if (result != VK_SUCCESS)
      return result;
    for (uint32_t displayIndex = 0; displayIndex < count; ++displayIndex) {
      const auto& display = displayProperties[displayIndex];
      if (display.physicalResolution.width != Width ||
          display.physicalResolution.height != Height)
        continue;
      uint32_t modeCount = 0;
      result = modes(physical, display.display, &modeCount, nullptr);
      if (result != VK_SUCCESS || !modeCount)
        continue;
      std::vector<VkDisplayModePropertiesKHR> modeProperties(modeCount);
      result = modes(physical, display.display, &modeCount, modeProperties.data());
      if (result != VK_SUCCESS)
        continue;
      for (uint32_t modeIndex = 0; modeIndex < modeCount; ++modeIndex) {
        const auto& mode = modeProperties[modeIndex];
        if (mode.parameters.visibleRegion.width != Width ||
            mode.parameters.visibleRegion.height != Height ||
            mode.parameters.refreshRate != RefreshMilliHz)
          continue;
        uint32_t planeCount = 0;
        result = planes(physical, &planeCount, nullptr);
        if (result != VK_SUCCESS || !planeCount)
          continue;
        std::vector<VkDisplayPlanePropertiesKHR> planeProperties(planeCount);
        result = planes(physical, &planeCount, planeProperties.data());
        if (result != VK_SUCCESS)
          continue;
        for (uint32_t plane = 0; plane < planeCount; ++plane) {
          uint32_t supportedCount = 0;
          result = supported(physical, plane, &supportedCount, nullptr);
          if (result != VK_SUCCESS || !supportedCount)
            continue;
          std::vector<VkDisplayKHR> planeDisplays(supportedCount);
          result = supported(physical, plane, &supportedCount,
                             planeDisplays.data());
          if (result != VK_SUCCESS ||
              std::find(planeDisplays.begin(), planeDisplays.end(),
                        display.display) == planeDisplays.end())
            continue;
          VkDisplayPlaneCapabilitiesKHR caps = {};
          result = capabilities(physical, mode.displayMode, plane, &caps);
          if (result != VK_SUCCESS ||
              !(caps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR))
            continue;
          VkDisplaySurfaceCreateInfoKHR info = {};
          info.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
          info.displayMode = mode.displayMode;
          info.planeIndex = plane;
          info.planeStackIndex = planeProperties[plane].currentStackIndex;
          info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
          info.globalAlpha = 1.0f;
          info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
          info.imageExtent = {Width, Height};
          return create(instance, &info, nullptr, surface);
        }
      }
    }
    return VK_ERROR_INITIALIZATION_FAILED;
  }
};

bool createDriver(WsiDriver** driver) {
  if (!driver) return false;
  *driver = new Ps5WsiDriver();
  return true;
}

} // namespace

WsiBootstrap Ps5WSI = {"PS5", createDriver};

} // namespace dxvk::wsi
