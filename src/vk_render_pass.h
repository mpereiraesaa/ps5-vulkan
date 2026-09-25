#ifndef PS5VK_RENDER_PASS_H
#define PS5VK_RENDER_PASS_H
#include "vk_internal.h"
#include "color_attachment_contract.h"

/* Bounded multiple-subpass profile. The shape is owned data - never retained
 * create-info pointers - and a graphics-capable backend must be enabled before
 * these objects are usable.
 *
 * The limits below are this profile's, not Vulkan's, and they are enforced at
 * creation so an unsupported shape is refused where the caller can see it
 * rather than accepted and failed later. */
enum { PS5VK_MAX_SUBPASSES = 8 };
/* One attachment per role a pass may name, which is not the same thing as one
 * per colour target a caller may draw into. The pinned multisample oracle's
 * pass carries the multisampled colour attachment, its single-sample resolve
 * target and one single-sample target per sample it fetches back
 * (external/vulkancts/modules/vulkan/pipeline/
 * vktPipelineMultisampleTests.cpp, MSCaseBaseResolveAndPerSampleFetch), plus
 * the optional depth attachment at the highest count this profile serves.
 * PS5VK_MAX_COLOR_ATTACHMENTS stays the answer to "how many targets may a draw
 * write", and the colour contract enforces it per subpass; a pass that named
 * more colour references than that is refused there, not here. */
enum { PS5VK_MAX_ATTACHMENTS = PS5VK_SAMPLE_COUNT_MAX_SERVED + 3 };
enum { PS5VK_MAX_DEPENDENCIES = 16 };
enum { PS5VK_MAX_CORRELATION_MASKS = 4 };
enum { PS5VK_MAX_INPUT_ATTACHMENTS = 4 };

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
 * (00841), and a chained or duplicated structure stays fail-closed. The helper
 * also enforces the structure's implicit sType obligation
 * (VUID-VkRenderPassMultiviewCreateInfo-sType-sType) and bounds the pass counts
 * and dependency list it is handed, so a direct caller cannot overrun the
 * fixed-size arrays this model owns or dereference a missing pDependencies. */
VkResult ps5vk_render_pass_multiview_validate(const VkRenderPassCreateInfo *info,
    const VkRenderPassMultiviewCreateInfo *multiview, VkBool32 multiview_enabled,
    uint32_t max_multiview_view_count, struct ps5vk_render_pass_multiview *out);

/* VK_KHR_create_renderpass2: a VkRenderPassCreateInfo2 translated into the
 * version-1 description vkCreateRenderPass consumes, with every array it
 * points at owned by this structure. The version-2 obligations that have no
 * version-1 equivalent - structure types, the extension chains and the input
 * aspect masks - are checked by the translation; everything else is left to
 * the version-1 path, so both entry points share one model and one set of
 * profile rules. `info.pNext` names `multiview` only when the pass declares a
 * view mask, a view offset, a view-local dependency or a correlated mask. */
/* One translated subpass's references. A preserve list names distinct
 * attachments of the pass, so the pass's attachment bound bounds it. */
struct ps5vk_render_pass2_refs {
    VkAttachmentReference color[PS5VK_MAX_COLOR_ATTACHMENTS];
    VkAttachmentReference resolve[PS5VK_MAX_COLOR_ATTACHMENTS];
    VkAttachmentReference input[PS5VK_MAX_INPUT_ATTACHMENTS];
    VkAttachmentReference depth;
    uint32_t preserve[PS5VK_MAX_ATTACHMENTS];
};
/* The STENCIL aspect's layouts of a pass (VK_KHR_separate_depth_stencil_layouts).
 * A render pass created through vkCreateRenderPass uses one layout for both
 * aspects of a combined depth/stencil attachment, so this table repeats the
 * attachment's own initial/final layouts and the depth reference's layout.
 * A version-2 pass that chains VkAttachmentDescriptionStencilLayout or
 * VkAttachmentReferenceStencilLayout supplies them explicitly through
 * ps5vk_render_pass_create. The table is owned by value: no caller pointer is
 * retained. Entries for attachments without a stencil aspect are unused. */
