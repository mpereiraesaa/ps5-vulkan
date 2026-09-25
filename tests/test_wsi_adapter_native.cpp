/* Public Vulkan entrypoint route through the pinned DXVK PS5 WSI adapter. */
#include "wsi_platform.h"

#include <cstdint>

namespace dxvk::wsi { extern WsiBootstrap Ps5WSI; }

extern "C" VkResult dxvk_witness_create_surface(VkInstance instance,
                                                  VkSurfaceKHR* surface)
{
    if (!instance || !surface) return VK_ERROR_INITIALIZATION_FAILED;
    *surface = VK_NULL_HANDLE;
    dxvk::wsi::WsiDriver* driver = nullptr;
    if (!dxvk::wsi::Ps5WSI.createDriver(&driver) || !driver)
        return VK_ERROR_INITIALIZATION_FAILED;
    const auto window = reinterpret_cast<HWND>(uintptr_t(1));
    const VkResult result = driver->createSurface(
        window, vkGetInstanceProcAddr, instance, surface);
    delete driver;
    return result;
}
