#include "vk_render_pass.h"
#include "attachment_ops.h"
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
        assert(!s->color.attachment && s->depth.attachment == 1);
    }
    assert(pass->attachments[0].format == VK_FORMAT_B8G8R8A8_UNORM &&
           !pass->dependencies[0].srcSubpass && pass->dependencies[0].dstSubpass == 1);
    vkDestroyRenderPass(d, pass, NULL);
    color.attachment = 0; depth.attachment = 1; between.srcSubpass = 0;
    attachments[0].format = VK_FORMAT_B8G8R8A8_UNORM;

    /* Refused shapes. Each leaves no object and no accounting behind. */
    const unsigned objects = d->graphics_objects;

    /* A preserve list has no legal non-empty form in this profile: every
     * subpass names the same attachments and every attachment must be named,
     * so nothing can be preserved-but-unused. The list is refused rather than
     * stored where it could never mean anything. */
    uint32_t preserved = 1;
    subpasses[0].preserveAttachmentCount = 1;
    subpasses[0].pPreserveAttachments = &preserved;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT &&
           !pass);
    subpasses[0].preserveAttachmentCount = 0;
    subpasses[0].pPreserveAttachments = NULL;

    /* Vulkan IGNORES pInputAttachments when the count is zero, so a stale
     * pointer beside a zero count must NOT be refused. */
    VkAttachmentReference ignored = {0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    subpasses[0].pInputAttachments = &ignored;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_SUCCESS);
    vkDestroyRenderPass(d, pass, NULL);
    subpasses[0].pInputAttachments = NULL;

    /* Subpasses must name the SAME attachments: a pass whose subpasses
     * disagreed about which attachment is the colour one could be created and
     * then served by no framebuffer at all. */
    VkAttachmentReference other_color = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    subpasses[1].pColorAttachments = &other_color;
    subpasses[1].pDepthStencilAttachment = NULL;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    subpasses[1].pColorAttachments = &color;
    subpasses[1].pDepthStencilAttachment = &depth;
    /* Including the case where one subpass simply drops the depth role. */
    subpasses[1].pDepthStencilAttachment = NULL;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    subpasses[1].pDepthStencilAttachment = &depth;

    /* more subpasses than the profile executes */
    VkSubpassDescription three[3] = {subpasses[0], subpasses[1], subpasses[0]};
    info.subpassCount = 3; info.pSubpasses = three;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    info.subpassCount = 0;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    info.subpassCount = 2; info.pSubpasses = subpasses;

    /* a requested input or resolve attachment is refused rather than ignored */
    VkAttachmentReference extra = {0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    subpasses[1].inputAttachmentCount = 1; subpasses[1].pInputAttachments = &extra;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    subpasses[1].inputAttachmentCount = 0; subpasses[1].pInputAttachments = NULL;
    subpasses[1].pResolveAttachments = &extra;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    subpasses[1].pResolveAttachments = NULL;

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
    attachments[0].format=VK_FORMAT_UNDEFINED; color.attachment=1;
    assert(pass->attachments[0].format == VK_FORMAT_B8G8R8A8_UNORM &&
           ps5vk_render_pass_subpass(pass, 0)->color.attachment == 0 &&
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
        color.layout,VK_FALSE,&plan)==VK_SUCCESS && plan.load && plan.store && !plan.clear);
    attachments[0].format=VK_FORMAT_R8G8B8A8_UNORM;
    assert(ps5vk_attachment_plan(&attachments[0],VK_FORMAT_R8G8B8A8_UNORM,
        color.layout,VK_FALSE,&plan)==VK_SUCCESS && plan.load && plan.store && !plan.clear);
    attachments[0].format=VK_FORMAT_B8G8R8A8_UNORM;
    attachments[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    assert(ps5vk_attachment_plan(&attachments[0],VK_FORMAT_B8G8R8A8_UNORM,
        color.layout,VK_FALSE,&plan)==VK_ERROR_FEATURE_NOT_PRESENT);
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
    multiview_model(&d);
    puts("Render pass owned subpass/attachment/dependency data: host only; no execution");
}
