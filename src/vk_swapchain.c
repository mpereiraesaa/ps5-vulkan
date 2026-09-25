#include "vk_image.h"
#include "vk_queue.h"
#include "wsi_present_backend.h"

#define INVALID VK_ERROR_UNKNOWN
#define WSI_MEMORY_BYTES UINT64_C(0x08000000)
#define WSI_SECOND_OFFSET UINT64_C(0x04000000)
#define WSI_QUEUE_TIMEOUT_NS UINT64_C(3000000000)

struct VkSwapchainKHR_T {
    VkDevice device;
    VkSurfaceKHR surface;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    VkImage images[2];
    VkDeviceMemory memory;
    struct ps5vk_wsi_present *present;
    VkBool32 acquired[2];
    uint64_t token;
    struct VkSwapchainKHR_T *next;
};

static VkBool32 live_swapchain(VkDevice d, VkSwapchainKHR swapchain)
{
    for (VkSwapchainKHR current = d ? d->swapchains : VK_NULL_HANDLE;
         current; current = current->next)
        if (current == swapchain) return VK_TRUE;
    return VK_FALSE;
}

static VkBool32 live_surface(VkInstance instance, VkSurfaceKHR surface)
{
    for (VkSurfaceKHR current = instance ? instance->surfaces : VK_NULL_HANDLE;
         current; current = current->next)
        if (current == surface) return VK_TRUE;
    return VK_FALSE;
}

static void release_images(VkSwapchainKHR swapchain)
{
    if (!swapchain) return;
    VkDevice d = swapchain->device;
    for (unsigned n = 0; n < 2; ++n)
        if (swapchain->images[n]) {
            vkDestroyImage(d, swapchain->images[n], NULL);
            swapchain->images[n] = VK_NULL_HANDLE;
        }
    if (swapchain->memory) {
        vkFreeMemory(d, swapchain->memory, NULL);
        swapchain->memory = VK_NULL_HANDLE;
    }
}

/* A present must follow all preceding queue writes, but an unbounded idle
 * wait would hide a stalled GPU and prevent the caller from closing the title. */
