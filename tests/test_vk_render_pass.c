#include "vk_render_pass.h"
#include "attachment_ops.h"
#include "vk_framebuffer.h"
#include <assert.h>
#include <stdio.h>
/* DXVK262-T02 slice B: VkRenderPassMultiviewCreateInfo is parsed, validated
 * and owned by the pass. Correlation masks are validated hints and are never
 * executed; this profile advertises no multiview feature, so every view mask
 * the device accepts must be zero, and the full rule set is exercised through
 * the pure validator so the slice that enables the feature inherits it. */
static void multiview_model(struct VkDevice_T *d)
{
    VkAttachmentDescription attachments[2] = {
        {.format=VK_FORMAT_B8G8R8A8_UNORM, .samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.format=VK_FORMAT_D32_SFLOAT, .samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpasses[2] = {
        {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount=1, .pColorAttachments=&color,
         .pDepthStencilAttachment=&depth},
        {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount=1, .pColorAttachments=&color,
         .pDepthStencilAttachment=&depth}};
    VkSubpassDependency dependencies[2] = {
        {.srcSubpass=0, .dstSubpass=1,
         .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         .dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
         .dependencyFlags=VK_DEPENDENCY_VIEW_LOCAL_BIT},
        {.srcSubpass=0, .dstSubpass=1,
         .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         .dstAccessMask=VK_ACCESS_SHADER_READ_BIT}};
    VkRenderPassCreateInfo info = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=2, .pAttachments=attachments,
        .subpassCount=2, .pSubpasses=subpasses,
        .dependencyCount=2, .pDependencies=dependencies};
    const uint32_t masks_enabled[2] = {0x3u, 0x1u};
    const int32_t offsets_view_local[2] = {1, 0};
    const uint32_t correlations[2] = {0x1u, 0x2u};
    VkRenderPassMultiviewCreateInfo multiview = {
        .sType=VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
        .subpassCount=2, .pViewMasks=masks_enabled,
        .dependencyCount=2, .pViewOffsets=offsets_view_local};
    struct ps5vk_render_pass_multiview owned;

    /* Positive with the feature enabled: two non-zero masks under the reported
     * limit, a view-local dependency with a non-zero offset between two
     * different subpasses, and disjoint correlation masks that are stored as
     * hints. */
    assert(ps5vk_render_pass_multiview_validate(&info, &multiview, VK_TRUE, 6u,
        &owned) == VK_SUCCESS);
    assert(owned.present && owned.subpass_count == 2 && owned.dependency_count == 2);
    assert(owned.view_masks[0] == 0x3u && owned.view_masks[1] == 0x1u);
    assert(owned.view_offsets[0] == 1 && owned.view_offsets[1] == 0);
    assert(owned.correlation_mask_count == 0);
    multiview.correlationMaskCount = 2; multiview.pCorrelationMasks = correlations;
    assert(ps5vk_render_pass_multiview_validate(&info, &multiview, VK_TRUE, 6u,
        &owned) == VK_SUCCESS);
    assert(owned.correlation_mask_count == 2 && owned.correlation_masks[0] == 0x1u &&
           owned.correlation_masks[1] == 0x2u);
    /* The masks are the pass's structural data, not a promise that anything
     * executes them: the stored correlation masks are the only trace and no
     * execution field exists for them. */
    assert(!owned.view_masks[0] || owned.subpass_count == 2);

    /* 06697: the most significant set bit must be below maxMultiviewViewCount. */
    const uint32_t masks_too_wide[2] = {0x40u, 0x40u};
    VkRenderPassMultiviewCreateInfo bad = multiview;
    bad.pViewMasks = masks_too_wide;
    assert(ps5vk_render_pass_multiview_validate(&info, &bad, VK_TRUE, 6u, &owned) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
    /* 02513: all view masks are zero or all are non-zero. */
    const uint32_t masks_mixed[2] = {0x0u, 0x1u};
    bad = multiview; bad.pViewMasks = masks_mixed;
    assert(ps5vk_render_pass_multiview_validate(&info, &bad, VK_TRUE, 6u, &owned) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
    /* 06555: without the feature every view mask must be zero. */
    assert(ps5vk_render_pass_multiview_validate(&info, &multiview, VK_FALSE, 0u,
        &owned) == VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
    /* 01928 and 01929: counts must match the pass being created. */
    bad = multiview; bad.subpassCount = 1;
    assert(ps5vk_render_pass_multiview_validate(&info, &bad, VK_TRUE, 6u, &owned) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
    bad = multiview; bad.dependencyCount = 1;
    assert(ps5vk_render_pass_multiview_validate(&info, &bad, VK_TRUE, 6u, &owned) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
    /* A required array that is missing, and a chained structure. */
    bad = multiview; bad.pViewMasks = NULL;
    assert(ps5vk_render_pass_multiview_validate(&info, &bad, VK_TRUE, 6u, &owned) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
    bad = multiview; bad.pNext = (const void *)&multiview;
    assert(ps5vk_render_pass_multiview_validate(&info, &bad, VK_TRUE, 6u, &owned) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
    /* 01930: a non-zero offset needs two different subpasses; 02512: an offset
     * is meaningless without a view-local dependency. */
    VkSubpassDependency self_dependency = dependencies[0];
    self_dependency.dstSubpass = self_dependency.srcSubpass;
    VkSubpassDependency saved = dependencies[0];
    dependencies[0] = self_dependency;
    assert(ps5vk_render_pass_multiview_validate(&info, &multiview, VK_TRUE, 6u,
        &owned) == VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
    dependencies[0] = saved;
    dependencies[0].dependencyFlags = 0;
    assert(ps5vk_render_pass_multiview_validate(&info, &multiview, VK_TRUE, 6u,
        &owned) == VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
    dependencies[0] = saved;
    /* 02514: with every mask zero no dependency may be view-local, and 02515:
     * correlation masks cannot be declared either. */
    const uint32_t masks_zero[2] = {0u, 0u};
    const int32_t offsets_zero[2] = {0, 0};
    VkRenderPassMultiviewCreateInfo zero = {
        .sType=VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
        .subpassCount=2, .pViewMasks=masks_zero,
        .dependencyCount=2, .pViewOffsets=offsets_zero};
    assert(ps5vk_render_pass_multiview_validate(&info, &zero, VK_FALSE, 0u, &owned) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
    dependencies[0].dependencyFlags = 0;
    zero.correlationMaskCount = 1; zero.pCorrelationMasks = correlations;
    assert(ps5vk_render_pass_multiview_validate(&info, &zero, VK_FALSE, 0u, &owned) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
    zero.correlationMaskCount = 0; zero.pCorrelationMasks = NULL;
    /* 00841: a view index may appear in at most one correlation mask, and the
     * profile bounds how many masks it is willing to own. */
    const uint32_t overlapping[2] = {0x1u, 0x1u};
    bad = multiview; bad.pCorrelationMasks = overlapping;
    assert(ps5vk_render_pass_multiview_validate(&info, &bad, VK_TRUE, 6u, &owned) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
    bad = multiview; bad.correlationMaskCount = PS5VK_MAX_CORRELATION_MASKS + 1;
    assert(ps5vk_render_pass_multiview_validate(&info, &bad, VK_TRUE, 6u, &owned) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);

    /* The helper is exported and directly exercised, so it refuses its own
     * malformed inputs before it copies or dereferences anything: the implicit
     * sType obligation, pass counts the fixed-size arrays cannot hold, and a
     * non-zero dependency count with no dependency list. */
    bad = multiview; bad.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    assert(ps5vk_render_pass_multiview_validate(&info, &bad, VK_TRUE, 6u, &owned) ==
           VK_ERROR_UNKNOWN && !owned.present);
    bad = multiview; bad.sType = VK_STRUCTURE_TYPE_MAX_ENUM;
    assert(ps5vk_render_pass_multiview_validate(&info, &bad, VK_TRUE, 6u, &owned) ==
           VK_ERROR_UNKNOWN && !owned.present);
    {
        VkRenderPassCreateInfo oversized = info;
        oversized.subpassCount = PS5VK_MAX_SUBPASSES + 1;
        bad = multiview; bad.subpassCount = oversized.subpassCount;
        assert(ps5vk_render_pass_multiview_validate(&oversized, &bad, VK_TRUE, 6u,
            &owned) == VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
        oversized = info; oversized.dependencyCount = PS5VK_MAX_DEPENDENCIES + 1;
        bad = multiview; bad.dependencyCount = oversized.dependencyCount;
        assert(ps5vk_render_pass_multiview_validate(&oversized, &bad, VK_TRUE, 6u,
            &owned) == VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
        oversized = info; oversized.pDependencies = NULL;
        assert(ps5vk_render_pass_multiview_validate(&oversized, &multiview, VK_TRUE, 6u,
            &owned) == VK_ERROR_FEATURE_NOT_PRESENT && !owned.present);
    }

    /* Through the device: this profile reports no multiview feature and no view
     * count, so the only shape vkCreateRenderPass may accept is the all-zero
     * one - which is exactly "multiview disabled" - and the pass owns a copy. */
    uint32_t caller_masks[2] = {0u, 0u};
    int32_t caller_offsets[2] = {0, 0};
    VkRenderPassMultiviewCreateInfo caller = {
        .sType=VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
        .subpassCount=2, .pViewMasks=caller_masks,
        .dependencyCount=2, .pViewOffsets=caller_offsets};
    info.pNext = &caller;
    VkRenderPass pass = VK_NULL_HANDLE;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_SUCCESS);
    assert(pass->multiview.present && pass->multiview.subpass_count == 2 &&
           pass->multiview.dependency_count == 2 &&
           !pass->multiview.view_masks[0] && !pass->multiview.view_masks[1] &&
           !pass->multiview.correlation_mask_count);
    caller_masks[0] = 0x1u; caller_offsets[0] = 3;
    assert(!pass->multiview.view_masks[0] && !pass->multiview.view_offsets[0]);
    caller_masks[0] = 0u; caller_offsets[0] = 0;
    vkDestroyRenderPass(d, pass, NULL);
    /* A non-zero mask needs the feature this profile does not advertise. */
    caller_masks[0] = 0x1u;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT &&
           !pass);
    caller_masks[0] = 0u;
    /* Unknown or duplicated chained structures stay fail-closed. */
    VkBaseInStructure unknown = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM};
    caller.pNext = &unknown;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT &&
           !pass);
    caller.pNext = NULL;
    VkRenderPassMultiviewCreateInfo second = caller;
    caller.pNext = &second;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT &&
           !pass);
    info.pNext = NULL;
}

/* The bounded multiple-subpass profile: the object model, its owned arrays and
 * the exact shapes it refuses. Nothing here executes - submitting a pass with
 * more than one subpass stays fail-closed until the execution slice. */
/* A resolve reference is a role of its own (DXVK262-T06). The shape the pinned
 * multisample family builds - a multisampled colour target plus the
 * single-sample attachment that receives its resolved result - is accepted by
 * the object model; the native executor refuses to run it until the resolve
 * itself exists. Every malformed reference stays refused here. */
static void resolve_attachments(struct VkDevice_T *d)
{
    VkAttachmentDescription attachments[2] = {
        {.format=VK_FORMAT_B8G8R8A8_UNORM, .samples=VK_SAMPLE_COUNT_4_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.format=VK_FORMAT_B8G8R8A8_UNORM, .samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolve = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1, .pColorAttachments=&color, .pResolveAttachments=&resolve};
    VkRenderPassCreateInfo info = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=2, .pAttachments=attachments,
        .subpassCount=1, .pSubpasses=&subpass};
    VkRenderPass pass = VK_NULL_HANDLE;

    /* The counts only exist on a platform that serves them, exactly as for a
     * multisampled colour attachment without a resolve target. */
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT && !pass);
    d->platform_features |= PS5VK_FEATURE_SAMPLE_RATE_SHADING;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_SUCCESS && pass);
    /* Both roles are owned: the resolve reference sits beside the colour one
     * and is not the colour attachment itself. */
    assert(ps5vk_render_pass_subpass(pass, 0)->resolve[0].attachment == 1 &&
           ps5vk_render_pass_subpass(pass, 0)->color[0].attachment == 0 &&
           ps5vk_subpass_uses_resolve(ps5vk_render_pass_subpass(pass, 0)));

    /* Compatibility is checked per reference against the framebuffer's own
     * attachment slots, not through the role fields: a framebuffer whose slot 1
     * was created for a different format or sample count is not compatible with
     * this pass, whatever its role slots say (DXVK262-T06). */
    {
        struct VkFramebuffer_T framebuffer = {.device=d, .attachment_count=2,
            .formats={VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM},
            .samples={VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_1_BIT},
            .color_attachments={0}, .color_count=1,
            .resolve_count=1, .resolve_attachments={1},
            .depth_attachment=VK_ATTACHMENT_UNUSED};
        assert(ps5vk_framebuffer_compatible(&framebuffer, pass));
        /* The role rule is index-independent by design - a conformant
         * continuation may reach the same role through another slot - so what
         * makes a framebuffer incompatible is a role it does not carry, a slot
         * outside it, or a format/sample count that disagrees. */
        framebuffer.resolve_count = 0;
        assert(!ps5vk_framebuffer_compatible(&framebuffer, pass));
        framebuffer.resolve_count = 1;
        framebuffer.resolve_attachments[0] = VK_ATTACHMENT_UNUSED;
        assert(!ps5vk_framebuffer_compatible(&framebuffer, pass));
        framebuffer.resolve_attachments[0] = 1;
        framebuffer.attachment_count = 1;
        assert(!ps5vk_framebuffer_compatible(&framebuffer, pass));
        framebuffer.attachment_count = 2;
        framebuffer.samples[1] = VK_SAMPLE_COUNT_4_BIT;
        assert(!ps5vk_framebuffer_compatible(&framebuffer, pass));
        framebuffer.samples[1] = VK_SAMPLE_COUNT_1_BIT;
        framebuffer.formats[1] = VK_FORMAT_R8G8B8A8_UNORM;
        assert(!ps5vk_framebuffer_compatible(&framebuffer, pass));
    }
    vkDestroyRenderPass(d, pass, NULL);

    /* A resolve target is single-sample by definition. */
    attachments[1].samples = VK_SAMPLE_COUNT_4_BIT;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT && !pass);
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    /* Its format is the colour attachment's. */
    attachments[1].format = VK_FORMAT_R8G8B8A8_UNORM;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT && !pass);
    attachments[1].format = VK_FORMAT_B8G8R8A8_UNORM;
    /* A layout a colour target may not use. */
    resolve.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_UNKNOWN && !pass);
    resolve.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    /* One surface cannot be both what the subpass renders into and what
     * receives the resolved result. */
    resolve.attachment = 0;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_UNKNOWN && !pass);
    /* ... nor a depth reference. Within this profile's two-attachment bound
     * that means a pass cannot carry a resolve target and a depth role at the
     * same time, which is exactly the refusal below. */
    VkAttachmentReference depth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription with_depth = subpass;
    with_depth.pDepthStencilAttachment = &depth;
    info.pSubpasses = &with_depth;
    resolve.attachment = 1;
    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_UNKNOWN && !pass);
    attachments[1].format = VK_FORMAT_B8G8R8A8_UNORM;
    info.pSubpasses = &subpass;
    /* An attachment no reference names has no role, and a resolve reference to
     * one is out of range. */
    resolve.attachment = 2;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_UNKNOWN && !pass);
    resolve.attachment = 1;
    /* Each subpass declares its own resolve array (DXVK262-T06): one declaring
     * a resolve reference and the other declaring the array with an UNUSED
     * entry is a shape the model describes, and both are accepted. */
    VkSubpassDescription two[2] = {subpass, subpass};
    info.subpassCount = 2; info.pSubpasses = two;
    VkAttachmentReference none = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
    two[1].pResolveAttachments = &none;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_SUCCESS && pass);
    assert(ps5vk_render_pass_subpass(pass, 0)->resolve_count == 1 &&
           ps5vk_render_pass_subpass(pass, 0)->resolve[0].attachment == 1 &&
           ps5vk_render_pass_subpass(pass, 1)->resolve_count == 1 &&
           ps5vk_render_pass_subpass(pass, 1)->resolve[0].attachment == VK_ATTACHMENT_UNUSED);
    vkDestroyRenderPass(d, pass, NULL);
    two[1].pResolveAttachments = &resolve;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_SUCCESS && pass);
    vkDestroyRenderPass(d, pass, NULL);
    d->platform_features &= ~(uint32_t)PS5VK_FEATURE_SAMPLE_RATE_SHADING;
}

