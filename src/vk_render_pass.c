#include "vk_render_pass.h"
#include <stdint.h>
#include <string.h>

static int layout(VkImageLayout value, int depth, int initial)
{
    return (initial && value == VK_IMAGE_LAYOUT_UNDEFINED) ||
        value == VK_IMAGE_LAYOUT_GENERAL || value == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        value == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        value == (depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ||
        (!depth && value == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* One subpass description against this profile: exactly one colour reference,
 * an optional D32 depth reference that may not alias it, and nothing this
 * driver cannot execute. Input and resolve attachments are refused because
 * they are unimplemented, not tolerated because the structure has fields for
 * them. Preserve entries are validated here and owned by the object. */
static VkResult subpass_valid(const VkSubpassDescription *s, uint32_t attachments,
    VkAttachmentReference *color, VkAttachmentReference *depth)
{
    if (s->flags || s->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS ||
        s->colorAttachmentCount != 1 || !s->pColorAttachments ||
        /* Vulkan IGNORES pInputAttachments when the count is zero, so the
         * pointer says nothing; only a nonzero count asks for input
         * attachments, which are unimplemented. pResolveAttachments is
         * different: a non-null pointer IS the request. */
        s->inputAttachmentCount || s->pResolveAttachments)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Every attachment of this profile is used by every subpass, so nothing
     * can be preserved-but-unused and a non-empty list has no legal form. */
    if (s->preserveAttachmentCount) return VK_ERROR_FEATURE_NOT_PRESENT;
    *color = s->pColorAttachments[0];
    depth->attachment = VK_ATTACHMENT_UNUSED;
    depth->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (s->pDepthStencilAttachment) *depth = *s->pDepthStencilAttachment;
    if (color->attachment >= attachments ||
        (color->layout != VK_IMAGE_LAYOUT_GENERAL &&
         color->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ||
        (depth->attachment != VK_ATTACHMENT_UNUSED &&
         (depth->attachment >= attachments || depth->attachment == color->attachment ||
          (depth->layout != VK_IMAGE_LAYOUT_GENERAL &&
           depth->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL))))
        return VK_ERROR_UNKNOWN;
    return VK_SUCCESS;
}

/* Byte size of the object and everything it owns, with every multiplication
 * and addition checked. The four arrays are suballocated from one block, so
 * there is no partially built object to roll back: either the single
 * allocation succeeds and the object is complete, or nothing was allocated. */
static VkResult owned_bytes(const VkRenderPassCreateInfo *info, size_t *total)
{
    size_t bytes = sizeof(struct VkRenderPass_T);
    /* Same order as the suballocation below, so the total cannot drift from
     * the layout it is supposed to cover. */
    const size_t parts[3] = {
        (size_t)info->subpassCount * sizeof(struct ps5vk_subpass),
        (size_t)info->attachmentCount * sizeof(VkAttachmentDescription),
        (size_t)info->dependencyCount * sizeof(VkSubpassDependency),
    };
    for (unsigned i = 0; i < 3; ++i) {
        /* The counts are already bounded by the profile limits checked above,
         * so these cannot overflow; the checks are kept so a later limit
         * change cannot turn into a silent wrap. */
        if (parts[i] > SIZE_MAX - bytes) return VK_ERROR_OUT_OF_HOST_MEMORY;
        bytes += parts[i];
    }
    *total = bytes;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice d,
    const VkRenderPassCreateInfo *info, const VkAllocationCallbacks *allocator, VkRenderPass *out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO)
        return VK_ERROR_UNKNOWN;
    if (!d->graphics_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->pNext || info->flags ||
        !info->subpassCount || info->subpassCount > PS5VK_MAX_SUBPASSES || !info->pSubpasses ||
        !info->attachmentCount || info->attachmentCount > PS5VK_MAX_ATTACHMENTS ||
        !info->pAttachments ||
        info->dependencyCount > PS5VK_MAX_DEPENDENCIES ||
        (info->dependencyCount && !info->pDependencies))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkAttachmentReference colors[PS5VK_MAX_SUBPASSES], depths[PS5VK_MAX_SUBPASSES];
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        VkResult rc = subpass_valid(&info->pSubpasses[i], info->attachmentCount,
                                    &colors[i], &depths[i]);
        if (rc != VK_SUCCESS) return rc;
    }
    /* EVERY SUBPASS MUST NAME THE SAME ATTACHMENTS. A framebuffer in this
     * driver carries one colour role and one depth role, derived from the
     * pass, so a pass whose subpasses disagreed about which attachment is the
     * colour one could be created and then served by no framebuffer at all.
     * The profile is constrained here, where the caller sees it, instead of
     * being advertised and then failing later. Layouts may still differ per
     * subpass; only the attachment each role names is fixed. */
    for (uint32_t i = 1; i < info->subpassCount; ++i)
        if (colors[i].attachment != colors[0].attachment ||
            depths[i].attachment != depths[0].attachment)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Every attachment must be reachable through a subpass reference: an
     * attachment this profile never uses has no role to play. */
    for (uint32_t i = 0; i < info->attachmentCount; ++i)
        if (colors[0].attachment != i && depths[0].attachment != i)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        const VkAttachmentDescription *a = &info->pAttachments[i];
        /* The roles are shared, so subpass 0 decides which attachment is the
         * depth one for the whole pass. */
        int is_depth = depths[0].attachment == i;
        if (a->flags || a->samples != VK_SAMPLE_COUNT_1_BIT ||
            (is_depth ? a->format != VK_FORMAT_D32_SFLOAT :
             (a->format != VK_FORMAT_B8G8R8A8_UNORM && a->format != VK_FORMAT_R8G8B8A8_UNORM)))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (a->loadOp < VK_ATTACHMENT_LOAD_OP_LOAD || a->loadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
            a->storeOp < VK_ATTACHMENT_STORE_OP_STORE || a->storeOp > VK_ATTACHMENT_STORE_OP_DONT_CARE ||
            a->stencilLoadOp < VK_ATTACHMENT_LOAD_OP_LOAD || a->stencilLoadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
            a->stencilStoreOp < VK_ATTACHMENT_STORE_OP_STORE || a->stencilStoreOp > VK_ATTACHMENT_STORE_OP_DONT_CARE ||
            !layout(a->initialLayout, is_depth, 1) || !layout(a->finalLayout, is_depth, 0) ||
            (a->initialLayout == VK_IMAGE_LAYOUT_UNDEFINED && a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD))
            return VK_ERROR_UNKNOWN;
    }
    for (uint32_t i = 0; i < info->dependencyCount; ++i) {
        const VkSubpassDependency *dep = &info->pDependencies[i];
        const VkBool32 src_external = dep->srcSubpass == VK_SUBPASS_EXTERNAL;
        const VkBool32 dst_external = dep->dstSubpass == VK_SUBPASS_EXTERNAL;
        /* Endpoints must exist, at least one end must be a real subpass, and a
         * subpass-to-subpass edge must point FORWARD: a backward edge or a
         * self-dependency would describe execution this driver does not
         * perform, so it is refused rather than stored and ignored. */
        if ((!src_external && dep->srcSubpass >= info->subpassCount) ||
            (!dst_external && dep->dstSubpass >= info->subpassCount) ||
            (src_external && dst_external) ||
            (!src_external && !dst_external && dep->srcSubpass >= dep->dstSubpass) ||
            (dep->dependencyFlags & ~VK_DEPENDENCY_BY_REGION_BIT) ||
            !dep->srcStageMask || !dep->dstStageMask) return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    size_t bytes = 0;
    if (owned_bytes(info, &bytes) != VK_SUCCESS)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkAllocationCallbacks saved; VkBool32 custom;
    VkRenderPass pass = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL, allocator,
        bytes, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!pass) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(pass, 0, bytes);
    pass->device = d; pass->allocator = saved; pass->custom_allocator = custom;
    pass->attachment_count = info->attachmentCount;
    pass->subpass_count = info->subpassCount;
    pass->dependency_count = info->dependencyCount;
    /* Suballocate the owned arrays from the single block in DESCENDING
     * alignment order. ps5vk_subpass contains a pointer and so needs the
     * strictest alignment; VkAttachmentDescription is a run of 32-bit enums
     * whose size is not a multiple of that, so placing it first would leave
     * every later array misaligned. The object's own size is a multiple of the
     * pointer alignment, so the first array starts aligned. */
    unsigned char *cursor = (unsigned char *)pass + sizeof(*pass);
    struct ps5vk_subpass *subpasses = (struct ps5vk_subpass *)(void *)cursor;
    cursor += (size_t)info->subpassCount * sizeof(*subpasses);
    VkAttachmentDescription *attachments = (VkAttachmentDescription *)(void *)cursor;
    cursor += (size_t)info->attachmentCount * sizeof(*attachments);
    VkSubpassDependency *dependencies = (VkSubpassDependency *)(void *)cursor;
    memcpy(attachments, info->pAttachments,
           (size_t)info->attachmentCount * sizeof(*attachments));
    if (info->dependencyCount)
        memcpy(dependencies, info->pDependencies,
               (size_t)info->dependencyCount * sizeof(*dependencies));
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        subpasses[i].color = colors[i];
        subpasses[i].depth = depths[i];
    }
    pass->attachments = attachments;
    pass->subpasses = subpasses;
    pass->dependencies = dependencies;
    ++d->graphics_objects; *out = pass; return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice d, VkRenderPass pass,
                                               const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!pass) return;
    if (!d || pass->device != d || pass->pending ||
        (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_RENDER_PASS, pass))) {
        if (d) ++d->lifetime_errors;
        return;
    }
    --d->graphics_objects;
    ps5vk_object_free(pass, &pass->allocator, pass->custom_allocator);
}

VKAPI_ATTR void VKAPI_CALL vkGetRenderAreaGranularity(VkDevice d, VkRenderPass pass,
                                                     VkExtent2D *pGranularity)
{
    if (!pGranularity) return;
    if (!d || !pass || pass->device != d) {
        *pGranularity = (VkExtent2D){0, 0};
        return;
    }
    *pGranularity = (VkExtent2D){1, 1};
}
