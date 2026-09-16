#ifndef PS5VK_RENDER_PASS_H
#define PS5VK_RENDER_PASS_H
#include "vk_internal.h"

/* Bounded multiple-subpass profile. The shape is owned data - never retained
 * create-info pointers - and a graphics-capable backend must be enabled before
 * these objects are usable.
 *
 * The limits below are this profile's, not Vulkan's, and they are enforced at
 * creation so an unsupported shape is refused where the caller can see it
 * rather than accepted and failed later. */
enum { PS5VK_MAX_SUBPASSES = 2 };
enum { PS5VK_MAX_ATTACHMENTS = 2 };
enum { PS5VK_MAX_DEPENDENCIES = 4 };
enum { PS5VK_MAX_CORRELATION_MASKS = 4 };

/* Owned multiview configuration of a render pass. The structure is parsed and
 * validated at creation and never retains caller pointers.
 *
 * View masks and view offsets are the structural part multiview needs; the
 * correlation masks are performance hints - sets of views an implementation
 * MAY render concurrently - so they are validated (each view index in at most
 * one mask, VUID-VkRenderPassMultiviewCreateInfo-pCorrelationMasks-00841) and
 * then stored without ever affecting execution. Nothing in this driver
 * broadcasts a draw to a layer yet, and this profile does not advertise the
 * multiview feature, so VUID-VkRenderPassMultiviewCreateInfo-multiview-06555
 * keeps every accepted view mask at zero until the slice that enables it. */
struct ps5vk_render_pass_multiview {
    VkBool32 present;
    uint32_t subpass_count, dependency_count, correlation_mask_count;
    uint32_t view_masks[PS5VK_MAX_SUBPASSES];
    int32_t view_offsets[PS5VK_MAX_DEPENDENCIES];
    uint32_t correlation_masks[PS5VK_MAX_CORRELATION_MASKS];
};

/* The exact pinned obligations for VkRenderPassMultiviewCreateInfo, as a pure
 * function so both feature states are testable: with the feature disabled
 * every view mask must be zero (06555), and with it enabled the all-or-nothing
 * rule (02513), the most-significant-bit limit (06697), the view-offset rules
 * (01930, 02512) and the all-zero consequences (02514, 02515) apply. Counts
 * must match the pass (01928, 01929), correlation masks must be disjoint
 * (00841), and a chained or duplicated structure stays fail-closed. */
VkResult ps5vk_render_pass_multiview_validate(const VkRenderPassCreateInfo *info,
    const VkRenderPassMultiviewCreateInfo *multiview, VkBool32 multiview_enabled,
    uint32_t max_multiview_view_count, struct ps5vk_render_pass_multiview *out);

/* One subpass: the roles this profile executes.
 *
 * There is no preserve list here, and that is a statement about the profile
 * rather than an omission. Every subpass names the SAME colour and depth
 * attachments - the only shape the framebuffer model can serve - and every
 * attachment must be named by a subpass, so every attachment is used by every
 * subpass and no attachment can be preserved-but-unused. A non-empty
 * pPreserveAttachments therefore has no legal form here and is refused at
 * creation rather than stored where it could never mean anything. */
struct ps5vk_subpass {
    VkAttachmentReference color, depth;
};

struct VkRenderPass_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    unsigned pending;
    uint32_t attachment_count, subpass_count, dependency_count;
    /* All three arrays live in the object's single allocation, so a failed
     * creation frees exactly one block and no partially built object can
     * escape. Sizes are computed with checked arithmetic before allocating. */
    VkAttachmentDescription *attachments;
    struct ps5vk_subpass *subpasses;
    VkSubpassDependency *dependencies;
    struct ps5vk_render_pass_multiview multiview;
};

/* The references of one subpass. Callers that only handle the single-subpass
 * profile pass 0 and say so, rather than reaching for fields that no longer
 * describe the whole pass. Every subpass of a pass names the same attachments,
 * so a caller that needs only the ROLES may read subpass 0 for any of them. */
static inline const struct ps5vk_subpass *ps5vk_render_pass_subpass(
    VkRenderPass pass, uint32_t index)
{
    return &pass->subpasses[index];
}
#endif
