#include "wsi_present_backend.h"
#include <stdlib.h>

#if defined(__PROSPERO__) || (defined(PS5VK_TARGET_PS5) && PS5VK_TARGET_PS5)

#include "present_ps5.h"

struct ps5vk_wsi_present {
    struct ps5vk_native_present native;
};

VkBool32 ps5vk_wsi_present_available(void)
{
    return VK_TRUE;
}

VkResult ps5vk_wsi_present_open(VkDevice device, const VkImage images[2],
                               struct ps5vk_wsi_present **out)
{
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = NULL;
    if (!device || !images || !images[0] || !images[1])
        return VK_ERROR_INITIALIZATION_FAILED;
    struct ps5vk_wsi_present *present = calloc(1, sizeof(*present));
    if (!present) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult result = ps5vk_native_present_open(&present->native, device,
                                                images[0], images[1]);
    if (result != VK_SUCCESS) {
        free(present);
        return result;
    }
    *out = present;
    return VK_SUCCESS;
}

VkResult ps5vk_wsi_present_frame(struct ps5vk_wsi_present *present,
                                uint32_t slot, uint64_t token)
{
    if (!present) return VK_ERROR_INITIALIZATION_FAILED;
    return ps5vk_native_present_frame(&present->native, slot, token, 0);
}

VkResult ps5vk_wsi_present_close(struct ps5vk_wsi_present *present)
{
    if (!present) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = ps5vk_native_present_close(&present->native);
    if (result == VK_SUCCESS) free(present);
    return result;
}

#else

/* Host tests may replace these weak definitions with a controlled mock.
 * The ordinary host build cannot claim native display support. */
__attribute__((weak)) VkBool32 ps5vk_wsi_present_available(void)
{
    return VK_FALSE;
}

__attribute__((weak)) VkResult ps5vk_wsi_present_open(
    VkDevice device, const VkImage images[2], struct ps5vk_wsi_present **out)
{
    (void)device;
    (void)images;
    if (out) *out = NULL;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

__attribute__((weak)) VkResult ps5vk_wsi_present_frame(
    struct ps5vk_wsi_present *present, uint32_t slot, uint64_t token)
{
    (void)present;
    (void)slot;
    (void)token;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

__attribute__((weak)) VkResult ps5vk_wsi_present_close(
    struct ps5vk_wsi_present *present)
{
    (void)present;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

#endif
