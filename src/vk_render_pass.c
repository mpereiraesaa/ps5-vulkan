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

/* The layouts a subpass may name for an input reference. Vulkan forbids
 * UNDEFINED, PREINITIALIZED and the transfer/present layouts here; of the ones
 * that are legal, this profile accepts exactly the layouts whose attachment
 * semantics it already models, so an input reference can never name a layout
 * the rest of the driver has no meaning for. */
static int input_layout(VkImageLayout value)
{
    return value == VK_IMAGE_LAYOUT_GENERAL ||
        value == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
        value == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
        value == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
        value == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}

/* One subpass description against this profile: exactly one colour reference,
 * an optional D32 depth reference that may not alias it, and the input
 * references this model can describe. Resolve attachments are still refused
 * because they are unimplemented, and a non-empty preserve list is outside the
 * bounded profile. */
static VkResult subpass_valid(const VkSubpassDescription *s, uint32_t attachments,
    VkAttachmentReference *color, VkAttachmentReference *depth, uint32_t *input_count)
{
    if (s->flags || s->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS ||
        s->colorAttachmentCount != 1 || !s->pColorAttachments ||
        /* Vulkan IGNORES pInputAttachments when the count is zero, so the
         * pointer says nothing there; a nonzero count is a real request that
         * has to name a valid array. pResolveAttachments is different: a
         * non-null pointer IS the request. */
        (s->inputAttachmentCount && !s->pInputAttachments) ||
        s->inputAttachmentCount > PS5VK_MAX_INPUT_ATTACHMENTS ||
        s->pResolveAttachments)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Every attachment of this profile is used by every subpass, so nothing
     * can be preserved-but-unused and a non-empty list has no legal form. */
    if (s->preserveAttachmentCount) return VK_ERROR_FEATURE_NOT_PRESENT;
    for (uint32_t i = 0; i < s->inputAttachmentCount; ++i) {
        const VkAttachmentReference *reference = &s->pInputAttachments[i];
        /* VK_ATTACHMENT_UNUSED is a legal entry and Vulkan ignores its layout,
         * so only a real reference is checked. A real one must name an
         * attachment of this pass and a layout an input attachment may use. */
        if (reference->attachment == VK_ATTACHMENT_UNUSED) continue;
        if (reference->attachment >= attachments || !input_layout(reference->layout))
            return VK_ERROR_UNKNOWN;
    }
    *input_count = s->inputAttachmentCount;
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
 * and addition checked. The arrays are suballocated from one block, so there is
 * no partially built object to roll back: either the single allocation succeeds
 * and the object is complete, or nothing was allocated. */
static VkResult owned_bytes(const VkRenderPassCreateInfo *info, size_t *total)
{
    size_t bytes = sizeof(struct VkRenderPass_T);
    /* Every subpass's input references are copied into the same block, so the
     * total has to include each one; a subpass count that overflows this sum
     * would otherwise leave the copy writing past the allocation. */
    size_t inputs = 0;
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        const size_t count = info->pSubpasses[i].inputAttachmentCount;
        if (count > (SIZE_MAX - inputs) / sizeof(VkAttachmentReference))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        inputs += count * sizeof(VkAttachmentReference);
    }
    /* Same order as the suballocation below, so the total cannot drift from
     * the layout it is supposed to cover. */
    const size_t parts[4] = {
        (size_t)info->subpassCount * sizeof(struct ps5vk_subpass),
        (size_t)info->attachmentCount * sizeof(VkAttachmentDescription),
        (size_t)info->dependencyCount * sizeof(VkSubpassDependency),
        inputs,
    };
    for (unsigned i = 0; i < 4; ++i) {
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
    /* Exactly one optional VkRenderPassMultiviewCreateInfo is understood; any
     * other structure, or a second copy of it, stays fail-closed. */
    const VkRenderPassMultiviewCreateInfo *multiview = NULL;
    for (const VkBaseInStructure *next = (const VkBaseInStructure *)info->pNext;
         next; next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO || multiview)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        multiview = (const VkRenderPassMultiviewCreateInfo *)next;
    }
    if (info->flags ||
        !info->subpassCount || info->subpassCount > PS5VK_MAX_SUBPASSES || !info->pSubpasses ||
        !info->attachmentCount || info->attachmentCount > PS5VK_MAX_ATTACHMENTS ||
        !info->pAttachments ||
        info->dependencyCount > PS5VK_MAX_DEPENDENCIES ||
        (info->dependencyCount && !info->pDependencies))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkAttachmentReference colors[PS5VK_MAX_SUBPASSES], depths[PS5VK_MAX_SUBPASSES];
    uint32_t input_counts[PS5VK_MAX_SUBPASSES];
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        VkResult rc = subpass_valid(&info->pSubpasses[i], info->attachmentCount,
                                    &colors[i], &depths[i], &input_counts[i]);
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
     * attachment this profile never uses has no role to play. An input
     * reference is a use, which is the whole point of the shape: a later
     * subpass reading an earlier attachment names it there and nowhere else. */
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        VkBool32 reachable = colors[0].attachment == i || depths[0].attachment == i;
        for (uint32_t s = 0; s < info->subpassCount && !reachable; ++s)
            for (uint32_t r = 0; r < input_counts[s]; ++r)
                if (info->pSubpasses[s].pInputAttachments[r].attachment == i) {
                    reachable = VK_TRUE;
                    break;
                }
        if (!reachable) return VK_ERROR_FEATURE_NOT_PRESENT;
    }
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
    /* The multiview obligations are checked against the pass this call is
     * creating, after the dependencies they refer to are themselves valid.
     * This profile advertises no multiview feature, so every accepted view mask
     * must be zero and the same function is exercised with the feature enabled
     * by the host regressions. The private diagnostic gate is the only thing
     * that can pass a real feature state instead, and it does not touch what the
     * device reports: the shipping build keeps multiview disabled with a zero
     * view limit, and a non-zero mask stays refused there. */
    struct ps5vk_render_pass_multiview owned_multiview = {0};
    if (multiview) {
        /* Shipping: a real view mask is accepted only on a device that ENABLED
         * the feature, which vkCreateDevice grants only for VK_KHR_multiview
         * with the measured capability and the properties2 dependency. The
         * private diagnostic build keeps its own gate, so the existing witness
         * is unchanged and keeps reporting exactly what it always did. */
#if PS5VK_MULTIVIEW_DIAGNOSTIC
        const VkBool32 multiview_enabled = VK_TRUE;
        const uint32_t max_multiview_view_count = PS5VK_MULTIVIEW_DIAGNOSTIC_VIEWS;
#else
        const VkBool32 multiview_enabled =
            (d->enabled_features & PS5VK_FEATURE_MULTIVIEW) != 0;
        const uint32_t max_multiview_view_count = (uint32_t)PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR;
#endif
        VkResult rc = ps5vk_render_pass_multiview_validate(info, multiview,
            multiview_enabled, max_multiview_view_count, &owned_multiview);
        if (rc != VK_SUCCESS) return rc;
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
    /* Suballocate the owned arrays from the single block in descending
     * alignment order. All current array element types have 32-bit alignment;
     * keeping the strictest-alignment-first rule explicit prevents a future
     * element with wider alignment from being placed after an unpadded array.
     * The object's own size is aligned for every pointer it stores. */
    unsigned char *cursor = (unsigned char *)pass + sizeof(*pass);
    struct ps5vk_subpass *subpasses = (struct ps5vk_subpass *)(void *)cursor;
    cursor += (size_t)info->subpassCount * sizeof(*subpasses);
    VkAttachmentDescription *attachments = (VkAttachmentDescription *)(void *)cursor;
    cursor += (size_t)info->attachmentCount * sizeof(*attachments);
    VkSubpassDependency *dependencies = (VkSubpassDependency *)(void *)cursor;
    cursor += (size_t)info->dependencyCount * sizeof(*dependencies);
    VkAttachmentReference *inputs = (VkAttachmentReference *)(void *)cursor;
    memcpy(attachments, info->pAttachments,
           (size_t)info->attachmentCount * sizeof(*attachments));
    if (info->dependencyCount)
        memcpy(dependencies, info->pDependencies,
               (size_t)info->dependencyCount * sizeof(*dependencies));
    /* Each subpass's input references are copied into the pass's own block, in
     * subpass order, so no caller array is retained and a caller that mutates
     * its structs afterwards cannot change this pass. */
    uint32_t input_total = 0;
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        subpasses[i].color = colors[i];
        subpasses[i].depth = depths[i];
        subpasses[i].input_first = input_total;
        subpasses[i].input_count = input_counts[i];
        if (input_counts[i]) {
            memcpy(&inputs[input_total], info->pSubpasses[i].pInputAttachments,
                   (size_t)input_counts[i] * sizeof(*inputs));
            input_total += input_counts[i];
        }
    }
    pass->attachments = attachments;
    pass->subpasses = subpasses;
    pass->dependencies = dependencies;
    pass->inputs = inputs;
    pass->input_count = input_total;
    pass->multiview = owned_multiview;
    ++d->graphics_objects; *out = pass; return VK_SUCCESS;
}

