#include "vk_render_pass.h"
#include "color_attachment_contract.h"
#include "depth_stencil_layout.h"
#include <stdint.h>
#include <string.h>

#if defined(PS5VK_TARGET_PS5) && PS5VK_TARGET_PS5
#include "ps5log.h"
#define PASS_MARK(...) ps5log_printf(PS5LOG_MARK, __VA_ARGS__)
#else
#define PASS_MARK(...) ((void)0)
#endif

/* The depth-aspect layouts a pass with an explicit stencil table may name for
 * a combined attachment: anything that means an attachment, read-only,
 * GENERAL or transfer state for the depth aspect, including the separate
 * DEPTH_* layouts (VK_KHR_separate_depth_stencil_layouts). */
static int separate_depth_layout(VkImageLayout value, int initial, int reference)
{
    VkImageLayout p;
    if (initial && value == VK_IMAGE_LAYOUT_UNDEFINED) return 1;
    if (!ps5vk_layout_for_aspect(value, VK_IMAGE_ASPECT_DEPTH_BIT, &p)) return 0;
    return p == VK_IMAGE_LAYOUT_GENERAL || p == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ||
        p == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
        (!reference && (p == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
                        p == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
}

/* The same for the stencil half, which never takes a combined layout from the
 * stencil structures (VkAttachmentDescriptionStencilLayout and
 * VkAttachmentReferenceStencilLayout name STENCIL_* or aspect-neutral
 * layouts only). */
static int separate_stencil_layout(VkImageLayout value, int initial, int reference)
{
    VkImageLayout p;
    if (initial && value == VK_IMAGE_LAYOUT_UNDEFINED) return 1;
    if (value == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
        value == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
        ps5vk_layout_is_mixed_depth_stencil(value) ||
        !ps5vk_layout_for_aspect(value, VK_IMAGE_ASPECT_STENCIL_BIT, &p)) return 0;
    return p == VK_IMAGE_LAYOUT_GENERAL || p == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL ||
        p == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL ||
        (!reference && (p == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
                        p == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
}

static int layout(VkImageLayout value, int depth, int initial)
{
    return (initial && value == VK_IMAGE_LAYOUT_UNDEFINED) ||
        value == VK_IMAGE_LAYOUT_GENERAL || value == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        value == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        (!depth && value == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) ||
        value == (depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ||
        (!depth && value == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* The layouts a subpass may name for an input reference. VUID 06912 forbids
 * the colour- and depth-attachment layouts outright for a real input reference
 * (an input attachment is read, not attached), and Vulkan also forbids
 * UNDEFINED, PREINITIALIZED and the transfer/present layouts. Of what remains,
 * this profile models exactly the two read layouts it can describe, so an input
 * reference can never name a layout the rest of the driver has no meaning
 * for. */
static int input_layout(VkImageLayout value)
{
    return value == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
        value == VK_IMAGE_LAYOUT_GENERAL;
}

/* One subpass description against this profile: up to two colour references
 * (none at all is the depth-only shape), an optional D32 depth reference that
 * may not alias any of them, and the input references this model can describe.
 * A resolve reference is accepted as a role of its own - it names the
 * single-sample attachment that receives the resolved result of the colour
 * reference it follows - and a non-empty preserve list is outside the bounded
 * profile. */
static VkResult subpass_valid(const VkSubpassDescription *s, uint32_t attachments,
    VkBool32 separate, struct ps5vk_subpass *out, uint32_t *input_count,
    uint32_t *preserve_count)
{
    /* A subpass names the colour attachments this profile serves, or none at
     * all. Zero is how
     * Vulkan expresses a DEPTH-ONLY pass, which the pinned upstream depth
     * clamp module builds (vktDrawDepthClampTests.cpp: colorAttachmentCount 0
     * with a depth-stencil reference). Vulkan ignores pColorAttachments when
     * the count is zero, so the pointer says nothing there. */
    if (s->flags || s->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS ||
        /* The colour-attachment bound lives in one place: a subpass may only
         * reference the targets the pipeline contract and the native path can
         * serve. */
        !ps5vk_color_attachment_count_supported(s->colorAttachmentCount) ||
        (s->colorAttachmentCount && !s->pColorAttachments) ||
        /* Vulkan IGNORES pInputAttachments when the count is zero, so the
         * pointer says nothing there; a nonzero count is a real request that
         * has to name a valid array. pResolveAttachments is different: a
         * non-null pointer IS the request. */
        (s->inputAttachmentCount && !s->pInputAttachments) ||
        s->inputAttachmentCount > PS5VK_MAX_INPUT_ATTACHMENTS)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* A preserve list is a real part of the shape now (DXVK262-T06): the pinned
     * multisample family keeps the resolve target and its per-sample targets
     * alive across the fetch subpasses this way. Vulkan's rules for it are the
     * ones enforced here: every entry names an attachment of this pass, no entry
     * is VK_ATTACHMENT_UNUSED, no attachment appears twice, and an attachment
     * cannot be both preserved and used by the same subpass - preserving is the
     * promise that its contents survive untouched, which a reference in the same
     * subpass would contradict. */
    if (s->preserveAttachmentCount && !s->pPreserveAttachments)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    for (uint32_t i = 0; i < s->preserveAttachmentCount; ++i) {
        const uint32_t attachment = s->pPreserveAttachments[i];
        if (attachment >= attachments || attachment == VK_ATTACHMENT_UNUSED)
            return VK_ERROR_UNKNOWN;
        for (uint32_t earlier = 0; earlier < i; ++earlier)
            if (s->pPreserveAttachments[earlier] == attachment) return VK_ERROR_UNKNOWN;
        if (s->pDepthStencilAttachment &&
            s->pDepthStencilAttachment->attachment == attachment) return VK_ERROR_UNKNOWN;
        for (uint32_t c = 0; c < s->colorAttachmentCount; ++c)
            if (s->pColorAttachments[c].attachment == attachment) return VK_ERROR_UNKNOWN;
        for (uint32_t c = 0; c < s->colorAttachmentCount && s->pResolveAttachments; ++c)
            if (s->pResolveAttachments[c].attachment == attachment) return VK_ERROR_UNKNOWN;
    }
    *preserve_count = s->preserveAttachmentCount;
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
    out->color_count = s->colorAttachmentCount;
    for (uint32_t i = 0; i < s->colorAttachmentCount; ++i) {
        const VkAttachmentReference *reference = &s->pColorAttachments[i];
        /* A colour reference must name an attachment of this pass and a layout
         * a colour attachment may use. Two references to the same attachment
         * are legal Vulkan but not a shape this profile can render - one
         * surface cannot be two targets - so they are refused here rather than
         * accepted and failed later. */
        if (reference->attachment >= attachments ||
            (reference->layout != VK_IMAGE_LAYOUT_GENERAL &&
             reference->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL))
            return VK_ERROR_UNKNOWN;
        for (uint32_t earlier = 0; earlier < i; ++earlier)
            if (s->pColorAttachments[earlier].attachment == reference->attachment)
                return VK_ERROR_UNKNOWN;
        out->color[i] = *reference;
    }
    out->depth.attachment = VK_ATTACHMENT_UNUSED;
    out->depth.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (s->pDepthStencilAttachment) out->depth = *s->pDepthStencilAttachment;
    /* Something must be rendered into. A subpass that names neither a colour
     * nor a depth attachment has no target at all. */
    if (!out->color_count && out->depth.attachment == VK_ATTACHMENT_UNUSED)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    for (uint32_t i = 0; i < s->colorAttachmentCount; ++i)
        if (out->depth.attachment != VK_ATTACHMENT_UNUSED &&
            (out->depth.attachment >= attachments ||
             out->depth.attachment == out->color[i].attachment))
            return VK_ERROR_UNKNOWN;
    if (out->depth.attachment != VK_ATTACHMENT_UNUSED &&
        (separate ? !separate_depth_layout(out->depth.layout, 0, 1) :
         (out->depth.layout != VK_IMAGE_LAYOUT_GENERAL &&
          out->depth.layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)))
        return VK_ERROR_UNKNOWN;
    /* pResolveAttachments, when present, carries exactly one entry per colour
     * reference: entry i resolves colour reference i, and an entry may be
     * VK_ATTACHMENT_UNUSED. A real entry must name an attachment of this pass,
     * must use a layout a colour target may use, and may not be any colour or
     * depth reference of the same subpass - one surface cannot be both what
     * the subpass renders into and what receives the resolved result. */
    for (uint32_t i = 0; i < s->colorAttachmentCount; ++i)
        out->resolve[i] = (VkAttachmentReference){
            VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
    out->resolve_count = 0;
    if (s->pResolveAttachments) {
        /* Vulkan requires one resolve entry per colour reference, so the count
         * is the colour count: the entries themselves may be UNUSED. */
        out->resolve_count = s->colorAttachmentCount;
        for (uint32_t i = 0; i < s->colorAttachmentCount; ++i) {
            const VkAttachmentReference *reference = &s->pResolveAttachments[i];
            if (reference->attachment == VK_ATTACHMENT_UNUSED) continue;
            if (reference->attachment >= attachments ||
                (reference->layout != VK_IMAGE_LAYOUT_GENERAL &&
                 reference->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ||
                reference->attachment == s->pColorAttachments[i].attachment)
                return VK_ERROR_UNKNOWN;
            for (uint32_t c = 0; c < s->colorAttachmentCount; ++c)
                if (s->pColorAttachments[c].attachment == reference->attachment)
                    return VK_ERROR_UNKNOWN;
            if (out->depth.attachment == reference->attachment) return VK_ERROR_UNKNOWN;
            for (uint32_t earlier = 0; earlier < i; ++earlier)
                if (s->pResolveAttachments[earlier].attachment == reference->attachment)
                    return VK_ERROR_UNKNOWN;
            out->resolve[i] = *reference;
        }
    }
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
    size_t inputs = 0, preserves = 0;
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        const size_t count = info->pSubpasses[i].inputAttachmentCount;
        if (count > (SIZE_MAX - inputs) / sizeof(VkAttachmentReference))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        inputs += count * sizeof(VkAttachmentReference);
        const size_t kept = info->pSubpasses[i].preserveAttachmentCount;
        if (kept > (SIZE_MAX - preserves) / sizeof(uint32_t))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        preserves += kept * sizeof(uint32_t);
    }
    /* Same order as the suballocation below, so the total cannot drift from
     * the layout it is supposed to cover. */
    const size_t parts[5] = {
        (size_t)info->subpassCount * sizeof(struct ps5vk_subpass),
        (size_t)info->attachmentCount * sizeof(VkAttachmentDescription),
        (size_t)info->dependencyCount * sizeof(VkSubpassDependency),
        inputs,
        preserves,
    };
    for (unsigned i = 0; i < 5; ++i) {
        /* The counts are already bounded by the profile limits checked above,
         * so these cannot overflow; the checks are kept so a later limit
         * change cannot turn into a silent wrap. */
        if (parts[i] > SIZE_MAX - bytes) return VK_ERROR_OUT_OF_HOST_MEMORY;
        bytes += parts[i];
    }
    *total = bytes;
    return VK_SUCCESS;
}

/* Whether a pass names a maintenance2 mixed layout for a combined
 * depth/stencil attachment, and if so its stencil projections. VK_SUCCESS
 * fills `out`; VK_NOT_READY means no mixed layout is named (the combined
 * model applies unchanged); VK_ERROR_UNKNOWN means a combined layout has no
 * stencil meaning. Counts and pointers are checked by the caller's profile
 * bounds first only where they are dereferenced here. */
static VkResult mixed_layout_table(const VkRenderPassCreateInfo *info,
    struct ps5vk_render_pass_stencil_layouts *out)
{
    if (!info->pAttachments || !info->pSubpasses ||
        info->attachmentCount > PS5VK_MAX_ATTACHMENTS ||
        info->subpassCount > PS5VK_MAX_SUBPASSES) return VK_NOT_READY;
    int mixed = 0;
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        const VkAttachmentDescription *a = &info->pAttachments[i];
        if (ps5vk_format_is_combined_depth_stencil(a->format))
            mixed |= ps5vk_layout_is_mixed_depth_stencil(a->initialLayout) ||
                ps5vk_layout_is_mixed_depth_stencil(a->finalLayout);
    }
    for (uint32_t s = 0; s < info->subpassCount; ++s) {
        const VkAttachmentReference *r = info->pSubpasses[s].pDepthStencilAttachment;
        if (r && r->attachment < info->attachmentCount &&
            ps5vk_format_is_combined_depth_stencil(info->pAttachments[r->attachment].format))
            mixed |= ps5vk_layout_is_mixed_depth_stencil(r->layout);
    }
    if (!mixed) return VK_NOT_READY;
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        const VkAttachmentDescription *a = &info->pAttachments[i];
        out->initial[i] = a->initialLayout;
        out->final[i] = a->finalLayout;
        if (ps5vk_format_is_combined_depth_stencil(a->format) &&
            (!ps5vk_layout_for_aspect(a->initialLayout, VK_IMAGE_ASPECT_STENCIL_BIT,
                                      &out->initial[i]) ||
             !ps5vk_layout_for_aspect(a->finalLayout, VK_IMAGE_ASPECT_STENCIL_BIT,
                                      &out->final[i])))
            return VK_ERROR_UNKNOWN;
    }
    for (uint32_t s = 0; s < info->subpassCount; ++s) {
        const VkAttachmentReference *r = info->pSubpasses[s].pDepthStencilAttachment;
        out->reference[s] = r ? r->layout : VK_IMAGE_LAYOUT_UNDEFINED;
        if (r && r->attachment < info->attachmentCount &&
            ps5vk_format_is_combined_depth_stencil(info->pAttachments[r->attachment].format) &&
            !ps5vk_layout_for_aspect(r->layout, VK_IMAGE_ASPECT_STENCIL_BIT, &out->reference[s]))
            return VK_ERROR_UNKNOWN;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice d,
    const VkRenderPassCreateInfo *info, const VkAllocationCallbacks *allocator, VkRenderPass *out)
{
    return ps5vk_render_pass_create(d, info, NULL, allocator, out);
}

VkResult ps5vk_render_pass_create(VkDevice d, const VkRenderPassCreateInfo *info,
    const struct ps5vk_render_pass_stencil_layouts *stencil,
    const VkAllocationCallbacks *allocator, VkRenderPass *out)
{
    PASS_MARK("PS5VK_RENDER_PASS_CREATE attachments=%u subpasses=%u dependencies=%u",
        info ? info->attachmentCount : 0u, info ? info->subpassCount : 0u,
        info ? info->dependencyCount : 0u);
    if (!out) return VK_ERROR_UNKNOWN;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO)
        return VK_ERROR_UNKNOWN;
    if (!d->graphics_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
    /* One optional VkRenderPassMultiviewCreateInfo and, on a device that
     * enabled VK_KHR_maintenance2, one VkRenderPassInputAttachmentAspectCreateInfo
     * are understood; any other structure, or a second copy of either, stays
     * fail-closed. */
    const VkRenderPassMultiviewCreateInfo *multiview = NULL;
    const VkRenderPassInputAttachmentAspectCreateInfo *input_aspects = NULL;
    for (const VkBaseInStructure *next = (const VkBaseInStructure *)info->pNext;
         next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO && !multiview)
            multiview = (const VkRenderPassMultiviewCreateInfo *)next;
        else if (next->sType == VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO &&
                 !input_aspects && d->maintenance2_extension_enabled)
            input_aspects = (const VkRenderPassInputAttachmentAspectCreateInfo *)next;
        else
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (info->flags ||
        !info->subpassCount || info->subpassCount > PS5VK_MAX_SUBPASSES || !info->pSubpasses ||
        !info->attachmentCount || info->attachmentCount > PS5VK_MAX_ATTACHMENTS ||
        !info->pAttachments ||
        info->dependencyCount > PS5VK_MAX_DEPENDENCIES ||
        (info->dependencyCount && !info->pDependencies))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* VK_KHR_maintenance2's two mixed layouts give the aspects of a combined
     * attachment different access through one layout value. They are read
     * per aspect, exactly like an explicit stencil table: when a pass without
     * one names a mixed layout for a combined attachment, the table is the
     * stencil projection of every combined layout. A layout with no stencil
     * meaning (a DEPTH_* one) cannot be projected and is refused, so this
     * never admits the separate layouts on the maintenance2 route alone. */
    struct ps5vk_render_pass_stencil_layouts projected;
    if (!stencil && d->maintenance2_extension_enabled) {
        const VkResult rc = mixed_layout_table(info, &projected);
        if (rc == VK_ERROR_UNKNOWN) return rc;
        if (rc == VK_SUCCESS) stencil = &projected;
    }
    struct ps5vk_subpass colors[PS5VK_MAX_SUBPASSES];
    VkAttachmentReference depths[PS5VK_MAX_SUBPASSES];
    uint32_t input_counts[PS5VK_MAX_SUBPASSES];
    uint32_t preserve_counts[PS5VK_MAX_SUBPASSES];
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        VkResult rc = subpass_valid(&info->pSubpasses[i], info->attachmentCount,
                                    stencil != NULL, &colors[i], &input_counts[i],
                                    &preserve_counts[i]);
        if (rc != VK_SUCCESS) return rc;
        depths[i] = colors[i].depth;
    }
    /* Each subpass names the attachments it renders into, reads and preserves
     * (DXVK262-T06). A framebuffer is an array of views indexed by attachment,
     * so subpasses may differ: the pinned multisample family renders into the
     * multisampled colour in subpass 0 and, in each following fetch subpass,
     * reads it as an input attachment while writing one per-sample target and
     * preserving the rest. Every reference is checked per subpass below and
     * framebuffer compatibility is checked per reference rather than by role.
     * The native queue still refuses a graph it cannot execute - it carries the
     * shared-role shape only - so nothing here reaches hardware that way. */
    /* Input aspects (VK_KHR_maintenance2). Each entry names an input reference
     * that exists (VUID-VkRenderPassCreateInfo-pNext-01926 and -01927) and an
     * aspect mask the version-2 reference would carry, checked by the same
     * rule against the attachment it names (01963). Because the owned input
     * reference reads every aspect, an accepted mask changes nothing stored. */
    if (input_aspects) {
        if (!input_aspects->aspectReferenceCount || !input_aspects->pAspectReferences)
            return VK_ERROR_UNKNOWN;
        for (uint32_t i = 0; i < input_aspects->aspectReferenceCount; ++i) {
            const VkInputAttachmentAspectReference *r = &input_aspects->pAspectReferences[i];
            if (r->subpass >= info->subpassCount ||
                r->inputAttachmentIndex >= info->pSubpasses[r->subpass].inputAttachmentCount)
                return VK_ERROR_UNKNOWN;
            const uint32_t attachment = info->pSubpasses[r->subpass]
                .pInputAttachments[r->inputAttachmentIndex].attachment;
            if (attachment == VK_ATTACHMENT_UNUSED) continue;
            VkResult rc = ps5vk_render_pass_input_aspect_valid(
                info->pAttachments[attachment].format, r->aspectMask);
            if (rc != VK_SUCCESS) return rc;
        }
    }
    /* Every attachment must be reachable through a subpass reference: an
     * attachment this profile never uses has no role to play. An input
     * reference is a use, which is the whole point of the shape: a later
     * subpass reading an earlier attachment names it there and nowhere else.
     * A resolve target is a use too: it is named by the reference that resolves
     * into it. */
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        VkBool32 reachable = VK_FALSE;
        for (uint32_t s = 0; s < info->subpassCount && !reachable; ++s) {
            const struct ps5vk_subpass *sp = &colors[s];
            if (depths[s].attachment == i) reachable = VK_TRUE;
            for (uint32_t c = 0; c < sp->color_count && !reachable; ++c)
                reachable = sp->color[c].attachment == i;
            for (uint32_t c = 0; c < sp->resolve_count && !reachable; ++c)
                reachable = sp->resolve[c].attachment == i;
            for (uint32_t r = 0; r < preserve_counts[s] && !reachable; ++r)
                reachable = info->pSubpasses[s].pPreserveAttachments[r] == i;
            for (uint32_t r = 0; r < input_counts[s] && !reachable; ++r)
                reachable = info->pSubpasses[s].pInputAttachments[r].attachment == i;
        }
        if (!reachable) return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    /* Per-attachment and per-subpass checks. An attachment may play different
     * roles in different subpasses, so what it must satisfy is the union of the
     * roles it is named in, and the sample-count agreement is a rule of each
     * subpass rather than of the whole pass (DXVK262-T06). */
    const VkSampleCountFlags served = ps5vk_platform_sample_counts(d->platform_features);
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        const VkAttachmentDescription *a = &info->pAttachments[i];
        int as_colour = 0, as_depth = 0, as_resolve = 0;
        for (uint32_t s = 0; s < info->subpassCount; ++s) {
            const struct ps5vk_subpass *sp = &colors[s];
            if (depths[s].attachment == i) as_depth = 1;
            for (uint32_t c = 0; c < sp->color_count; ++c)
                if (sp->color[c].attachment == i) as_colour = 1;
            for (uint32_t c = 0; c < sp->resolve_count; ++c)
                if (sp->resolve[c].attachment == i) as_resolve = 1;
        }
        /* Flags, the load/store operation ranges and the layouts are properties
         * of the attachment; the layout rules are the ones the roles it plays
         * need. A colour, resolve or depth role each bounds the format: the two
         * colour formats this profile renders into, and D32 for depth. */
        if (a->flags ||
            ((as_colour || as_resolve) && !ps5vk_color_target_format_supported(a->format)) ||
            (as_depth && a->format != VK_FORMAT_D32_SFLOAT &&
                         a->format != VK_FORMAT_D16_UNORM &&
                         a->format != VK_FORMAT_D32_SFLOAT_S8_UINT))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        /* With an explicit stencil table the depth half of a combined
         * attachment may use the separate DEPTH_* layouts, and its stencil
         * half is checked on its own; without one, both ride on one combined
         * layout exactly as before. A depth reference in a separate layout
         * must name a combined attachment. */
        const int combined = as_depth && ps5vk_format_is_combined_depth_stencil(a->format);
        if (stencil && combined) {
            if (!separate_depth_layout(a->initialLayout, 1, 0) ||
                !separate_depth_layout(a->finalLayout, 0, 0) ||
                !separate_stencil_layout(stencil->initial[i], 1, 0) ||
                !separate_stencil_layout(stencil->final[i], 0, 0) ||
                (a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
                 a->initialLayout == VK_IMAGE_LAYOUT_UNDEFINED) ||
                (a->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
                 stencil->initial[i] == VK_IMAGE_LAYOUT_UNDEFINED) ||
                a->loadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
                a->storeOp > VK_ATTACHMENT_STORE_OP_DONT_CARE ||
                a->stencilLoadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
                a->stencilStoreOp > VK_ATTACHMENT_STORE_OP_DONT_CARE)
                return VK_ERROR_UNKNOWN;
            for (uint32_t s = 0; s < info->subpassCount; ++s)
                if (depths[s].attachment == i &&
                    !separate_stencil_layout(stencil->reference[s], 0, 1))
                    return VK_ERROR_UNKNOWN;
            continue;
        }
        if (as_depth)
            for (uint32_t s = 0; s < info->subpassCount; ++s)
                if (depths[s].attachment == i &&
                    depths[s].layout != VK_IMAGE_LAYOUT_GENERAL &&
                    depths[s].layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
                    return VK_ERROR_UNKNOWN;
        if (a->loadOp < VK_ATTACHMENT_LOAD_OP_LOAD || a->loadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
            a->storeOp < VK_ATTACHMENT_STORE_OP_STORE || a->storeOp > VK_ATTACHMENT_STORE_OP_DONT_CARE ||
            a->stencilLoadOp < VK_ATTACHMENT_LOAD_OP_LOAD || a->stencilLoadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
            a->stencilStoreOp < VK_ATTACHMENT_STORE_OP_STORE || a->stencilStoreOp > VK_ATTACHMENT_STORE_OP_DONT_CARE ||
            !layout(a->initialLayout, as_depth, 1) || !layout(a->finalLayout, as_depth, 0) ||
            (a->initialLayout == VK_IMAGE_LAYOUT_UNDEFINED && a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD))
            return VK_ERROR_UNKNOWN;
    }
    for (uint32_t s = 0; s < info->subpassCount; ++s) {
        const struct ps5vk_subpass *sp = &colors[s];
        VkSampleCountFlagBits subpass_samples = VK_SAMPLE_COUNT_1_BIT;
        int rendered = 0;
        /* The counts this device serves (DXVK262-T06): every attachment a
         * subpass renders into agrees on one of them. The depth role stays 1x -
         * no multisampled depth target exists on this path yet - and a resolve
         * target is single-sample by definition. */
        for (uint32_t c = 0; c < sp->color_count; ++c) {
            const VkAttachmentDescription *a = &info->pAttachments[sp->color[c].attachment];
            if (!(served & a->samples)) return VK_ERROR_FEATURE_NOT_PRESENT;
            if (!rendered) { subpass_samples = a->samples; rendered = 1; }
            else if (a->samples != subpass_samples) return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (sp->depth.attachment != VK_ATTACHMENT_UNUSED) {
            const VkAttachmentDescription *a = &info->pAttachments[sp->depth.attachment];
            if (a->samples != VK_SAMPLE_COUNT_1_BIT ||
                (rendered && a->samples != subpass_samples))
                return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        for (uint32_t c = 0; c < sp->resolve_count; ++c) {
            const VkAttachmentReference *r = &sp->resolve[c];
            if (r->attachment == VK_ATTACHMENT_UNUSED) continue;
            const VkAttachmentDescription *target = &info->pAttachments[r->attachment];
            const VkAttachmentDescription *source = &info->pAttachments[sp->color[c].attachment];
            if (target->samples != VK_SAMPLE_COUNT_1_BIT || target->format != source->format)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    for (uint32_t i = 0; i < info->dependencyCount; ++i) {
        const VkSubpassDependency *dep = &info->pDependencies[i];
        const VkBool32 src_external = dep->srcSubpass == VK_SUBPASS_EXTERNAL;
        const VkBool32 dst_external = dep->dstSubpass == VK_SUBPASS_EXTERNAL;
        /* Forward dependencies are serviced by the native subpass boundary.
         * A view-local, BY_REGION self-dependency declares the scope available
         * to an in-pass barrier; it does not itself execute a barrier. The
         * command recorder still checks every actual in-pass operation. */
        const VkBool32 self = !src_external && !dst_external &&
            dep->srcSubpass == dep->dstSubpass;
        const VkDependencyFlags local_region =
            VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_VIEW_LOCAL_BIT;
        if ((!src_external && dep->srcSubpass >= info->subpassCount) ||
            (!dst_external && dep->dstSubpass >= info->subpassCount) ||
            (src_external && dst_external) ||
            (!src_external && !dst_external && dep->srcSubpass > dep->dstSubpass) ||
            (self && (dep->dependencyFlags & local_region) != local_region) ||
            (dep->dependencyFlags & ~local_region) ||
            ((dep->dependencyFlags & VK_DEPENDENCY_VIEW_LOCAL_BIT) &&
             (!multiview || src_external || dst_external)) ||
            !dep->srcStageMask || !dep->dstStageMask) return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    /* Validate masks and view-local dependencies against the enabled device
     * feature and measured view limit, after validating the dependency graph.
     * Merely placing a multiview structure in pNext does not enable a feature. */
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
    size_t input_elements = 0, preserve_elements = 0;
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        input_elements += input_counts[i];
        preserve_elements += preserve_counts[i];
    }
    cursor += input_elements * sizeof(*inputs);
    uint32_t *preserves = (uint32_t *)(void *)cursor;
    cursor += preserve_elements * sizeof(*preserves);
    memcpy(attachments, info->pAttachments,
           (size_t)info->attachmentCount * sizeof(*attachments));
    if (info->dependencyCount)
        memcpy(dependencies, info->pDependencies,
               (size_t)info->dependencyCount * sizeof(*dependencies));
    /* Each subpass's input references are copied into the pass's own block, in
     * subpass order, so no caller array is retained and a caller that mutates
     * its structs afterwards cannot change this pass. */
    uint32_t input_total = 0, preserve_total = 0;
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        /* The validated subpass is copied whole: its colour-reference array,
         * the count that bounds it and the depth reference. */
        subpasses[i] = colors[i];
        subpasses[i].depth = depths[i];
        subpasses[i].input_first = input_total;
        subpasses[i].input_count = input_counts[i];
        if (input_counts[i]) {
            memcpy(&inputs[input_total], info->pSubpasses[i].pInputAttachments,
                   (size_t)input_counts[i] * sizeof(*inputs));
            input_total += input_counts[i];
        }
        subpasses[i].preserve_first = preserve_total;
        subpasses[i].preserve_count = preserve_counts[i];
        if (preserve_counts[i]) {
            memcpy(&preserves[preserve_total], info->pSubpasses[i].pPreserveAttachments,
                   (size_t)preserve_counts[i] * sizeof(*preserves));
            preserve_total += preserve_counts[i];
        }
    }
    pass->attachments = attachments;
    pass->subpasses = subpasses;
    pass->dependencies = dependencies;
    pass->inputs = inputs;
    pass->input_count = input_total;
    pass->preserves = preserves;
    pass->preserve_count = preserve_total;
    pass->multiview = owned_multiview;
    /* The stencil halves: explicit for a combined attachment when a table
     * was supplied, the combined layouts otherwise. */
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        const int explicit_stencil = stencil &&
            ps5vk_format_is_combined_depth_stencil(info->pAttachments[i].format);
        pass->stencil.initial[i] = explicit_stencil ? stencil->initial[i] :
            info->pAttachments[i].initialLayout;
        pass->stencil.final[i] = explicit_stencil ? stencil->final[i] :
            info->pAttachments[i].finalLayout;
    }
    for (uint32_t s = 0; s < info->subpassCount; ++s) {
        const uint32_t a = depths[s].attachment;
        pass->stencil.reference[s] = (stencil && a != VK_ATTACHMENT_UNUSED &&
            ps5vk_format_is_combined_depth_stencil(info->pAttachments[a].format)) ?
            stencil->reference[s] : depths[s].layout;
    }
    ++d->graphics_objects; *out = pass; return VK_SUCCESS;
}

/* VK_KHR_create_renderpass2.
 *
 * A VkRenderPassCreateInfo2 describes the same object a VkRenderPassCreateInfo
 * plus a chained VkRenderPassMultiviewCreateInfo describes: the per-subpass
 * view masks, the per-dependency view offsets and the correlated view masks
 * move out of the multiview structure into the version-2 structures, and each
 * attachment reference gains an aspect mask that only an input reference
 * uses. The call therefore validates what is new in the version-2 structures
 * and then TRANSLATES the pass into the version-1 form, which it hands to
 * vkCreateRenderPass. There is exactly one render pass model and one set of
 * profile rules; a pass that the version-1 path refuses is refused here with
 * the same result, and a pass it accepts becomes the same object.
 *
 * Every translation array lives on the stack and is bounded by the profile
 * limits the version-1 path enforces, so a count beyond them is refused before
 * anything is copied and nothing is allocated until vkCreateRenderPass makes
 * its single allocation. */

/* The aspects a format has. The render pass profile only admits colour formats
 * and the depth-only D16/D32 formats, but the input-aspect rule is stated
 * against the attachment's format whatever the rest of the profile decides, so
 * the stencil and combined formats are named here too. */
static VkImageAspectFlags format_aspects(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkResult ps5vk_render_pass_input_aspect_valid(VkFormat format, VkImageAspectFlags aspect)
{
    const VkImageAspectFlags present = format_aspects(format);
    /* VUID-VkSubpassDescription2-attachment-02800 (not zero), -02801 (never
     * METADATA), -04563 (never a memory plane) and
     * VUID-VkRenderPassCreateInfo2-attachment-02525 (only aspects the
     * attachment's format has). */
    if (!aspect || (aspect & ~present)) return VK_ERROR_UNKNOWN;
    /* A version-1 input reference reads every aspect of its attachment. For
     * the single-aspect formats this profile serves the only valid mask IS
     * every aspect, so nothing valid is refused here; a strict subset of a
     * combined depth/stencil format would need a per-reference aspect this
     * model does not store yet, and is refused rather than widened. */
    if (aspect != present) return VK_ERROR_FEATURE_NOT_PRESENT;
    return VK_SUCCESS;
}

/* pNext of VkAttachmentDescription2. One VkAttachmentDescriptionStencilLayout
 * (VK_KHR_separate_depth_stencil_layouts) is understood and returned; the
 * owned per-aspect table in the render pass carries it. Anything else, or a
 * second copy, is refused rather than silently ignored. */
static VkResult attachment2_next(const VkAttachmentDescription2 *a,
    const VkAttachmentDescriptionStencilLayout **stencil)
{
    *stencil = NULL;
    for (const VkBaseInStructure *next = (const VkBaseInStructure *)a->pNext;
         next; next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT || *stencil)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        *stencil = (const VkAttachmentDescriptionStencilLayout *)next;
    }
    return VK_SUCCESS;
}

/* pNext of VkAttachmentReference2. VkAttachmentReferenceStencilLayout is
 * understood only where the caller passes `stencil` (the depth/stencil
 * reference); a colour, resolve or input reference refuses it, since no
 * stencil layout of those roles is modelled. */
static VkResult reference2_next(const VkAttachmentReference2 *r,
    const VkAttachmentReferenceStencilLayout **stencil)
{
    if (stencil) *stencil = NULL;
    for (const VkBaseInStructure *next = (const VkBaseInStructure *)r->pNext;
         next; next = next->pNext) {
        if (!stencil || next->sType != VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT ||
            *stencil)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        *stencil = (const VkAttachmentReferenceStencilLayout *)next;
    }
    return VK_SUCCESS;
}

/* One version-2 reference into its version-1 form. The aspect mask is not
 * part of the version-1 reference; input references check it separately. */
static VkResult reference2(const VkAttachmentReference2 *r, VkAttachmentReference *out,
    const VkAttachmentReferenceStencilLayout **stencil)
{
    if (r->sType != VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2) return VK_ERROR_UNKNOWN;
    VkResult rc = reference2_next(r, stencil);
    if (rc != VK_SUCCESS) return rc;
    *out = (VkAttachmentReference){r->attachment, r->layout};
    return VK_SUCCESS;
}

/* pNext of VkSubpassDescription2. Depth/stencil resolve
 * (VK_KHR_depth_stencil_resolve), a fragment shading rate attachment and every
 * other extension of the subpass are outside this profile and refused. */
static VkResult subpass2_next(const VkSubpassDescription2 *s)
{
    return s->pNext ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS;
}

/* pNext of VkSubpassDependency2. A chained VkMemoryBarrier2 needs
 * synchronization2, which this device does not expose. */
static VkResult dependency2_next(const VkSubpassDependency2 *d)
{
    return d->pNext ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS;
}

static VkResult subpass2(const VkRenderPassCreateInfo2 *info, uint32_t index,
    struct ps5vk_render_pass2_refs *refs, VkSubpassDescription *out,
    const VkAttachmentReferenceStencilLayout **depth_stencil)
{
    *depth_stencil = NULL;
    const VkSubpassDescription2 *s = &info->pSubpasses[index];
    if (s->sType != VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2) return VK_ERROR_UNKNOWN;
    VkResult rc = subpass2_next(s);
    if (rc != VK_SUCCESS) return rc;
    /* The profile bounds are the ones the version-1 path would apply to the
     * same counts, and with the same result. */
    if (s->colorAttachmentCount > PS5VK_MAX_COLOR_ATTACHMENTS ||
        (s->colorAttachmentCount && !s->pColorAttachments) ||
        s->inputAttachmentCount > PS5VK_MAX_INPUT_ATTACHMENTS ||
        (s->inputAttachmentCount && !s->pInputAttachments) ||
        (s->preserveAttachmentCount && !s->pPreserveAttachments))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* More preserve entries than attachments must repeat one or name one
     * outside the pass, which the version-1 path refuses as invalid. */
    if (s->preserveAttachmentCount > info->attachmentCount) return VK_ERROR_UNKNOWN;
    *out = (VkSubpassDescription){
        .flags = s->flags, .pipelineBindPoint = s->pipelineBindPoint,
        .inputAttachmentCount = s->inputAttachmentCount,
        .colorAttachmentCount = s->colorAttachmentCount,
        .preserveAttachmentCount = s->preserveAttachmentCount,
    };
    for (uint32_t i = 0; i < s->colorAttachmentCount; ++i)
        if ((rc = reference2(&s->pColorAttachments[i], &refs->color[i], NULL)) != VK_SUCCESS)
            return rc;
    if (s->colorAttachmentCount) out->pColorAttachments = refs->color;
    if (s->pResolveAttachments) {
        for (uint32_t i = 0; i < s->colorAttachmentCount; ++i)
            if ((rc = reference2(&s->pResolveAttachments[i], &refs->resolve[i], NULL)) != VK_SUCCESS)
                return rc;
        out->pResolveAttachments = refs->resolve;
    }
    for (uint32_t i = 0; i < s->inputAttachmentCount; ++i) {
        const VkAttachmentReference2 *r = &s->pInputAttachments[i];
        if ((rc = reference2(r, &refs->input[i], NULL)) != VK_SUCCESS) return rc;
        /* The aspect mask is meaningful only for a real input reference, and
         * is checked against the format of the attachment it names. */
        if (r->attachment == VK_ATTACHMENT_UNUSED) continue;
        if (r->attachment >= info->attachmentCount) return VK_ERROR_UNKNOWN;
        rc = ps5vk_render_pass_input_aspect_valid(
            info->pAttachments[r->attachment].format, r->aspectMask);
        if (rc != VK_SUCCESS) return rc;
    }
    if (s->inputAttachmentCount) out->pInputAttachments = refs->input;
    if (s->pDepthStencilAttachment) {
        if ((rc = reference2(s->pDepthStencilAttachment, &refs->depth, depth_stencil)) !=
            VK_SUCCESS)
            return rc;
        out->pDepthStencilAttachment = &refs->depth;
    }
    if (s->preserveAttachmentCount) {
        memcpy(refs->preserve, s->pPreserveAttachments,
               (size_t)s->preserveAttachmentCount * sizeof(refs->preserve[0]));
        out->pPreserveAttachments = refs->preserve;
    }
    return VK_SUCCESS;
}

VkResult ps5vk_render_pass2_translate(const VkRenderPassCreateInfo2 *info,
    struct ps5vk_render_pass2_translation *out)
{
    if (!info || !out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    if (info->sType != VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2) return VK_ERROR_UNKNOWN;
    /* No structure extends VkRenderPassCreateInfo2 on this device. */
    if (info->pNext) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!info->subpassCount || info->subpassCount > PS5VK_MAX_SUBPASSES || !info->pSubpasses ||
        !info->attachmentCount || info->attachmentCount > PS5VK_MAX_ATTACHMENTS ||
        !info->pAttachments ||
        info->dependencyCount > PS5VK_MAX_DEPENDENCIES ||
        (info->dependencyCount && !info->pDependencies) ||
        info->correlatedViewMaskCount > PS5VK_MAX_CORRELATION_MASKS ||
        (info->correlatedViewMaskCount && !info->pCorrelatedViewMasks))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult rc;
    const VkAttachmentDescriptionStencilLayout *attachment_stencil[PS5VK_MAX_ATTACHMENTS];
    const VkAttachmentReferenceStencilLayout *reference_stencil[PS5VK_MAX_SUBPASSES];
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        const VkAttachmentDescription2 *a = &info->pAttachments[i];
        if (a->sType != VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2) return VK_ERROR_UNKNOWN;
        if ((rc = attachment2_next(a, &attachment_stencil[i])) != VK_SUCCESS) return rc;
        out->stencil_layouts |= attachment_stencil[i] != NULL;
        out->attachments[i] = (VkAttachmentDescription){
            a->flags, a->format, a->samples, a->loadOp, a->storeOp,
            a->stencilLoadOp, a->stencilStoreOp, a->initialLayout, a->finalLayout};
    }
    /* Whether the pass says anything multiview at all. Only then is the
     * version-1 multiview structure chained, so a version-2 pass without views
     * becomes exactly the object its version-1 twin becomes. */
    VkBool32 views = info->correlatedViewMaskCount != 0;
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        if ((rc = subpass2(info, i, &out->refs[i], &out->subpasses[i],
                           &reference_stencil[i])) != VK_SUCCESS)
            return rc;
        out->stencil_layouts |= reference_stencil[i] != NULL;
        out->view_masks[i] = info->pSubpasses[i].viewMask;
        views |= out->view_masks[i] != 0;
    }
    for (uint32_t i = 0; i < info->dependencyCount; ++i) {
        const VkSubpassDependency2 *dep = &info->pDependencies[i];
        if (dep->sType != VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2) return VK_ERROR_UNKNOWN;
        if ((rc = dependency2_next(dep)) != VK_SUCCESS) return rc;
        out->dependencies[i] = (VkSubpassDependency){
            dep->srcSubpass, dep->dstSubpass, dep->srcStageMask, dep->dstStageMask,
            dep->srcAccessMask, dep->dstAccessMask, dep->dependencyFlags};
        out->view_offsets[i] = dep->viewOffset;
        views |= dep->viewOffset != 0 ||
            (dep->dependencyFlags & VK_DEPENDENCY_VIEW_LOCAL_BIT) != 0;
    }
    /* The stencil table, only when something separate was chained: the
     * chained layouts where present and the stencil projection of the
     * combined layout otherwise. A combined layout with no stencil meaning
     * (DEPTH_* without a chained stencil layout) cannot be split, which is
     * VUID-VkAttachmentDescription2-format-06906/06907 and
     * VUID-VkAttachmentReference2-attachment-06910. */
    if (out->stencil_layouts) {
        for (uint32_t i = 0; i < info->attachmentCount; ++i) {
            const VkAttachmentDescription2 *a = &info->pAttachments[i];
            const VkAttachmentDescriptionStencilLayout *chained = attachment_stencil[i];
            if (chained) {
                out->stencil.initial[i] = chained->stencilInitialLayout;
                out->stencil.final[i] = chained->stencilFinalLayout;
            } else if (ps5vk_format_is_combined_depth_stencil(a->format)) {
                if (!ps5vk_layout_for_aspect(a->initialLayout, VK_IMAGE_ASPECT_STENCIL_BIT,
                                             &out->stencil.initial[i]) ||
                    !ps5vk_layout_for_aspect(a->finalLayout, VK_IMAGE_ASPECT_STENCIL_BIT,
                                             &out->stencil.final[i]))
                    return VK_ERROR_UNKNOWN;
            } else {
                out->stencil.initial[i] = a->initialLayout;
                out->stencil.final[i] = a->finalLayout;
            }
        }
        for (uint32_t i = 0; i < info->subpassCount; ++i) {
            const VkAttachmentReference2 *r = info->pSubpasses[i].pDepthStencilAttachment;
            if (reference_stencil[i]) {
                out->stencil.reference[i] = reference_stencil[i]->stencilLayout;
            } else if (r && r->attachment < info->attachmentCount &&
                       ps5vk_format_is_combined_depth_stencil(
                           info->pAttachments[r->attachment].format)) {
                if (!ps5vk_layout_for_aspect(r->layout, VK_IMAGE_ASPECT_STENCIL_BIT,
                                             &out->stencil.reference[i]))
                    return VK_ERROR_UNKNOWN;
            } else {
                out->stencil.reference[i] = r ? r->layout : VK_IMAGE_LAYOUT_UNDEFINED;
            }
        }
    }
    if (info->correlatedViewMaskCount)
        memcpy(out->correlation_masks, info->pCorrelatedViewMasks,
               (size_t)info->correlatedViewMaskCount * sizeof(out->correlation_masks[0]));
    out->info = (VkRenderPassCreateInfo){
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .flags = info->flags,
        .attachmentCount = info->attachmentCount, .pAttachments = out->attachments,
        .subpassCount = info->subpassCount, .pSubpasses = out->subpasses,
        .dependencyCount = info->dependencyCount,
        .pDependencies = info->dependencyCount ? out->dependencies : NULL,
    };
    if (views) {
        /* VUID-VkRenderPassCreateInfo2-viewMask-03057/03058/03059,
         * VUID-VkSubpassDependency2-dependencyFlags-03092 and -viewOffset-02530,
         * VUID-VkRenderPassCreateInfo2-pCorrelatedViewMasks-03056 and
         * VUID-VkSubpassDescription2-multiview-06558/viewMask-06706 are the
         * version-1 multiview obligations restated, and the version-1
         * validator enforces them on this structure. */
        out->multiview = (VkRenderPassMultiviewCreateInfo){
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
            .subpassCount = info->subpassCount, .pViewMasks = out->view_masks,
            .dependencyCount = info->dependencyCount,
            .pViewOffsets = info->dependencyCount ? out->view_offsets : NULL,
            .correlationMaskCount = info->correlatedViewMaskCount,
            .pCorrelationMasks = info->correlatedViewMaskCount ? out->correlation_masks : NULL,
        };
        out->info.pNext = &out->multiview;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass2KHR(VkDevice d,
    const VkRenderPassCreateInfo2 *info, const VkAllocationCallbacks *allocator, VkRenderPass *out)
{
    PASS_MARK("PS5VK_RENDER_PASS2_CREATE attachments=%u subpasses=%u dependencies=%u",
        info ? info->attachmentCount : 0u, info ? info->subpassCount : 0u,
        info ? info->dependencyCount : 0u);
    if (!out) return VK_ERROR_UNKNOWN;
    *out = VK_NULL_HANDLE;
    if (!d || !info) return VK_ERROR_UNKNOWN;
    if (!d->create_renderpass2_extension_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_render_pass2_translation translation;
    VkResult rc = ps5vk_render_pass2_translate(info, &translation);
    if (rc != VK_SUCCESS) return rc;
    /* The stencil-layout structures belong to
     * VK_KHR_separate_depth_stencil_layouts: without the negotiated feature
     * they are refused, never ignored. */
    if (translation.stencil_layouts &&
        !(d->enabled_features_t09 & PS5VK_T09_FEATURE_SEPARATE_DEPTH_STENCIL_LAYOUTS))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    return ps5vk_render_pass_create(d, &translation.info,
        translation.stencil_layouts ? &translation.stencil : NULL, allocator, out);
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
    /* Zero counts specify implicit zero masks/offsets for the whole pass.
     * Normalize to the pass counts so view-local validation also runs when
     * pViewOffsets is omitted, as in the upstream multiview constructor. */
    const uint32_t subpass_count = info->subpassCount;
    const uint32_t dependency_count = info->dependencyCount;
    for (uint32_t i = 0; i < multiview->subpassCount; ++i) view_masks[i] = multiview->pViewMasks[i];
    for (uint32_t i = 0; i < multiview->dependencyCount; ++i) view_offsets[i] = multiview->pViewOffsets[i];
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