static VkResult drain_before_present(VkDevice d)
{
    if (!d->progress.clock_ns || !d->progress.pause) return VK_ERROR_DEVICE_LOST;
    const uint64_t start = d->progress.clock_ns(d->progress.context);
    uint32_t polls = 0;
    for (;;) {
        ps5vk_device_lock(d);
        const VkBool32 pending = d->submission != NULL;
        ps5vk_device_unlock(d);
        if (!pending) return VK_SUCCESS;
        VkResult result = ps5vk_queue_poll(d);
        if (result != VK_SUCCESS) return result;
        const uint64_t now = d->progress.clock_ns(d->progress.context);
        if (now - start >= WSI_QUEUE_TIMEOUT_NS || ++polls >= 3000)
            return VK_ERROR_DEVICE_LOST;
        d->progress.pause(d->progress.context, UINT64_C(1000000));
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice d,
    const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *allocator,
    VkSwapchainKHR *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR ||
        !d->swapchain_extension_enabled || !ps5vk_wsi_present_available())
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkSurfaceKHR surface = info->surface;
    if (!live_surface(d->physical->instance, surface) ||
        surface->instance != d->physical->instance ||
        !surface->instance->surface_extension_enabled)
        return VK_ERROR_SURFACE_LOST_KHR;
    if (surface->swapchains || d->swapchains)
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    if (!d->graphics_enabled || !d->graphics_submit_enabled ||
        !d->image_requirements) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->pNext || info->flags || info->oldSwapchain ||
        info->minImageCount != 2 ||
        info->imageFormat != VK_FORMAT_B8G8R8A8_UNORM ||
        info->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
        info->imageExtent.width != 1920 || info->imageExtent.height != 1080 ||
        info->imageArrayLayers != 1 ||
        !info->imageUsage ||
        (info->imageUsage & ~(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ||
        info->imageSharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        info->queueFamilyIndexCount || info->pQueueFamilyIndices ||
        info->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
        info->compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ||
        info->presentMode != VK_PRESENT_MODE_FIFO_KHR || !info->clipped)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkAllocationCallbacks saved = {0};
    VkBool32 custom = VK_FALSE;
    VkSwapchainKHR chain = ps5vk_object_alloc(
        d->custom_allocator ? &d->allocator : NULL, allocator, sizeof(*chain),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!chain) return VK_ERROR_OUT_OF_HOST_MEMORY;
    chain->device = d;
    chain->surface = surface;
    chain->allocator = saved;
    chain->custom_allocator = custom;
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = {1920, 1080, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = info->imageUsage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult result = VK_SUCCESS;
    for (unsigned n = 0; n < 2; ++n) {
        result = vkCreateImage(d, &image_info, NULL, &chain->images[n]);
        if (result != VK_SUCCESS) goto fail;
        chain->images[n]->swapchain_owned = VK_TRUE;
        /* The first acquisition has undefined contents, while importers such
         * as DXVK track the WSI image in PRESENT_SRC_KHR from the outset. */
        chain->images[n]->layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(d, chain->images[n], &requirements);
        if (!requirements.size || requirements.size > WSI_SECOND_OFFSET ||
            requirements.alignment > WSI_SECOND_OFFSET ||
            WSI_SECOND_OFFSET % requirements.alignment) {
            result = VK_ERROR_FORMAT_NOT_SUPPORTED;
            goto fail;
        }
    }
    VkMemoryAllocateInfo memory_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = WSI_MEMORY_BYTES,
        .memoryTypeIndex = 0,
    };
    result = vkAllocateMemory(d, &memory_info, NULL, &chain->memory);
    if (result != VK_SUCCESS) goto fail;
    result = vkBindImageMemory(d, chain->images[0], chain->memory, 0);
    if (result != VK_SUCCESS) goto fail;
    result = vkBindImageMemory(d, chain->images[1], chain->memory,
                               WSI_SECOND_OFFSET);
    if (result != VK_SUCCESS) goto fail;
    result = ps5vk_wsi_present_open(d, chain->images, &chain->present);
    if (result != VK_SUCCESS) goto fail;
    chain->next = d->swapchains;
    d->swapchains = chain;
    ++surface->swapchains;
    *out = chain;
    return VK_SUCCESS;
fail:
    release_images(chain);
    ps5vk_object_free(chain, &saved, custom);
    return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice d,
    VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!swapchain) return;
    if (!live_swapchain(d, swapchain)) return;
    for (unsigned n = 0; n < 2; ++n)
        if (swapchain->images[n]->views || swapchain->images[n]->pending != 1) {
            ++d->lifetime_errors;
            return;
        }
    VkResult result = ps5vk_wsi_present_close(swapchain->present);
    if (result != VK_SUCCESS) { ++d->lifetime_errors; return; }
    VkSwapchainKHR *link = &d->swapchains;
    while (*link && *link != swapchain) link = &(*link)->next;
    if (*link) *link = swapchain->next;
    --swapchain->surface->swapchains;
    release_images(swapchain);
    VkAllocationCallbacks saved = swapchain->allocator;
    VkBool32 custom = swapchain->custom_allocator;
    ps5vk_object_free(swapchain, &saved, custom);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice d,
    VkSwapchainKHR swapchain, uint32_t *count, VkImage *images)
{
    if (!count || !live_swapchain(d, swapchain)) return INVALID;
    if (!images) { *count = 2; return VK_SUCCESS; }
    uint32_t written = *count < 2 ? *count : 2;
    for (uint32_t n = 0; n < written; ++n) images[n] = swapchain->images[n];
    *count = written;
    return written == 2 ? VK_SUCCESS : VK_INCOMPLETE;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice d,
    VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
    VkFence fence, uint32_t *index)
{
    if (!index || !live_swapchain(d, swapchain) || (!semaphore && !fence))
        return INVALID;
    if (d->lost) return VK_ERROR_DEVICE_LOST;
    if (semaphore && (semaphore->device != d || semaphore->type != VK_SEMAPHORE_TYPE_BINARY ||
                      semaphore->pending || semaphore->signaled)) return INVALID;
    if (fence && (fence->device != d || fence->pending_serial || fence->signaled))
        return INVALID;
    const uint64_t start = timeout && d->progress.clock_ns ?
        d->progress.clock_ns(d->progress.context) : 0;
    for (;;) {
        ps5vk_device_lock(d);
        for (unsigned n = 0; n < 2; ++n) {
            if (swapchain->acquired[n] || swapchain->images[n]->display_busy) continue;
            swapchain->acquired[n] = VK_TRUE;
            if (semaphore) semaphore->signaled = VK_TRUE;
            if (fence) fence->signaled = VK_TRUE;
            *index = n;
            ps5vk_device_unlock(d);
            return VK_SUCCESS;
        }
        ps5vk_device_unlock(d);
        if (!timeout) return VK_NOT_READY;
        if (!d->progress.clock_ns || !d->progress.pause) return VK_TIMEOUT;
        const uint64_t elapsed = d->progress.clock_ns(d->progress.context) - start;
        if (timeout != UINT64_MAX && elapsed >= timeout) return VK_TIMEOUT;
        const uint64_t remaining = timeout == UINT64_MAX ? UINT64_MAX : timeout - elapsed;
        d->progress.pause(d->progress.context, remaining);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue,
    const VkPresentInfoKHR *info)
{
    if (!queue || !queue->device || !info ||
        info->sType != VK_STRUCTURE_TYPE_PRESENT_INFO_KHR || info->pNext ||
        info->swapchainCount != 1 || !info->pSwapchains || !info->pImageIndices ||
        (info->waitSemaphoreCount && !info->pWaitSemaphores)) return INVALID;
    VkDevice d = queue->device;
    VkSwapchainKHR chain = info->pSwapchains[0];
    uint32_t slot = info->pImageIndices[0];
    if (!live_swapchain(d, chain) || slot >= 2 || !chain->acquired[slot])
        return INVALID;
    VkResult result = drain_before_present(d);
    if (result != VK_SUCCESS) {
        if (info->pResults) info->pResults[0] = result;
        return result;
    }
    if (chain->images[slot]->layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        if (info->pResults) info->pResults[0] = INVALID;
        return INVALID;
    }
    ps5vk_device_lock(d);
    for (uint32_t n = 0; n < info->waitSemaphoreCount; ++n) {
        VkSemaphore semaphore = info->pWaitSemaphores[n];
        if (!semaphore || semaphore->device != d ||
            semaphore->type != VK_SEMAPHORE_TYPE_BINARY || !semaphore->signaled) {
            ps5vk_device_unlock(d);
            if (info->pResults) info->pResults[0] = INVALID;
            return INVALID;
        }
    }
    for (uint32_t n = 0; n < info->waitSemaphoreCount; ++n)
        info->pWaitSemaphores[n]->signaled = VK_FALSE;
    ps5vk_device_unlock(d);
    result = ps5vk_wsi_present_frame(chain->present, slot, ++chain->token);
    if (result == VK_SUCCESS) chain->acquired[slot] = VK_FALSE;
    if (info->pResults) info->pResults[0] = result;
    return result;
}
