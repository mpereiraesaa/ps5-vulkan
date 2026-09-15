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

/* One subpass: the roles this profile executes, plus the preserve list it is
 * required to carry faithfully. A preserve entry names an attachment the
 * subpass neither reads nor writes but whose contents must survive it; the
 * list is validated and owned, and it has no execution meaning while only one
 * subpass can execute. */
struct ps5vk_subpass {
    VkAttachmentReference color, depth;
    uint32_t preserve_count;
    uint32_t *preserve;
};

struct VkRenderPass_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    unsigned pending;
    uint32_t attachment_count, subpass_count, dependency_count;
    /* All four arrays live in the object's single allocation, so a failed
     * creation frees exactly one block and no partially built object can
     * escape. Sizes are computed with checked arithmetic before allocating. */
    VkAttachmentDescription *attachments;
    struct ps5vk_subpass *subpasses;
    VkSubpassDependency *dependencies;
};

/* The references of one subpass. Callers that only handle the single-subpass
 * profile pass 0 and say so, rather than reaching for fields that no longer
 * describe the whole pass. */
static inline const struct ps5vk_subpass *ps5vk_render_pass_subpass(
    VkRenderPass pass, uint32_t index)
{
    return &pass->subpasses[index];
}
#endif
