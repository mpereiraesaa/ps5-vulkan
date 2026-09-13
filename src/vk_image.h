#ifndef PS5VK_IMAGE_H
#define PS5VK_IMAGE_H
#include "vk_internal.h"
struct VkImage_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator, ever_bound;
    VkImageCreateInfo info;
    VkMemoryRequirements requirements;
    VkDeviceMemory memory;
    VkDeviceSize offset;
    unsigned pending, views;
    /* Committed only after confirmed completion; calloc initializes UNDEFINED. */
    VkImageLayout layout;
    /* Native display ownership is independent of queued rendering references. */
    VkBool32 display_busy;
    struct VkImage_T *next;
};
struct VkImageView_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    VkImage image;
    VkImageSubresourceRange range;
    VkFormat format;
    unsigned pending, framebuffers;
};
VkResult ps5vk_image_span(VkDevice, VkImage, void **address, VkDeviceSize *bytes);
/* The host-visible padded-linear transfer role: RGBA8, one mip/layer/sample,
 * optimal tiling, usage drawn from the two transfer bits only. No GPU stage can
 * sample, render into or read such an image, so its transfers and layout
 * transitions are frontend work over the same padded layout the upload path
 * uses. */
VkBool32 ps5vk_pure_transfer_image(VkImage);
#endif
