#ifndef PS5VK_ATTACHMENT_OPS_H
#define PS5VK_ATTACHMENT_OPS_H

#include <vulkan/vulkan.h>
#include "color_attachment_contract.h"
#include "sample_rate_contract.h"

struct ps5vk_attachment_plan {
    VkBool32 clear;
    VkBool32 load;
    VkBool32 store;
};

/* Native attachment contract.  This is deliberately narrower than render-pass
 * object creation: it describes semantics the AGC queue can execute without
 * silently weakening LOAD, CLEAR, or final-layout ownership.
 *
 * The sample counts are the ones this contract carries (DXVK262-T06): the
 * colour role takes the 1x/2x/4x envelope in src/sample_rate_contract.h and the
 * depth role stays 1x, exactly as the render pass and pipeline frontends
 * accept them. The render pass has already refused a count the device's
 * platform does not serve, so this bound is the compile-time envelope: the
 * native target builder is what turns the colour count into CB_COLOR0_ATTRIB's
 * NUM_SAMPLES/NUM_FRAGMENTS, and a count no target can describe is refused
 * here rather than drawn at the wrong rate. */
static inline int ps5vk_attachment_samples_supported(VkSampleCountFlagBits samples,
                                                     VkBool32 depth)
{
    return depth ? samples == VK_SAMPLE_COUNT_1_BIT :
        (ps5vk_sample_count_mask() & samples) != 0;
}

static inline VkResult ps5vk_attachment_plan(const VkAttachmentDescription *a,
    VkFormat format, VkImageLayout reference, VkBool32 depth,
    struct ps5vk_attachment_plan *out)
{
    const VkImageLayout attachment = depth ?
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (!a || !out || a->format != format ||
        !ps5vk_attachment_samples_supported(a->samples, depth) ||
        (depth ? format != VK_FORMAT_D32_SFLOAT :
         !ps5vk_color_target_format_supported(format)) ||
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