VkResult ps5vk_render_pass_multiview_validate(const VkRenderPassCreateInfo *info,
    const VkRenderPassMultiviewCreateInfo *multiview, VkBool32 multiview_enabled,
    uint32_t max_multiview_view_count, struct ps5vk_render_pass_multiview *out)
{
    if (!info || !multiview || !out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    /* This helper is exported and directly exercised, so it validates its own
     * inputs before it copies or dereferences anything rather than trusting
     * that a caller already went through vkCreateRenderPass: the structure's
     * implicit sType obligation (VUID-VkRenderPassMultiviewCreateInfo-sType-sType),
     * the pass counts the fixed-size owned arrays can hold, and the pass
     * dependency list the view-offset obligations below read. A direct call can
     * therefore never overrun view_masks/view_offsets or dereference a missing
     * pDependencies. */
    if (multiview->sType != VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)
        return VK_ERROR_UNKNOWN;
    if (info->subpassCount > PS5VK_MAX_SUBPASSES ||
        info->dependencyCount > PS5VK_MAX_DEPENDENCIES ||
        (info->dependencyCount && !info->pDependencies))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* A chained structure, a count that does not match the pass, or a missing
     * array where one is required cannot be interpreted at all. */
    if (multiview->pNext ||
        (multiview->subpassCount && multiview->subpassCount != info->subpassCount) ||
        (multiview->dependencyCount && multiview->dependencyCount != info->dependencyCount) ||
        (multiview->subpassCount && !multiview->pViewMasks) ||
        (multiview->dependencyCount && !multiview->pViewOffsets) ||
        multiview->correlationMaskCount > PS5VK_MAX_CORRELATION_MASKS ||
        (multiview->correlationMaskCount && !multiview->pCorrelationMasks))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t view_masks[PS5VK_MAX_SUBPASSES] = {0};
    int32_t view_offsets[PS5VK_MAX_DEPENDENCIES] = {0};
    const uint32_t subpass_count = multiview->subpassCount;
    const uint32_t dependency_count = multiview->dependencyCount;
    for (uint32_t i = 0; i < subpass_count; ++i) view_masks[i] = multiview->pViewMasks[i];
    for (uint32_t i = 0; i < dependency_count; ++i) view_offsets[i] = multiview->pViewOffsets[i];
    /* 02513: multiview is all-or-nothing for a render pass, so the masks are
     * either all zero (multiview disabled) or all non-zero. */
    unsigned non_zero = 0;
    for (uint32_t i = 0; i < subpass_count; ++i) non_zero += view_masks[i] != 0;
    if (non_zero && non_zero != subpass_count) return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkBool32 enabled = non_zero != 0;
    /* 06555: without the feature, every view mask must be zero. 06697: with
     * it, the most significant bit of each mask must be below the reported
     * maxMultiviewViewCount. */
    if (enabled && !multiview_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (enabled) {
        for (uint32_t i = 0; i < subpass_count; ++i) {
            uint32_t msb = 0;
            for (uint32_t bit = 0; bit < 32u; ++bit)
                if (view_masks[i] & (UINT32_C(1) << bit)) msb = bit;
            if (msb >= max_multiview_view_count) return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    for (uint32_t i = 0; i < dependency_count; ++i) {
        const VkSubpassDependency *dep = &info->pDependencies[i];
        const VkBool32 view_local =
            (dep->dependencyFlags & VK_DEPENDENCY_VIEW_LOCAL_BIT) != 0;
        /* 02512: a view offset is only meaningful for a view-local dependency.
         * 01930: a non-zero offset needs two different subpasses to relate. */
        if (!view_local && view_offsets[i]) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (view_offsets[i] && dep->srcSubpass == dep->dstSubpass)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        /* 02514: with every mask zero there is no view for a view-local
         * dependency to relate. */
        if (!enabled && view_local) return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    /* 02515: correlation masks describe views that may be rendered
     * concurrently, which cannot be stated when multiview is disabled. */
    if (!enabled && multiview->correlationMaskCount) return VK_ERROR_FEATURE_NOT_PRESENT;
    /* 00841: a view index may not appear in more than one correlation mask.
     * They remain hints: nothing in the driver executes them. */
    uint32_t seen = 0;
    for (uint32_t i = 0; i < multiview->correlationMaskCount; ++i) {
        const uint32_t mask = multiview->pCorrelationMasks[i];
        for (uint32_t bit = 0; bit < 32u; ++bit) {
            const uint32_t flag = UINT32_C(1) << bit;
            if (!(mask & flag)) continue;
            if (seen & flag) return VK_ERROR_FEATURE_NOT_PRESENT;
            seen |= flag;
        }
    }
    out->present = VK_TRUE;
    out->subpass_count = subpass_count;
    out->dependency_count = dependency_count;
    out->correlation_mask_count = multiview->correlationMaskCount;
    memcpy(out->view_masks, view_masks, sizeof(out->view_masks));
    memcpy(out->view_offsets, view_offsets, sizeof(out->view_offsets));
    if (multiview->correlationMaskCount)
        memcpy(out->correlation_masks, multiview->pCorrelationMasks,
               (size_t)multiview->correlationMaskCount * sizeof(out->correlation_masks[0]));
    return VK_SUCCESS;
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
