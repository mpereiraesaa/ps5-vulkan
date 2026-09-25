#include "wsi_platform.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace dxvk::wsi { extern WsiBootstrap Ps5WSI; }

namespace {

VkInstance instance = reinterpret_cast<VkInstance>(uintptr_t(1));
VkPhysicalDevice physical = reinterpret_cast<VkPhysicalDevice>(uintptr_t(2));
VkDisplayKHR display = reinterpret_cast<VkDisplayKHR>(uintptr_t(3));
VkDisplayModeKHR mode = reinterpret_cast<VkDisplayModeKHR>(uintptr_t(4));
VkSurfaceKHR created = reinterpret_cast<VkSurfaceKHR>(uintptr_t(5));
bool omitCreate = false;
bool wrongRefresh = false;
bool wrongAlpha = false;
unsigned createCalls = 0;

VKAPI_ATTR VkResult VKAPI_CALL physicalDevices(VkInstance i, uint32_t* count,
                                                VkPhysicalDevice* out) {
  assert(i == instance && count);
  if (out) { assert(*count >= 1); out[0] = physical; }
  *count = 1;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL displayProperties(VkPhysicalDevice p,
    uint32_t* count, VkDisplayPropertiesKHR* out) {
  assert(p == physical && count);
  if (out) {
    assert(*count >= 1);
    out[0] = {};
    out[0].display = display;
    out[0].physicalResolution = {1920, 1080};
  }
  *count = 1;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL modeProperties(VkPhysicalDevice p, VkDisplayKHR d,
    uint32_t* count, VkDisplayModePropertiesKHR* out) {
  assert(p == physical && d == display && count);
  if (out) {
    assert(*count >= 1);
    out[0] = {};
    out[0].displayMode = mode;
    out[0].parameters = {{1920, 1080}, wrongRefresh ? 50000u : 60000u};
  }
  *count = 1;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL planeProperties(VkPhysicalDevice p,
    uint32_t* count, VkDisplayPlanePropertiesKHR* out) {
  assert(p == physical && count);
  if (out) {
    assert(*count >= 1);
    out[0] = {display, 0};
  }
  *count = 1;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL supportedDisplays(VkPhysicalDevice p,
    uint32_t plane, uint32_t* count, VkDisplayKHR* out) {
  assert(p == physical && plane == 0 && count);
  if (out) { assert(*count >= 1); out[0] = display; }
  *count = 1;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL planeCapabilities(VkPhysicalDevice p,
    VkDisplayModeKHR m, uint32_t plane, VkDisplayPlaneCapabilitiesKHR* out) {
  assert(p == physical && m == mode && plane == 0 && out);
  *out = {};
  out->supportedAlpha = wrongAlpha ? 0 : VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL createSurface(VkInstance i,
    const VkDisplaySurfaceCreateInfoKHR* info, const VkAllocationCallbacks*,
    VkSurfaceKHR* out) {
  assert(i == instance && info && out);
  assert(info->sType == VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR);
  assert(info->displayMode == mode && info->planeIndex == 0);
  assert(info->planeStackIndex == 0);
  assert(info->transform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
  assert(info->alphaMode == VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR);
  assert(info->imageExtent.width == 1920 && info->imageExtent.height == 1080);
  ++createCalls;
  *out = created;
  return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lookup(VkInstance i, const char* name) {
  assert(i == instance && name);
#define ENTRY(symbol, func) \
  if (!std::strcmp(name, symbol)) return reinterpret_cast<PFN_vkVoidFunction>(func)
  ENTRY("vkEnumeratePhysicalDevices", physicalDevices);
  ENTRY("vkGetPhysicalDeviceDisplayPropertiesKHR", displayProperties);
  ENTRY("vkGetDisplayModePropertiesKHR", modeProperties);
  ENTRY("vkGetPhysicalDeviceDisplayPlanePropertiesKHR", planeProperties);
  ENTRY("vkGetDisplayPlaneSupportedDisplaysKHR", supportedDisplays);
  ENTRY("vkGetDisplayPlaneCapabilitiesKHR", planeCapabilities);
  if (omitCreate && !std::strcmp(name, "vkCreateDisplayPlaneSurfaceKHR"))
    return nullptr;
  ENTRY("vkCreateDisplayPlaneSurfaceKHR", createSurface);
#undef ENTRY
  return nullptr;
}

} // namespace

int main() {
  using namespace dxvk::wsi;
  WsiDriver* driver = nullptr;
  assert(Ps5WSI.createDriver(&driver) && driver);
  auto extensions = driver->getInstanceExtensions();
  auto hasExtension = [&](const char* name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](const char* value) { return !std::strcmp(value, name); });
  };
  assert(hasExtension(VK_KHR_SURFACE_EXTENSION_NAME));
  assert(hasExtension(VK_KHR_DISPLAY_EXTENSION_NAME));
  HMONITOR monitor = driver->getDefaultMonitor();
  assert(monitor && driver->enumMonitors(0) == monitor);
  assert(!driver->enumMonitors(1));
  WsiMode modeInfo = {};
  assert(driver->getDisplayMode(monitor, 0, &modeInfo));
  assert(modeInfo.width == 1920 && modeInfo.height == 1080);
  assert(modeInfo.refreshRate.numerator == 60000);
  assert(!driver->getDisplayMode(monitor, 1, &modeInfo));
  RECT rect = {};
  assert(driver->getDesktopCoordinates(monitor, &rect));
  assert(rect.right == 1920 && rect.bottom == 1080);
  HWND window = reinterpret_cast<HWND>(uintptr_t(9));
  assert(driver->isWindow(window) && driver->getWindowMonitor(window) == monitor);
  assert(driver->setWindowMode(monitor, window, nullptr, modeInfo));
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  assert(driver->createSurface(window, lookup, instance, &surface) == VK_SUCCESS);
  assert(surface == created && createCalls == 1);
  omitCreate = true;
  assert(driver->createSurface(window, lookup, instance, &surface) ==
         VK_ERROR_EXTENSION_NOT_PRESENT);
  assert(surface == VK_NULL_HANDLE && createCalls == 1);
  omitCreate = false;
  wrongRefresh = true;
  assert(driver->createSurface(window, lookup, instance, &surface) ==
         VK_ERROR_INITIALIZATION_FAILED);
  assert(createCalls == 1);
  wrongRefresh = false;
  wrongAlpha = true;
  assert(driver->createSurface(window, lookup, instance, &surface) ==
         VK_ERROR_INITIALIZATION_FAILED);
  assert(createCalls == 1);
  delete driver;
}
