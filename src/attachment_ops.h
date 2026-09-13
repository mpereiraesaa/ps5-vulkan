#ifndef PS5VK_ATTACHMENT_OPS_H
#define PS5VK_ATTACHMENT_OPS_H

#include <vulkan/vulkan.h>

struct ps5vk_attachment_plan {
    VkBool32 clear;
    VkBool32 load;
    VkBool32 store;
};

/* Native single-sample attachment contract.  This is deliberately narrower
 * than render-pass object creation: it describes semantics the AGC queue can
 * execute without silently weakening LOAD, CLEAR, or final-layout ownership. */
static inline VkResult ps5vk_attachment_plan(const VkAttachmentDescription *a,
    VkFormat format, VkImageLayout reference, VkBool32 depth,
    struct ps5vk_attachment_plan *out)
{
    const VkImageLayout attachment = depth ?
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (!a || !out || a->format != format || a->samples != VK_SAMPLE_COUNT_1_BIT ||
        (depth ? format != VK_FORMAT_D32_SFLOAT :
         (format != VK_FORMAT_B8G8R8A8_UNORM &&
          format != VK_FORMAT_R8G8B8A8_UNORM)) ||
        (reference != attachment && reference != VK_IMAGE_LAYOUT_GENERAL) ||
        (a->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
         a->initialLayout != attachment && a->initialLayout != VK_IMAGE_LAYOUT_GENERAL) ||
        (a->finalLayout != attachment && a->finalLayout != VK_IMAGE_LAYOUT_GENERAL) ||
        a->loadOp < VK_ATTACHMENT_LOAD_OP_LOAD || a->loadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
        a->storeOp < VK_ATTACHMENT_STORE_OP_STORE || a->storeOp > VK_ATTACHMENT_STORE_OP_DONT_CARE ||
        (a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
         a->initialLayout == VK_IMAGE_LAYOUT_UNDEFINED))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    *out = (struct ps5vk_attachment_plan){
        .clear = a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR,
        .load = a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD,
        .store = a->storeOp == VK_ATTACHMENT_STORE_OP_STORE,
    };
    return VK_SUCCESS;
}

#endif