static void multiple_subpasses(struct VkDevice_T *d)
{
    VkAttachmentDescription attachments[2] = {
        {.format=VK_FORMAT_B8G8R8A8_UNORM, .samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {.format=VK_FORMAT_D32_SFLOAT, .samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpasses[2] = {
        {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount=1, .pColorAttachments=&color,
         .pDepthStencilAttachment=&depth},
        {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount=1, .pColorAttachments=&color,
         .pDepthStencilAttachment=&depth}};
    VkSubpassDependency between = {.srcSubpass=0, .dstSubpass=1,
        .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
    VkRenderPassCreateInfo info = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=2, .pAttachments=attachments,
        .subpassCount=2, .pSubpasses=subpasses,
        .dependencyCount=1, .pDependencies=&between};
    VkRenderPass pass = VK_NULL_HANDLE;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_SUCCESS);
    assert(pass->subpass_count == 2 && pass->attachment_count == 2 &&
           pass->dependency_count == 1);
    /* Everything is OWNED: mutating the caller's structures afterwards cannot
     * change what the object recorded. */
    color.attachment = 1; depth.attachment = 0; between.srcSubpass = 1;
    attachments[0].format = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < 2; ++i) {
        const struct ps5vk_subpass *s = ps5vk_render_pass_subpass(pass, i);
        assert(!s->color[0].attachment && s->color_count == 1 && s->depth.attachment == 1);
    }
    assert(pass->attachments[0].format == VK_FORMAT_B8G8R8A8_UNORM &&
           !pass->dependencies[0].srcSubpass && pass->dependencies[0].dstSubpass == 1);
    vkDestroyRenderPass(d, pass, NULL);
    color.attachment = 0; depth.attachment = 1; between.srcSubpass = 0;
    attachments[0].format = VK_FORMAT_B8G8R8A8_UNORM;

    /* Refused shapes. Each leaves no object and no accounting behind. */
    const unsigned objects = d->graphics_objects;

    /* Each subpass may name its OWN attachments (DXVK262-T06): a framebuffer is
     * an array of views indexed by attachment, and a subpass may preserve what
     * it does not render into - which is how the pinned multisample family
     * keeps its per-sample targets alive. */
    {
        VkAttachmentDescription two[2] = {
            {.format=VK_FORMAT_B8G8R8A8_UNORM, .samples=VK_SAMPLE_COUNT_1_BIT,
             .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
             .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
             .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {.format=VK_FORMAT_B8G8R8A8_UNORM, .samples=VK_SAMPLE_COUNT_1_BIT,
             .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
             .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
             .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
        VkAttachmentReference first = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference second = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        uint32_t preserved = 1;
        VkSubpassDescription described[2] = {
            {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount=1,
             .pColorAttachments=&first, .preserveAttachmentCount=1,
             .pPreserveAttachments=&preserved},
            {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount=1,
             .pColorAttachments=&second}};
        VkRenderPassCreateInfo two_info = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount=2, .pAttachments=two, .subpassCount=2, .pSubpasses=described};
        VkRenderPass two_pass = VK_NULL_HANDLE;
        {
            VkResult rc = vkCreateRenderPass(d, &two_info, NULL, &two_pass);
            if (rc != VK_SUCCESS) fprintf(stderr, "DEBUG heterogeneous rc=%d\n", (int)rc);
            assert(rc == VK_SUCCESS && two_pass);
        }
        assert(ps5vk_render_pass_subpass(two_pass, 0)->color[0].attachment == 0 &&
               ps5vk_render_pass_subpass(two_pass, 1)->color[0].attachment == 1);
        assert(ps5vk_render_pass_subpass(two_pass, 0)->preserve_count == 1 &&
               ps5vk_render_pass_preserves(two_pass, 0)[0] == 1 &&
               ps5vk_render_pass_has_preserve_list(two_pass));
        /* Owned: mutating the caller's array afterwards changes nothing. */
        preserved = 0;
        assert(ps5vk_render_pass_preserves(two_pass, 0)[0] == 1);
        vkDestroyRenderPass(d, two_pass, NULL);
        /* An attachment a subpass renders into may not also be preserved by
         * that subpass, an entry may not be VK_ATTACHMENT_UNUSED or out of
         * range, and one attachment may not appear twice. */
        uint32_t used = 0;
        described[0].pPreserveAttachments = &used;
        assert(vkCreateRenderPass(d, &two_info, NULL, &two_pass) == VK_ERROR_UNKNOWN && !two_pass);
        uint32_t unused_entry = VK_ATTACHMENT_UNUSED;
        described[0].pPreserveAttachments = &unused_entry;
        assert(vkCreateRenderPass(d, &two_info, NULL, &two_pass) == VK_ERROR_UNKNOWN && !two_pass);
        uint32_t out_of_range = 2;
        described[0].pPreserveAttachments = &out_of_range;
        assert(vkCreateRenderPass(d, &two_info, NULL, &two_pass) == VK_ERROR_UNKNOWN && !two_pass);
        uint32_t twice[2] = {1, 1};
        described[0].pPreserveAttachments = twice;
        described[0].preserveAttachmentCount = 2;
        assert(vkCreateRenderPass(d, &two_info, NULL, &two_pass) == VK_ERROR_UNKNOWN && !two_pass);
        /* A count without an array is malformed, not empty. */
        described[0].pPreserveAttachments = NULL;
        assert(vkCreateRenderPass(d, &two_info, NULL, &two_pass) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !two_pass);
        described[0].preserveAttachmentCount = 0;
        /* An attachment no reference of any subpass names has no role. */
        VkAttachmentDescription unlisted[3] = {two[0], two[1], two[0]};
        VkRenderPassCreateInfo unlisted_info = two_info;
        unlisted_info.attachmentCount = 3; unlisted_info.pAttachments = unlisted;
        assert(vkCreateRenderPass(d, &unlisted_info, NULL, &two_pass) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !two_pass);
    }
    /* Vulkan IGNORES pInputAttachments when the count is zero, so a stale
     * pointer beside a zero count must NOT be refused. */
    VkAttachmentReference ignored = {0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    subpasses[0].pInputAttachments = &ignored;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_SUCCESS);
    vkDestroyRenderPass(d, pass, NULL);
    subpasses[0].pInputAttachments = NULL;

    /* Subpasses may name DIFFERENT attachments (DXVK262-T06). What is still
     * refused is a reference that does not fit the description it names: this
     * fixture's attachment 1 is D32, so a subpass that renders into it as a
     * colour attachment is refused by the format rule rather than by a
     * shared-role rule. */
    VkAttachmentReference other_color = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    subpasses[1].pColorAttachments = &other_color;
    subpasses[1].pDepthStencilAttachment = NULL;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    subpasses[1].pColorAttachments = &color;
    /* Dropping a role in one subpass is legal now. */
    subpasses[1].pDepthStencilAttachment = NULL;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_SUCCESS && pass);
    vkDestroyRenderPass(d, pass, NULL);
    subpasses[1].pDepthStencilAttachment = &depth;

    /* more subpasses than the profile executes */
    info.subpassCount = PS5VK_MAX_SUBPASSES + 1; info.pSubpasses = subpasses;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    info.subpassCount = 0;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    info.subpassCount = 2; info.pSubpasses = subpasses;

    /* an attachment no subpass references */
    VkAttachmentReference only_color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    subpasses[0].pDepthStencilAttachment = NULL;
    subpasses[1].pDepthStencilAttachment = NULL;
    subpasses[0].pColorAttachments = &only_color;
    subpasses[1].pColorAttachments = &only_color;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    subpasses[0].pDepthStencilAttachment = &depth;
    subpasses[1].pDepthStencilAttachment = &depth;
    subpasses[0].pColorAttachments = &color;
    subpasses[1].pColorAttachments = &color;

    /* swapping the two roles between subpasses is the same refusal */
    VkAttachmentReference swapped_color = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference swapped_depth = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    subpasses[1].pColorAttachments = &swapped_color;
    subpasses[1].pDepthStencilAttachment = &swapped_depth;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    subpasses[1].pColorAttachments = &color;
    subpasses[1].pDepthStencilAttachment = &depth;

    /* dependency endpoints: backward, self, out of range, and external to
     * external are all refused rather than stored and ignored */
    const VkSubpassDependency good = between;
    struct { uint32_t src, dst; } bad_edges[] = {
        {1, 0}, {0, 0}, {1, 1}, {0, 2}, {2, 1},
        {VK_SUBPASS_EXTERNAL, VK_SUBPASS_EXTERNAL}};
    for (unsigned i = 0; i < sizeof(bad_edges) / sizeof(bad_edges[0]); ++i) {
        between.srcSubpass = bad_edges[i].src; between.dstSubpass = bad_edges[i].dst;
        assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    }
    between = good;
    /* the forward edge and both external edges remain accepted */
    struct { uint32_t src, dst; } good_edges[] = {
        {0, 1}, {VK_SUBPASS_EXTERNAL, 0}, {1, VK_SUBPASS_EXTERNAL}};
    for (unsigned i = 0; i < sizeof(good_edges) / sizeof(good_edges[0]); ++i) {
        between.srcSubpass = good_edges[i].src; between.dstSubpass = good_edges[i].dst;
        assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_SUCCESS);
        vkDestroyRenderPass(d, pass, NULL);
    }
    between = good;
    assert(d->graphics_objects == objects);
}

/* The input-attachment object model: the references of every subpass are
 * validated against this profile and then copied into the pass's own
 * allocation, so nothing a caller does afterwards can change what the pass
 * describes. Nothing consumes them yet - that is the next slice - so these
 * regressions are about the owned representation and every fail-closed edge. */
static void input_attachments(struct VkDevice_T *d)
{
    VkAttachmentDescription attachments[2] = {
        {.format=VK_FORMAT_B8G8R8A8_UNORM, .samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.format=VK_FORMAT_R8G8B8A8_UNORM, .samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference inputs[2] = {
        {1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED}};
    VkSubpassDescription subpasses[2] = {
        {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount=1, .pColorAttachments=&color},
        {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
         .colorAttachmentCount=1, .pColorAttachments=&color,
         .inputAttachmentCount=2, .pInputAttachments=inputs}};
    VkRenderPassCreateInfo info = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=2, .pAttachments=attachments,
        .subpassCount=2, .pSubpasses=subpasses};
    VkRenderPass pass = VK_NULL_HANDLE;

    /* Attachment 1 has no colour or depth role at all: the input reference is
     * the only use that keeps it reachable, which is exactly the shape a later
     * subpass reading an earlier attachment has. */
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_SUCCESS);
    assert(pass->input_count == 2);
    assert(pass->subpasses[0].input_count == 0);
    assert(pass->subpasses[1].input_first == 0 && pass->subpasses[1].input_count == 2);
    const VkAttachmentReference *owned = ps5vk_render_pass_inputs(pass, 1);
    assert(owned[0].attachment == 1 && owned[0].layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    assert(owned[1].attachment == VK_ATTACHMENT_UNUSED);
    /* The owned copy is the pass's, not the caller's: mutating either the
     * reference array or the subpass description afterwards changes nothing. */
    inputs[0].layout = VK_IMAGE_LAYOUT_GENERAL; inputs[0].attachment = VK_ATTACHMENT_UNUSED;
    inputs[1].attachment = 0;
    subpasses[1].inputAttachmentCount = 0;
    assert(owned[0].attachment == 1 && owned[0].layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    assert(owned[1].attachment == VK_ATTACHMENT_UNUSED);
    assert(pass->subpasses[1].input_count == 2);
    subpasses[1].inputAttachmentCount = 2;
    inputs[0].attachment = 1; inputs[0].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    inputs[1].attachment = VK_ATTACHMENT_UNUSED;
    vkDestroyRenderPass(d, pass, NULL);

    /* Fail-closed edges. Every one of them leaves the output untouched and
     * creates no object. */
    unsigned before = d->graphics_objects;
    VkAttachmentReference bad = {2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    subpasses[1].pInputAttachments = &bad; subpasses[1].inputAttachmentCount = 1;
    pass = (VkRenderPass)(uintptr_t)1;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_UNKNOWN &&
           pass == VK_NULL_HANDLE && d->graphics_objects == before);
    bad.attachment = 1;
    /* A layout a subpass may not read through: Vulkan forbids these for an
     * input reference, and the profile refuses them rather than storing them. */
    const VkImageLayout illegal[] = {VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PREINITIALIZED,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL + 4096,
                                     /* VUID 06912 forbids both attachment layouts for a real
                                      * input reference: an input attachment is read, not
                                      * attached, so neither is a legal read layout. */
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    for (unsigned i = 0; i < sizeof(illegal)/sizeof(illegal[0]); ++i) {
        bad.layout = (VkImageLayout)illegal[i];
        assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_UNKNOWN &&
               pass == VK_NULL_HANDLE && d->graphics_objects == before);
    }
    /* A count without an array cannot be interpreted. */
    subpasses[1].pInputAttachments = NULL;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT &&
           pass == VK_NULL_HANDLE && d->graphics_objects == before);
    /* More references than the model holds. */
    VkAttachmentReference too_many[PS5VK_MAX_INPUT_ATTACHMENTS + 1];
    for (uint32_t i = 0; i < PS5VK_MAX_INPUT_ATTACHMENTS + 1; ++i)
        too_many[i] = (VkAttachmentReference){1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    subpasses[1].pInputAttachments = too_many;
    subpasses[1].inputAttachmentCount = PS5VK_MAX_INPUT_ATTACHMENTS + 1;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT &&
           pass == VK_NULL_HANDLE && d->graphics_objects == before);
    /* And an attachment that nothing references is still refused, whether the
     * input entry is removed or turned into VK_ATTACHMENT_UNUSED. */
    subpasses[1].pInputAttachments = inputs;
    subpasses[1].inputAttachmentCount = 2;
    inputs[0].attachment = VK_ATTACHMENT_UNUSED;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT &&
           pass == VK_NULL_HANDLE && d->graphics_objects == before);
    inputs[0].attachment = 1;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_SUCCESS);
    vkDestroyRenderPass(d, pass, NULL);
    assert(d->graphics_objects == before);
}