struct ps5vk_render_pass_stencil_layouts {
    VkImageLayout initial[PS5VK_MAX_ATTACHMENTS];
    VkImageLayout final[PS5VK_MAX_ATTACHMENTS];
    /* Stencil layout of each subpass's depth/stencil reference. */
    VkImageLayout reference[PS5VK_MAX_SUBPASSES];
};

struct ps5vk_render_pass2_translation {
    VkRenderPassCreateInfo info;
    VkRenderPassMultiviewCreateInfo multiview;
    VkAttachmentDescription attachments[PS5VK_MAX_ATTACHMENTS];
    VkSubpassDescription subpasses[PS5VK_MAX_SUBPASSES];
    struct ps5vk_render_pass2_refs refs[PS5VK_MAX_SUBPASSES];
    VkSubpassDependency dependencies[PS5VK_MAX_DEPENDENCIES];
    uint32_t view_masks[PS5VK_MAX_SUBPASSES];
    int32_t view_offsets[PS5VK_MAX_DEPENDENCIES];
    uint32_t correlation_masks[PS5VK_MAX_CORRELATION_MASKS];
    /* VK_KHR_separate_depth_stencil_layouts: set when any attachment chains
     * VkAttachmentDescriptionStencilLayout or the depth/stencil reference of
     * any subpass chains VkAttachmentReferenceStencilLayout. The table then
     * holds every combined attachment's stencil layouts: the chained ones, or
     * the stencil projection of the combined layout where nothing is
     * chained. */
    VkBool32 stencil_layouts;
    struct ps5vk_render_pass_stencil_layouts stencil;
};
VkResult ps5vk_render_pass2_translate(const VkRenderPassCreateInfo2 *info,
    struct ps5vk_render_pass2_translation *out);

/* The aspect mask an input reference declares, against the format of the
 * attachment it names: never empty, never METADATA or a memory plane, only
 * aspects the format has (VK_ERROR_UNKNOWN otherwise), and every aspect the
 * format has, because the owned input reference reads them all
 * (VK_ERROR_FEATURE_NOT_PRESENT for a strict subset). Shared by the version-2
 * reference and VkRenderPassInputAttachmentAspectCreateInfo. */
VkResult ps5vk_render_pass_input_aspect_valid(VkFormat format, VkImageAspectFlags aspect);


/* One subpass: the roles this profile executes.
 *
 * There is no preserve list here, and that is a statement about the profile
 * rather than an omission. Every subpass names the SAME colour and depth
 * attachments - the only shape the framebuffer model can serve - and every
 * attachment must be named by a subpass, so every attachment is used by every
 * subpass and no attachment can be preserved-but-unused. A non-empty
 * pPreserveAttachments therefore has no legal form here and is refused at
 * creation rather than stored where it could never mean anything. */
/* One subpass: the roles this profile executes plus the input references it
 * owns.
 *
 * The input references are parsed, validated and then copied into the pass's
 * own allocation, exactly like the attachment descriptions: no create-info
 * pointer survives the call. They are stored because a subpass that reads an
 * earlier attachment is a real structure this model has to describe honestly.
 * One measured profile of them is now consumed: native execution admits a
 * single input reference at index 0 of a later subpass when it names the
 * promoted attachment, is read through that attachment's own framebuffer view
 * at the resource-only record width, and is ordered by the boundary transition
 * the executor emits for the reading subpass (see
 * native/input_attachment_gate.c). The reference and the descriptor may name
 * either read layout this profile admits - GENERAL, which the multiview witness
 * declares, or SHADER_READ_ONLY_OPTIMAL, which the pinned multisample oracle
 * declares for the colour attachment its fetch subpasses read - and the queue
 * carries the attachment through that declared layout at the boundary rather
 * than assuming GENERAL. Every broader shape - a second reference, another
 * index, another layout, another subpass pair - stays stored-only and fails
 * closed, and no shader or reporting surface claims more than that one measured
 * read. */
