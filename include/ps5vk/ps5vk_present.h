#ifndef PS5VK_PS5VK_PRESENT_H
#define PS5VK_PS5VK_PRESENT_H

#include "ps5vk.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal native presentation interface for PS5 VideoOut swapchain integration.
 */
typedef struct ps5vk_present_surface_T *ps5vk_present_surface;

struct ps5vk_present_config {
    uint32_t width;
    uint32_t height;
    VkFormat format;
    uint32_t buffer_count;
};

/*
 * Create/bind a presentation surface to the given device and display images.
 * On native PS5, binds 2 BGRA8 images (1920x1080) to VideoOut.
 */
VKAPI_ATTR VkResult VKAPI_CALL ps5vkCreatePresentSurface(
    VkDevice device,
    const struct ps5vk_present_config *config,
    uint32_t image_count,
    const VkImage *images,
    ps5vk_present_surface *out_surface);

/*
 * Present a frame: submits flip event for buffer_index (0 or 1).
 */
VKAPI_ATTR VkResult VKAPI_CALL ps5vkPresentFrame(
    ps5vk_present_surface surface,
    uint32_t buffer_index,
    uint64_t token,
    uint32_t timeout_us);

/*
 * Destroy and close the presentation surface, releasing VideoOut resources.
 */
VKAPI_ATTR void VKAPI_CALL ps5vkDestroyPresentSurface(
    ps5vk_present_surface surface);

#ifdef __cplusplus
}
#endif

#endif /* PS5VK_PS5VK_PRESENT_H */