static void six_view_subpass_chain(struct VkDevice_T *d)
{
    const uint32_t saved_features = d->enabled_features;
    d->enabled_features |= PS5VK_FEATURE_MULTIVIEW;
    VkAttachmentDescription attachment = {.format=VK_FORMAT_R8G8B8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT, .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpasses[6];
    VkSubpassDependency dependencies[6];
    uint32_t masks[6];
    for (uint32_t i=0; i<6; ++i) {
        masks[i] = 1u << i;
        subpasses[i] = (VkSubpassDescription){.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount=1, .pColorAttachments=&color};
        dependencies[i] = (VkSubpassDependency){.srcSubpass=i, .dstSubpass=i<5 ? i+1 : i,
            .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
            .dependencyFlags=VK_DEPENDENCY_VIEW_LOCAL_BIT |
                (i==5 ? VK_DEPENDENCY_BY_REGION_BIT : 0)};
    }
    /* The pinned CTS omits offsets (count zero): every dependency still has
     * an implicit zero offset and must still undergo view-local validation. */
    VkRenderPassMultiviewCreateInfo mv = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
        .subpassCount=6, .pViewMasks=masks};
    VkRenderPassCreateInfo info = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext=&mv, .attachmentCount=1, .pAttachments=&attachment,
        .subpassCount=6, .pSubpasses=subpasses, .dependencyCount=6,
        .pDependencies=dependencies};
    VkRenderPass pass = NULL;
    assert(vkCreateRenderPass(d, &info, NULL, &pass)==VK_SUCCESS);
    assert(pass->subpass_count==6 && pass->multiview.dependency_count==6);
    for (uint32_t i=0; i<6; ++i) {
        assert(pass->multiview.view_masks[i]==(1u<<i));
        assert(pass->multiview.view_offsets[i]==0);
        masks[i]=0;
    }
    assert(pass->multiview.view_masks[5]==32); /* owned, not borrowed */
    vkDestroyRenderPass(d, pass, NULL);
    assert(vkCreateRenderPass(d, &info, NULL, &pass)==VK_ERROR_FEATURE_NOT_PRESENT);
    for (uint32_t i=0; i<6; ++i) masks[i]=1u<<i;
    dependencies[5].dependencyFlags=VK_DEPENDENCY_VIEW_LOCAL_BIT;
    assert(vkCreateRenderPass(d, &info, NULL, &pass)==VK_ERROR_FEATURE_NOT_PRESENT);
    dependencies[5].dependencyFlags |= VK_DEPENDENCY_BY_REGION_BIT;
    dependencies[4].dstSubpass=3;
    assert(vkCreateRenderPass(d, &info, NULL, &pass)==VK_ERROR_FEATURE_NOT_PRESENT);
    dependencies[4].dstSubpass=5;
    d->enabled_features=saved_features;
    assert(vkCreateRenderPass(d, &info, NULL, &pass)==VK_ERROR_FEATURE_NOT_PRESENT);
}