struct ps5vk_subpass {
    /* The subpass's colour references, in attachment order. The count is the
     * subpass's own, bounded by the colour-attachment contract; every consumer
     * reads color[0] while that bound is one, and the shape is what the second
     * target will need. */
    VkAttachmentReference color[PS5VK_MAX_COLOR_ATTACHMENTS];
    uint32_t color_count;
    VkAttachmentReference depth;
    /* The resolve target of each colour reference, in the same order, or
     * VK_ATTACHMENT_UNUSED when the subpass declares none. A resolve target is
     * a role of its own: the subpass does not render into it, it receives the
     * sample-resolved result of the colour attachment it follows, and Vulkan
     * requires it to be a single-sample attachment of that colour attachment's
     * format (DXVK262-T06).
     *
     * resolve_count is the number of entries DECLARED: zero when the subpass
     * supplied no pResolveAttachments, and the colour count when it did (Vulkan
     * requires those counts to match). Declaring is therefore what the count
     * says, and a zero-initialized subpass means "no resolve target" instead of
     * silently naming attachment 0. */
    uint32_t resolve_count;
    VkAttachmentReference resolve[PS5VK_MAX_COLOR_ATTACHMENTS];
    /* Where this subpass's input references start in the pass's one owned
     * array, and how many of them there are. An index rather than a pointer
     * keeps every element in the object's single allocation at 32-bit
     * alignment, which is the rule the suballocation below depends on. */
    uint32_t input_first, input_count;
    /* Where this subpass's preserve list starts in the pass's one owned array,
     * and how many entries it has. A preserved attachment is not rendered into
     * by the subpass and must come out of it unchanged, which is how the pinned
     * multisample family keeps the resolve target and its per-sample targets
     * alive across the fetch subpasses (DXVK262-T06). */
    uint32_t preserve_first, preserve_count;
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
    /* Every subpass's input references, concatenated in subpass order, in the
     * same single allocation. Never a create-info pointer. */
    VkAttachmentReference *inputs;
    uint32_t input_count;
    /* Every subpass's preserve list, concatenated in subpass order, in the same
     * single allocation. Never a create-info pointer. */
    uint32_t *preserves;
    uint32_t preserve_count;
    struct ps5vk_render_pass_multiview multiview;
    /* Per-aspect stencil layouts; always filled (see above). */
    struct ps5vk_render_pass_stencil_layouts stencil;
};

/* vkCreateRenderPass with an optional explicit stencil-layout table. NULL is
 * the version-1 meaning: every stencil layout equals the combined layout. A
 * table is only meaningful for attachments whose format has a stencil aspect;
 * each entry must name a layout that means something for the stencil aspect
 * (STENCIL_* or an aspect-neutral layout, never a combined or DEPTH_* one),
 * and a final layout is never UNDEFINED. With a table present, the depth
 * layouts of a combined attachment may be the separate DEPTH_* ones, since
 * the stencil half no longer rides on them. */
VkResult ps5vk_render_pass_create(VkDevice d, const VkRenderPassCreateInfo *info,
    const struct ps5vk_render_pass_stencil_layouts *stencil,
    const VkAllocationCallbacks *allocator, VkRenderPass *out);

/* The per-aspect layouts of a depth/stencil attachment in one subpass. The
 * depth half is the attachment description's and the depth reference's; the
 * stencil half comes from the pass's stencil table. */
static inline int ps5vk_render_pass_depth_stencil_layouts(VkRenderPass pass,
    uint32_t subpass, uint32_t attachment,
    VkImageLayout *initial_depth, VkImageLayout *initial_stencil,
    VkImageLayout *reference_depth, VkImageLayout *reference_stencil,
    VkImageLayout *final_depth, VkImageLayout *final_stencil)
{
    if (!pass || subpass >= pass->subpass_count || attachment >= pass->attachment_count ||
        pass->subpasses[subpass].depth.attachment != attachment) return 0;
    *initial_depth = pass->attachments[attachment].initialLayout;
    *final_depth = pass->attachments[attachment].finalLayout;
    *reference_depth = pass->subpasses[subpass].depth.layout;
    *initial_stencil = pass->stencil.initial[attachment];
    *final_stencil = pass->stencil.final[attachment];
    *reference_stencil = pass->stencil.reference[subpass];
    return 1;
}

