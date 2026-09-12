#include "ps5vk/ps5vk_present.h"
#include <stdlib.h>
#include <string.h>

#if defined(__PROSPERO__) || (defined(PS5VK_TARGET_PS5) && PS5VK_TARGET_PS5)
#include "present_ps5.h"
#define PS5VK_HAS_NATIVE_PRESENT 1
#endif

struct ps5vk_present_surface_T {
    VkDevice device;
    struct ps5vk_present_config config;
    uint32_t image_count;
    VkImage images[2];
#if defined(PS5VK_HAS_NATIVE_PRESENT)
    struct ps5vk_native_present native;
#endif
};

VkResult ps5vkCreatePresentSurface(
    VkDevice device,
    const struct ps5vk_present_config *config,
    uint32_t image_count,
    const VkImage *images,
    ps5vk_present_surface *out_surface)
{
    if (!device || !config || !images || !out_surface) return VK_ERROR_INITIALIZATION_FAILED;
    if (image_count != 2 || config->buffer_count != 2) return VK_ERROR_INITIALIZATION_FAILED;
    if (config->width != 1920 || config->height != 1080) return VK_ERROR_INITIALIZATION_FAILED;
    if (config->format != VK_FORMAT_B8G8R8A8_UNORM) return VK_ERROR_INITIALIZATION_FAILED;
    if (!images[0] || !images[1]) return VK_ERROR_INITIALIZATION_FAILED;

    struct ps5vk_present_surface_T *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;

    s->device = device;
    s->config = *config;
    s->image_count = image_count;
    s->images[0] = images[0];
    s->images[1] = images[1];

#if defined(PS5VK_HAS_NATIVE_PRESENT)
    VkResult res = ps5vk_native_present_open(&s->native, device, images[0], images[1]);
    if (res != VK_SUCCESS) {
        free(s);
        return res;
    }
#endif

    *out_surface = s;
    return VK_SUCCESS;
}

VkResult ps5vkPresentFrame(
    ps5vk_present_surface surface,
    uint32_t buffer_index,
    uint64_t token)
{
    if (!surface || buffer_index >= surface->image_count) return VK_ERROR_DEVICE_LOST;

#if defined(PS5VK_HAS_NATIVE_PRESENT)
    /* The native adapter owns its bounded completion waits. Public frame
     * presentation never adds the demo-only post-present hold delay. */
    return ps5vk_native_present_frame(&surface->native, buffer_index, token, 0);
#else
    (void)token;
    return VK_SUCCESS;
#endif
}

void ps5vkDestroyPresentSurface(ps5vk_present_surface surface)
{
    if (!surface) return;

#if defined(PS5VK_HAS_NATIVE_PRESENT)
    ps5vk_native_present_close(&surface->native);
#endif

    free(surface);
}
