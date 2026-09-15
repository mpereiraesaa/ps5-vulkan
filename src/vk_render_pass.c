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
        s->inputAttachmentCount || s->pInputAttachments || s->pResolveAttachments)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (s->preserveAttachmentCount && !s->pPreserveAttachments)
        return VK_ERROR_UNKNOWN;
    if (s->preserveAttachmentCount > attachments) return VK_ERROR_FEATURE_NOT_PRESENT;
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
    /* A preserved attachment is one the subpass does NOT use, so naming an
     * attachment it also references is a contradiction, and so is naming the
     * same one twice. */
    for (uint32_t j = 0; j < s->preserveAttachmentCount; ++j) {
        uint32_t preserved = s->pPreserveAttachments[j];
        if (preserved >= attachments || preserved == color->attachment ||
            preserved == depth->attachment) return VK_ERROR_UNKNOWN;
        for (uint32_t k = 0; k < j; ++k)
            if (s->pPreserveAttachments[k] == preserved) return VK_ERROR_UNKNOWN;
    }
    return VK_SUCCESS;
}

/* Byte size of the object and everything it owns, with every multiplication
 * and addition checked. The four arrays are suballocated from one block, so
 * there is no partially built object to roll back: either the single
 * allocation succeeds and the object is complete, or nothing was allocated. */
static VkResult owned_bytes(const VkRenderPassCreateInfo *info, size_t *total,
    size_t *preserve_total)
{
    size_t preserved = 0;
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        uint32_t count = info->pSubpasses[i].preserveAttachmentCount;
        if (count > SIZE_MAX - preserved) return VK_ERROR_OUT_OF_HOST_MEMORY;
        preserved += count;
    }
    size_t bytes = sizeof(struct VkRenderPass_T);
    /* Same order as the suballocation below, so the total cannot drift from
     * the layout it is supposed to cover. */
    const size_t parts[4] = {
        (size_t)info->subpassCount * sizeof(struct ps5vk_subpass),
        (size_t)info->attachmentCount * sizeof(VkAttachmentDescription),
        (size_t)info->dependencyCount * sizeof(VkSubpassDependency),
        preserved * sizeof(uint32_t),
    };
    for (unsigned i = 0; i < 4; ++i) {
        /* The counts are already bounded by the profile limits checked above,
         * so these cannot overflow; the checks are kept so a later limit
         * change cannot turn into a silent wrap. */
        if (parts[i] > SIZE_MAX - bytes) return VK_ERROR_OUT_OF_HOST_MEMORY;
        bytes += parts[i];
    }
    *total = bytes; *preserve_total = preserved;
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
    /* Every attachment must be reachable through some subpass reference: this
     * profile has no attachment that exists only to be preserved. */
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        VkBool32 referenced = VK_FALSE;
        for (uint32_t j = 0; j < info->subpassCount; ++j)
            referenced |= colors[j].attachment == i || depths[j].attachment == i;
        if (!referenced) return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        const VkAttachmentDescription *a = &info->pAttachments[i];
        /* An attachment is the depth one if ANY subpass uses it as depth, and
         * a single attachment may not be colour in one subpass and depth in
         * another: the formats are disjoint and the roles are not swappable. */
        int is_depth = 0, is_color = 0;
        for (uint32_t j = 0; j < info->subpassCount; ++j) {
            is_depth |= depths[j].attachment == i;
            is_color |= colors[j].attachment == i;
        }
        if (is_depth && is_color) return VK_ERROR_UNKNOWN;
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
    size_t bytes = 0, preserved = 0;
    if (owned_bytes(info, &bytes, &preserved) != VK_SUCCESS)
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
    cursor += (size_t)info->dependencyCount * sizeof(*dependencies);
    uint32_t *preserve = (uint32_t *)(void *)cursor;
    memcpy(attachments, info->pAttachments,
           (size_t)info->attachmentCount * sizeof(*attachments));
    if (info->dependencyCount)
        memcpy(dependencies, info->pDependencies,
               (size_t)info->dependencyCount * sizeof(*dependencies));
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        const VkSubpassDescription *described = &info->pSubpasses[i];
        subpasses[i].color = colors[i];
        subpasses[i].depth = depths[i];
        subpasses[i].preserve_count = described->preserveAttachmentCount;
        subpasses[i].preserve = preserve;
        if (described->preserveAttachmentCount) {
            memcpy(preserve, described->pPreserveAttachments,
                   (size_t)described->preserveAttachmentCount * sizeof(*preserve));
            preserve += described->preserveAttachmentCount;
        }
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
