#ifndef PS5VK_ATTACHMENT_OPS_H
#define PS5VK_ATTACHMENT_OPS_H

#include <vulkan/vulkan.h>
#include "color_attachment_contract.h"
#include "sample_rate_contract.h"
#include "depth_stencil_layout.h"

struct ps5vk_attachment_plan {
    VkBool32 clear;
    VkBool32 load;
    VkBool32 store;
};

/* Native attachment contract.  This is deliberately narrower than render-pass
 * object creation: it describes semantics the AGC queue can execute without
 * silently weakening LOAD, CLEAR, or final-layout ownership.
 *
 * `readback` says the colour attachment is also read back after the pass. The
 * pinned render-pass module reads every attachment of its pass back, and its
 * own final layout for them is the transfer source: the pass is the transition
 * that hands the rendered surface to its readback, which is exactly what the
 * executor programmes for that role (one initial-to-final transition per
 * target). Every other attachment keeps ending in its attachment layout or
 * GENERAL.
 *
 * `served` is why the sample count is bounded by the mask the DEVICE's platform
 * serves rather than by a constant here (DXVK262-T06): the front end already
 * refused a count the platform does not report, so this bound only keeps a
 * hand-built plan from naming a count no build serves. The colour target
 * carries the count and is sized for it, and the queue's clear is a
 * whole-surface fill over the image's own span, which writes every sample of
 * every covered texel whatever order the hardware stores them in. A depth
 * attachment stays single-sample: no multisampled depth target exists on this
 * path. */
static inline VkResult ps5vk_attachment_plan(const VkAttachmentDescription *a,
    VkFormat format, VkImageLayout reference, VkBool32 depth, VkBool32 readback,
    VkSampleCountFlags served, struct ps5vk_attachment_plan *out)
{
    const VkImageLayout attachment = depth ?
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (!a || !out || a->format != format ||
        (depth ? a->samples != VK_SAMPLE_COUNT_1_BIT : !(served & a->samples)) ||
        (depth ? (format != VK_FORMAT_D32_SFLOAT && format != VK_FORMAT_D16_UNORM) :
         !ps5vk_color_target_format_supported(format)) ||
        (reference != attachment && reference != VK_IMAGE_LAYOUT_GENERAL) ||
        (a->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
         a->initialLayout != attachment && a->initialLayout != VK_IMAGE_LAYOUT_GENERAL) ||
        (a->finalLayout != attachment && a->finalLayout != VK_IMAGE_LAYOUT_GENERAL &&
         !(readback && !depth &&
           a->finalLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) ||
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

/* The combined depth/stencil attachment (D32_SFLOAT_S8_UINT). Each aspect has
 * its own load/store operation (loadOp/storeOp for depth,
 * stencilLoadOp/stencilStoreOp for stencil) and, with
 * VK_KHR_separate_depth_stencil_layouts, its own initial, subpass and final
 * layout. The per-aspect layouts are read through the aspect
 * (ps5vk_layout_for_aspect), so a combined, mixed or separate layout is
 * accepted exactly when it means an attachment, read-only, GENERAL or
 * readback state for that aspect:
 *
 *   reference  attachment, read-only or GENERAL;
 *   initial    UNDEFINED or any of those, or TRANSFER_SRC (a surface handed
 *              back from its readback);
 *   final      attachment, read-only, GENERAL or TRANSFER_SRC.
 *
 * LOAD of an aspect whose initial layout is UNDEFINED is refused: there are
 * no contents to load. A read-only reference makes the aspect's store a no-op
 * for the DB, which never writes an aspect the draw state leaves disabled. */
struct ps5vk_depth_stencil_plan {
    VkBool32 depth_clear, depth_load, depth_store;
    VkBool32 stencil_clear, stencil_load, stencil_store;
};

static inline int ps5vk_depth_stencil_aspect_layout(VkImageLayout layout,
    VkImageAspectFlags aspect, int initial, int final)
{
    VkImageLayout p;
    if (initial && layout == VK_IMAGE_LAYOUT_UNDEFINED) return 1;
    if (!ps5vk_layout_for_aspect(layout, aspect, &p)) return 0;
    if (p == VK_IMAGE_LAYOUT_GENERAL || p == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ||
        p == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
        p == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL ||
        p == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL) return 1;
    return (initial || final) && p == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

static inline VkResult ps5vk_depth_stencil_attachment_plan(const VkAttachmentDescription *a,
    VkFormat format, const struct ps5vk_depth_stencil_layouts *initial,
    const struct ps5vk_depth_stencil_layouts *reference,
    const struct ps5vk_depth_stencil_layouts *final,
    struct ps5vk_depth_stencil_plan *out)
{
    if (!a || !out || !initial || !reference || !final || a->format != format ||
        format != VK_FORMAT_D32_SFLOAT_S8_UINT || a->samples != VK_SAMPLE_COUNT_1_BIT ||
        a->loadOp < VK_ATTACHMENT_LOAD_OP_LOAD || a->loadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
        a->storeOp < VK_ATTACHMENT_STORE_OP_STORE || a->storeOp > VK_ATTACHMENT_STORE_OP_DONT_CARE ||
        a->stencilLoadOp < VK_ATTACHMENT_LOAD_OP_LOAD ||
        a->stencilLoadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
        a->stencilStoreOp < VK_ATTACHMENT_STORE_OP_STORE ||
        a->stencilStoreOp > VK_ATTACHMENT_STORE_OP_DONT_CARE ||
        !ps5vk_depth_stencil_aspect_layout(initial->depth, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 0) ||
        !ps5vk_depth_stencil_aspect_layout(initial->stencil, VK_IMAGE_ASPECT_STENCIL_BIT, 1, 0) ||
        !ps5vk_depth_stencil_aspect_layout(reference->depth, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0) ||
        !ps5vk_depth_stencil_aspect_layout(reference->stencil, VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0) ||
        !ps5vk_depth_stencil_aspect_layout(final->depth, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1) ||
        !ps5vk_depth_stencil_aspect_layout(final->stencil, VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1) ||
        (a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && initial->depth == VK_IMAGE_LAYOUT_UNDEFINED) ||
        (a->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
         initial->stencil == VK_IMAGE_LAYOUT_UNDEFINED))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    *out = (struct ps5vk_depth_stencil_plan){
        .depth_clear = a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR,
        .depth_load = a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD,
        .depth_store = a->storeOp == VK_ATTACHMENT_STORE_OP_STORE,
        .stencil_clear = a->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR,
        .stencil_load = a->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD,
        .stencil_store = a->stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE,
    };
    return VK_SUCCESS;
}

#endif
