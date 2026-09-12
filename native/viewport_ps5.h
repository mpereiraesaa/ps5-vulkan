#ifndef PS5VK_VIEWPORT_PS5_H
#define PS5VK_VIEWPORT_PS5_H
#include <vulkan/vulkan_core.h>
#include "ps5_agc.h"
enum { PS5VK_VIEWPORT_REGISTERS = 10 };
/* Compact context offsets, gfx1013. No register publication/submission here.
 * Scissor is intersected with render area; zero extent clips every fragment. */
VkResult ps5vk_native_viewport(const VkViewport *, const VkRect2D *scissor,
    const VkRect2D *render_area, ps5_agc_register out[PS5VK_VIEWPORT_REGISTERS]);
#endif
