#ifndef PS5VK_DEPTH_STENCIL_LAYOUT_H
#define PS5VK_DEPTH_STENCIL_LAYOUT_H
/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Per-aspect image layouts (VK_KHR_separate_depth_stencil_layouts and the
 * mixed depth/stencil layouts of VK_KHR_maintenance2).
 *
 * A combined depth/stencil image carries one layout per aspect. Every layout
 * a barrier, a render pass or a copy may name is therefore read through the
 * aspect it applies to: DEPTH_STENCIL_ATTACHMENT_OPTIMAL means "depth
 * attachment" to the depth aspect and "stencil attachment" to the stencil
 * aspect, DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL means "depth read-only"
 * and "stencil attachment", and so on. That projection is the Vulkan image
 * layout matching rule for depth/stencil aspects: two layouts match for an
 * aspect exactly when they project to the same per-aspect layout, so
 * DEPTH_ATTACHMENT_OPTIMAL and DEPTH_STENCIL_ATTACHMENT_OPTIMAL name the same
 * depth state while leaving the stencil aspect alone.
 *
 * A layout that only describes the other aspect (DEPTH_* for the stencil
 * aspect, STENCIL_* for the depth aspect) and every depth/stencil layout for a
 * colour aspect has no projection, which is how an aspect/layout mismatch is
 * refused. The functions are pure and host-testable; the layout transaction
 * (src/image_layout_state.c) and the command recorder share them so the two
 * cannot disagree about what a layout means for an aspect.
 */
#include <vulkan/vulkan_core.h>

#define PS5VK_DEPTH_STENCIL_ASPECTS \
    ((VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))

/* The aspects a format carries. Only the depth/stencil formats the Vulkan
 * registry defines are named; everything else is a colour format here. */
static inline VkImageAspectFlags ps5vk_format_aspects(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return PS5VK_DEPTH_STENCIL_ASPECTS;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

static inline int ps5vk_format_is_combined_depth_stencil(VkFormat format)
{
    return ps5vk_format_aspects(format) == PS5VK_DEPTH_STENCIL_ASPECTS;
}

/* True for the depth/stencil layouts that name one aspect only
 * (VK_KHR_separate_depth_stencil_layouts). */
static inline int ps5vk_layout_is_separate_aspect(VkImageLayout layout)
{
    return layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
}

/* True for the two layouts that give the aspects different access
 * (VK_KHR_maintenance2). */
static inline int ps5vk_layout_is_mixed_depth_stencil(VkImageLayout layout)
{
    return layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL ||
        layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
}

/* The layout one aspect is in when `layout` is applied to it, or 0 when the
 * layout has no meaning for that aspect. `aspect` is exactly one aspect bit.
 * UNDEFINED and PREINITIALIZED project to themselves, like every layout that
 * does not distinguish aspects. */
static inline int ps5vk_layout_for_aspect(VkImageLayout layout,
    VkImageAspectFlags aspect, VkImageLayout *out)
{
    const int depth = aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    const int stencil = aspect == VK_IMAGE_ASPECT_STENCIL_BIT;
    const int colour = aspect == VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout result;
    if (!out || (!depth && !stencil && !colour)) return 0;
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
    case VK_IMAGE_LAYOUT_PREINITIALIZED:
    case VK_IMAGE_LAYOUT_GENERAL:
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        result = layout;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        if (!colour) return 0;
        result = layout;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        if (colour) return 0;
        result = depth ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL :
            VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        if (colour) return 0;
        result = depth ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL :
            VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
        if (colour) return 0;
        result = depth ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL :
            VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
        if (colour) return 0;
        result = depth ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL :
            VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
        if (!depth) return 0;
        result = layout;
        break;
    case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
        if (!stencil) return 0;
        result = layout;
        break;
    default:
        return 0;
    }
    *out = result;
    return 1;
}

/* The image layout matching rule for one aspect. Both layouts must be
 * meaningful for the aspect. */
static inline int ps5vk_layouts_match_for_aspect(VkImageLayout a, VkImageLayout b,
    VkImageAspectFlags aspect)
{
    VkImageLayout pa, pb;
    return ps5vk_layout_for_aspect(a, aspect, &pa) &&
        ps5vk_layout_for_aspect(b, aspect, &pb) && pa == pb;
}

/* The per-aspect pair of a depth/stencil attachment or reference. A render
 * pass created without the separate stencil structures uses one combined
 * layout for both aspects; VkAttachmentDescriptionStencilLayout and
 * VkAttachmentReferenceStencilLayout override the stencil half. */
struct ps5vk_depth_stencil_layouts {
    VkImageLayout depth, stencil;
};

/* Split one layout into the layouts an image with `aspects` takes in each
 * aspect. The raw layout is kept (not its projection), so a single-aspect
 * image keeps reporting exactly the layout the application named. Returns 0
 * when the layout has no meaning for one of the aspects. */
static inline int ps5vk_depth_stencil_layouts_from(VkImageLayout layout,
    VkImageAspectFlags aspects, struct ps5vk_depth_stencil_layouts *out)
{
    VkImageLayout ignored;
    if (!out || !aspects || (aspects & ~PS5VK_DEPTH_STENCIL_ASPECTS)) return 0;
    if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) &&
        !ps5vk_layout_for_aspect(layout, VK_IMAGE_ASPECT_DEPTH_BIT, &ignored)) return 0;
    if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) &&
        !ps5vk_layout_for_aspect(layout, VK_IMAGE_ASPECT_STENCIL_BIT, &ignored)) return 0;
    out->depth = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? layout : VK_IMAGE_LAYOUT_UNDEFINED;
    out->stencil = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? layout : VK_IMAGE_LAYOUT_UNDEFINED;
    return 1;
}

/* Whether a layout gives the aspect write access as an attachment. Used to
 * decide whether a render pass may write an aspect at all: a read-only aspect
 * must keep its contents whatever the load/store ops say. */
static inline int ps5vk_layout_aspect_attachment_writable(VkImageLayout layout,
    VkImageAspectFlags aspect)
{
    VkImageLayout projected;
    if (!ps5vk_layout_for_aspect(layout, aspect, &projected)) return 0;
    return projected == VK_IMAGE_LAYOUT_GENERAL ||
        projected == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ||
        projected == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
}
#endif
