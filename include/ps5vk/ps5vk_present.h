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

#ifdef __cplusplus
}
#endif

#endif /* PS5VK_PS5VK_PRESENT_H */