int main(void)
{
    struct VkDevice_T d = {0};
    VkAttachmentDescription attachments[2] = {
        {.format=VK_FORMAT_B8G8R8A8_UNORM, .samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {.format=VK_FORMAT_D32_SFLOAT, .samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference color={0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth={1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1, .pColorAttachments=&color, .pDepthStencilAttachment=&depth};
    VkRenderPassCreateInfo info={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=2, .pAttachments=attachments, .subpassCount=1, .pSubpasses=&sub};
    VkRenderPass pass;
    assert(vkCreateRenderPass(&d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT && !pass);
    d.graphics_enabled=1;
    assert(vkCreateRenderPass(&d, &info, NULL, &pass) == VK_SUCCESS);
    /* A DEPTH-ONLY pass: one depth attachment, colorAttachmentCount 0. This is
     * the shape the pinned upstream depth clamp module builds, and the pass
     * has to own it, because a framebuffer and a pipeline are derived from
     * what the subpass names. A pass that names neither role has no target at
     * all and stays refused. */
    {
        VkAttachmentDescription only_depth = attachments[1];
        only_depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        only_depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference depth_zero = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription depth_sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 0, .pColorAttachments = NULL,
            .pDepthStencilAttachment = &depth_zero};
        VkRenderPassCreateInfo depth_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &only_depth,
            .subpassCount = 1, .pSubpasses = &depth_sub};
        VkRenderPass depth_pass = VK_NULL_HANDLE;
        assert(vkCreateRenderPass(&d, &depth_info, NULL, &depth_pass) == VK_SUCCESS);
        const struct ps5vk_subpass *s = ps5vk_render_pass_subpass(depth_pass, 0);
        assert(s->color_count == 0);
        assert(s->depth.attachment == 0);
        assert(s->depth.attachment == 0);
        vkDestroyRenderPass(&d, depth_pass, NULL);

        /* Neither role named: nothing to render into. */
        VkSubpassDescription empty = depth_sub;
        empty.pDepthStencilAttachment = NULL;
        VkRenderPassCreateInfo empty_info = depth_info;
        empty_info.pSubpasses = &empty;
        VkRenderPass none = VK_NULL_HANDLE;
        assert(vkCreateRenderPass(&d, &empty_info, NULL, &none) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !none);

        /* Two colour attachments is still outside the profile. */
        VkAttachmentReference pair[2] = {
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
        VkSubpassDescription two = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 2, .pColorAttachments = pair};
        VkRenderPassCreateInfo two_info = info;
        two_info.pSubpasses = &two;
        VkRenderPass rejected = VK_NULL_HANDLE;
        assert(vkCreateRenderPass(&d, &two_info, NULL, &rejected) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !rejected);
    }

    /* T02-E1b shipping gate: a real view mask is accepted only on a device that
     * ENABLED the feature. The extension being reachable is not enough - the
     * same device without the enabled feature refuses the mask - and the
     * accepted pass really owns it. */
    {
        const uint32_t masks[1] = {0x3fu};
        VkRenderPassMultiviewCreateInfo multiview = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
            .subpassCount = 1, .pViewMasks = masks};
        VkRenderPassCreateInfo masked = info; masked.pNext = &multiview;
        VkRenderPass gated = VK_NULL_HANDLE;
        assert(vkCreateRenderPass(&d, &masked, NULL, &gated) == VK_ERROR_FEATURE_NOT_PRESENT && !gated);
        struct VkDevice_T enabled = d;
        enabled.enabled_features |= PS5VK_FEATURE_MULTIVIEW;
        assert(vkCreateRenderPass(&enabled, &masked, NULL, &gated) == VK_SUCCESS && gated);
        assert(gated->multiview.present && gated->multiview.view_masks[0] == 0x3fu);
        vkDestroyRenderPass(&enabled, gated, NULL);
        /* The reported limit is the measured floor: a mask whose most
         * significant bit reaches it is refused. */
        const uint32_t too_wide[1] = {0x40u};
        multiview.pViewMasks = too_wide;
        VkRenderPass wide = VK_NULL_HANDLE;
        assert(vkCreateRenderPass(&enabled, &masked, NULL, &wide) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !wide);
        /* Nothing was created on the fixture device itself, so the object
         * counters the rest of this test checks are untouched. */
    }
    /* DXVK262-T06 multisample contract. A device whose platform never carried
     * the sample-rate bit serves 1x alone, so a 4x colour attachment stays
     * refused; a platform that carries it serves the 1x/2x/4x envelope, and
     * then every attachment of the pass still has to agree on the count, and
     * no multisampled depth target exists on this path. */
    {
        VkAttachmentDescription only_color = attachments[0];
        only_color.samples = VK_SAMPLE_COUNT_4_BIT;
        only_color.format = VK_FORMAT_B8G8R8A8_UNORM;
        VkSubpassDescription no_depth = sub; no_depth.pDepthStencilAttachment=NULL;
        VkRenderPassCreateInfo no_depth_info = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount=1, .pAttachments=&only_color,
            .subpassCount=1, .pSubpasses=&no_depth};
        VkRenderPass multisample = VK_NULL_HANDLE;
        assert(vkCreateRenderPass(&d, &no_depth_info, NULL, &multisample) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !multisample);
        struct VkDevice_T sampled = d;
        sampled.platform_features |= PS5VK_FEATURE_SAMPLE_RATE_SHADING;
        assert(vkCreateRenderPass(&sampled, &no_depth_info, NULL, &multisample) == VK_SUCCESS &&
               multisample && multisample->attachments[0].samples == VK_SAMPLE_COUNT_4_BIT);
        vkDestroyRenderPass(&sampled, multisample, NULL);
        /* 8x is outside the envelope this profile is built for. */
        only_color.samples = VK_SAMPLE_COUNT_8_BIT;
        assert(vkCreateRenderPass(&sampled, &no_depth_info, NULL, &multisample) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !multisample);
        /* A pass whose colour and depth attachments disagree on the count
         * cannot be backed by one framebuffer, so it is refused: here the
         * colour attachment is 4x and the depth attachment stays 1x. */
        VkAttachmentDescription mixed[2] = {attachments[0], attachments[1]};
        mixed[0].samples = VK_SAMPLE_COUNT_4_BIT;
        VkRenderPassCreateInfo mixed_info = info;
        mixed_info.pAttachments = mixed;
        assert(vkCreateRenderPass(&sampled, &mixed_info, NULL, &multisample) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !multisample);
        /* And a multisampled depth attachment stays unserved even then. */
        VkAttachmentDescription sampled_depth[2] = {attachments[0], attachments[1]};
        sampled_depth[0].samples = VK_SAMPLE_COUNT_4_BIT;
        sampled_depth[1].samples = VK_SAMPLE_COUNT_4_BIT;
        VkRenderPassCreateInfo sampled_both = info;
        sampled_both.pAttachments = sampled_depth;
        assert(vkCreateRenderPass(&sampled, &sampled_both, NULL, &multisample) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !multisample);
    }
    /* The created pass owns its own copy of what it validated: mutating the
     * fixture afterwards changes nothing about the pass. */
    attachments[0].format=VK_FORMAT_UNDEFINED; color.attachment=1;
    assert(pass->attachments[0].format == VK_FORMAT_B8G8R8A8_UNORM &&
           ps5vk_render_pass_subpass(pass, 0)->color[0].attachment == 0 &&
           pass->subpass_count == 1);
    pass->pending=1; vkDestroyRenderPass(&d, pass, NULL);
    assert(d.graphics_objects == 1 && d.lifetime_errors == 1);
    pass->pending=0; vkDestroyRenderPass(&d, pass, NULL); assert(!d.graphics_objects);
    attachments[0].format=VK_FORMAT_B8G8R8A8_UNORM;
    assert(vkCreateRenderPass(&d, &info, NULL, &pass) == VK_ERROR_UNKNOWN && !pass);
    color.attachment=0; attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    assert(vkCreateRenderPass(&d, &info, NULL, &pass) == VK_ERROR_UNKNOWN);
    struct ps5vk_attachment_plan plan;
    attachments[0].initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    assert(ps5vk_attachment_plan(&attachments[0],VK_FORMAT_B8G8R8A8_UNORM,
        color.layout,VK_FALSE,VK_SAMPLE_COUNT_1_BIT,&plan)==VK_SUCCESS &&
        plan.load && plan.store && !plan.clear);
    attachments[0].format=VK_FORMAT_R8G8B8A8_UNORM;
    assert(ps5vk_attachment_plan(&attachments[0],VK_FORMAT_R8G8B8A8_UNORM,
        color.layout,VK_FALSE,VK_SAMPLE_COUNT_1_BIT,&plan)==VK_SUCCESS &&
        plan.load && plan.store && !plan.clear);
    attachments[0].format=VK_FORMAT_B8G8R8A8_UNORM;
    attachments[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    assert(ps5vk_attachment_plan(&attachments[0],VK_FORMAT_B8G8R8A8_UNORM,
        color.layout,VK_FALSE,VK_SAMPLE_COUNT_1_BIT,&plan)==VK_ERROR_FEATURE_NOT_PRESENT);
    /* The plan is bounded by the mask the device's platform serves: a count it
     * does not serve is refused, and one it does is executable - the clear is a
     * whole-surface fill, which covers every sample of every texel. */
    attachments[0].initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].samples=VK_SAMPLE_COUNT_4_BIT;
    assert(ps5vk_attachment_plan(&attachments[0],VK_FORMAT_B8G8R8A8_UNORM,
        color.layout,VK_FALSE,VK_SAMPLE_COUNT_1_BIT,&plan)==VK_ERROR_FEATURE_NOT_PRESENT);
    assert(ps5vk_attachment_plan(&attachments[0],VK_FORMAT_B8G8R8A8_UNORM,
        color.layout,VK_FALSE,VK_SAMPLE_COUNT_4_BIT,&plan)==VK_SUCCESS && !plan.clear);
    /* The depth role never carries more than one sample. */
    attachments[0].samples=VK_SAMPLE_COUNT_1_BIT;
    attachments[1].samples=VK_SAMPLE_COUNT_4_BIT;
    assert(ps5vk_attachment_plan(&attachments[1],VK_FORMAT_D32_SFLOAT,
        depth.layout,VK_TRUE,VK_SAMPLE_COUNT_4_BIT,&plan)==VK_ERROR_FEATURE_NOT_PRESENT);
    attachments[1].samples=VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachments[1].samples=VK_SAMPLE_COUNT_4_BIT;
    assert(vkCreateRenderPass(&d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    attachments[1].samples=VK_SAMPLE_COUNT_1_BIT;
    VkSubpassDependency dep={.srcSubpass=VK_SUBPASS_EXTERNAL, .dstSubpass=0,
        .srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT, .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    info.dependencyCount=1; info.pDependencies=&dep;
    assert(vkCreateRenderPass(&d, &info, NULL, &pass) == VK_SUCCESS);
    VkExtent2D gran = {0, 0};
    vkGetRenderAreaGranularity(&d, pass, &gran);
    assert(gran.width == 1 && gran.height == 1);
    vkGetRenderAreaGranularity(&d, pass, NULL);
    gran = (VkExtent2D){99, 99};
    vkGetRenderAreaGranularity(NULL, pass, &gran);
    assert(gran.width == 0 && gran.height == 0);
    gran = (VkExtent2D){99, 99};
    vkGetRenderAreaGranularity(&d, NULL, &gran);
    assert(gran.width == 0 && gran.height == 0);
    struct VkDevice_T other_d = {0};
    gran = (VkExtent2D){99, 99};
    vkGetRenderAreaGranularity(&other_d, pass, &gran);
    assert(gran.width == 0 && gran.height == 0);
    dep.srcAccessMask=0;
    assert(pass->dependencies[0].srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
    vkDestroyRenderPass(&d, pass, NULL);
    dep.srcSubpass=0;
    assert(vkCreateRenderPass(&d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(!d.graphics_objects);
    multiple_subpasses(&d);
    resolve_attachments(&d);
    multiview_model(&d);
    input_attachments(&d);
    six_view_subpass_chain(&d);
    puts("Render pass owned subpass/attachment/dependency data: host only; no execution");
}
