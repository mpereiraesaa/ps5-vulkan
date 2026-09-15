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
    VkImageViewType view_type;
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
/* The colour-attachment readback shape that also declares a transfer
 * destination; the clear and buffer-upload destination paths accept it. */
VkBool32 ps5vk_colour_transfer_image(VkImage);
/* The descriptor shape of the one linear image this profile creates:
 * RGBA8, 2D, one mip, one layer, one sample, LINEAR tiling, usage TRANSFER_DST
 * alone. Used by vkCreateImage to accept it and to refuse every other linear
 * combination before an object exists. */
int ps5vk_linear_staging_descriptor(const VkImageCreateInfo *);
/* The pinned upstream draw module's host-readback staging image itself: the
 * linear-tiling image vkGetImageSubresourceLayout may describe and the only
 * destination a colour-attachment readback copy may name. */
VkBool32 ps5vk_linear_staging_image(VkImage);
#endif