/* The references of one subpass. Callers that only handle the single-subpass
 * profile pass 0 and say so, rather than reaching for fields that no longer
 * describe the whole pass. Every subpass of a pass names the same attachments,
 * so a caller that needs only the ROLES may read subpass 0 for any of them. */
static inline const struct ps5vk_subpass *ps5vk_render_pass_subpass(
    VkRenderPass pass, uint32_t index)
{
    return &pass->subpasses[index];
}

/* The owned input references of one subpass. The bounded native gate above
 * consumes one of them per pass; the rest of the model still stores them only
 * so the pass it accepted is described honestly. */
static inline const VkAttachmentReference *ps5vk_render_pass_inputs(
    VkRenderPass pass, uint32_t index)
{
    return &pass->inputs[pass->subpasses[index].input_first];
}

/* The owned preserve list of one subpass, in the order the caller declared it. */
static inline const uint32_t *ps5vk_render_pass_preserves(
    VkRenderPass pass, uint32_t index)
{
    return &pass->preserves[pass->subpasses[index].preserve_first];
}

/* True when any subpass of this pass preserves an attachment. The object model
 * stores and validates the list; the native queue does not carry attachment
 * contents across a subpass boundary implicitly, so its executor refuses a pass
 * this returns true for rather than silently dropping the promise. */
static inline int ps5vk_render_pass_has_preserve_list(VkRenderPass pass)
{
    if (!pass) return 0;
    for (uint32_t i = 0; i < pass->subpass_count; ++i)
        if (pass->subpasses[i].preserve_count) return 1;
    return 0;
}

/* True when any colour reference of this subpass resolves into another
 * attachment. The object model accepts the shape - the render pass,
 * framebuffer and pipeline frontends all describe it - while the native path
 * cannot resolve yet, so its executor refuses a pass this returns true for.
 * One helper decides it so the executor cannot drift from the model. */
static inline int ps5vk_subpass_uses_resolve(const struct ps5vk_subpass *subpass)
{
    if (!subpass) return 0;
    for (uint32_t c = 0; c < subpass->resolve_count; ++c)
        if (subpass->resolve[c].attachment != VK_ATTACHMENT_UNUSED) return 1;
    return 0;
}

/* The layout one subpass NAMES for one attachment, and whether it names it at
 * all. A pass is therefore a sequence of layouts per attachment: the
 * attachment's initial layout, then what each subpass declares as the pass
 * reaches it (colour, then resolve, then an input read - the order a subpass
 * uses them in), then the attachment's final layout. The native executor walks
 * that sequence, because a render pass is what carries an attachment from one
 * layout to the next at a subpass boundary: the pinned multisample oracle
 * renders its multisampled colour attachment as colour in subpass 0 and reads
 * it as an input attachment in SHADER_READ_ONLY_OPTIMAL in the fetch subpasses,
 * and the identity of that read layout is exactly what the input-attachment
 * gate checks the recorded descriptor against.
 *
 * A subpass that names the attachment only in its preserve list names no
 * layout, and that is a real answer: a preserved attachment is neither read nor
 * written by the subpass, so it carries its layout through the subpass
 * unchanged. */
static inline int ps5vk_render_pass_attachment_layout(VkRenderPass pass,
    uint32_t subpass_index, uint32_t attachment, VkImageLayout *out)
{
    if (!pass || !out || subpass_index >= pass->subpass_count ||
        attachment == VK_ATTACHMENT_UNUSED) return 0;
    const struct ps5vk_subpass *subpass = &pass->subpasses[subpass_index];
    for (uint32_t c = 0; c < subpass->color_count; ++c)
        if (subpass->color[c].attachment == attachment) {
            *out = subpass->color[c].layout;
            return 1;
        }
    for (uint32_t r = 0; r < subpass->resolve_count; ++r)
        if (subpass->resolve[r].attachment == attachment) {
            *out = subpass->resolve[r].layout;
            return 1;
        }
    for (uint32_t i = 0; i < subpass->input_count; ++i)
        if (pass->inputs[subpass->input_first + i].attachment == attachment) {
            *out = pass->inputs[subpass->input_first + i].layout;
            return 1;
        }
    if (subpass->depth.attachment == attachment) {
        *out = subpass->depth.layout;
        return 1;
    }
    return 0;
}
#endif
