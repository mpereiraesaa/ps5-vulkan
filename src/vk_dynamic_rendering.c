/* VK_KHR_dynamic_rendering (DXVK262-T10).
 *
 * The pinned DXVK 2.6.2 renders exclusively through vkCmdBeginRendering /
 * vkCmdEndRendering (dxvk_context.cpp:5808-6010): for the first draw one colour
 * attachment in COLOR_ATTACHMENT_OPTIMAL, loadOp CLEAR (a deferred
 * ClearRenderTargetView folded into the pass, :2396-2409,5836-5844) or LOAD,
 * storeOp STORE, renderArea = the framebuffer size at offset zero,
 * layerCount 1, viewMask 0, no resolve, flags 0 (the secondary-contents flag
 * only for MSAA or tiler mode, :5929-5954). A standalone clear is the same
 * begin/end pair with nothing in between (:2136-2236).
 *
 * A render pass instance begun here is executed by the existing render-pass
 * machinery: the begin info is translated into the version-1 description of an
 * equivalent single-subpass pass - one attachment per colour attachment in
 * order, then the depth/stencil attachment, each with initial = reference =
 * final layout because dynamic rendering performs no layout transition - and
 * that description goes through ps5vk_render_pass_create, so it is validated
 * by exactly the rules vkCreateRenderPass applies. The pass and a framebuffer
 * naming the views are then stored BY VALUE as the begin operation's owned
 * payload: nothing retains a caller pointer, the draws recorded inside the
 * instance see an ordinary pass, and the queue and the native attachment
 * plans execute it exactly as they execute the equivalent vkCmdBeginRenderPass.
 *
 * Every shape the translation cannot express exactly is refused where it is
 * recorded, never approximated: a flag (secondary contents, suspending,
 * resuming), a view mask or more than one layer, a hole in the colour array
 * (a colour attachment with no view), a resolve, a STORE_OP_NONE or
 * LOAD_OP_NONE, a layout other than the attachment layout or GENERAL, differing
 * depth and stencil layouts, and any pNext. */
#include "vk_command.h"
#include "vk_dynamic_rendering.h"
#include "vk_framebuffer.h"
#include "vk_image.h"
#include "depth_stencil_layout.h"
#include <string.h>

#if defined(PS5VK_TARGET_PS5) && PS5VK_TARGET_PS5
#include "ps5log.h"
#define DR_MARK(...) ps5log_printf(PS5LOG_MARK, __VA_ARGS__)
#else
#define DR_MARK(...) ((void)0)
#endif

/* The owned payload of one begin operation. The pass's array pointers are
 * re-aimed at this payload's own arrays after it is copied into the command
 * buffer, so the object never points outside its allocation. */
struct ps5vk_dynamic_rendering_payload {
    struct VkRenderPass_T pass;
    struct VkFramebuffer_T framebuffer;
    VkAttachmentDescription attachments[PS5VK_MAX_ATTACHMENTS];
    struct ps5vk_subpass subpass;
};

static int load_op(VkAttachmentLoadOp op)
{
    return op == VK_ATTACHMENT_LOAD_OP_LOAD || op == VK_ATTACHMENT_LOAD_OP_CLEAR ||
        op == VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}
static int store_op(VkAttachmentStoreOp op)
{
    return op == VK_ATTACHMENT_STORE_OP_STORE || op == VK_ATTACHMENT_STORE_OP_DONT_CARE;
}
static int attachment_info(const VkRenderingAttachmentInfo *a, VkImageLayout attachment_layout)
{
    return a->sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO && !a->pNext &&
        a->imageView && a->resolveMode == VK_RESOLVE_MODE_NONE &&
        (a->imageLayout == attachment_layout || a->imageLayout == VK_IMAGE_LAYOUT_GENERAL) &&
        load_op(a->loadOp) && store_op(a->storeOp);
}

VkResult ps5vk_dynamic_rendering_pass(VkDevice d, const VkRenderingInfo *info,
    struct ps5vk_dynamic_rendering_shape *out)
{
    if (!d || !info || !out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    if (info->sType != VK_STRUCTURE_TYPE_RENDERING_INFO) return VK_ERROR_UNKNOWN;
    if (info->pNext || info->flags || info->viewMask || info->layerCount != 1 ||
        info->colorAttachmentCount > PS5VK_MAX_COLOR_ATTACHMENTS ||
        (info->colorAttachmentCount && !info->pColorAttachments))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->renderArea.offset.x < 0 || info->renderArea.offset.y < 0 ||
        !info->renderArea.extent.width || !info->renderArea.extent.height)
        return VK_ERROR_UNKNOWN;
    uint32_t n = 0;
    for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
        const VkRenderingAttachmentInfo *a = &info->pColorAttachments[i];
        if (!attachment_info(a, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ||
            a->imageView->device != d)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        out->views[n] = a->imageView;
        out->clears[n] = a->clearValue;
        out->attachments[n] = (VkAttachmentDescription){
            .format = a->imageView->format, .samples = a->imageView->image->info.samples,
            .loadOp = a->loadOp, .storeOp = a->storeOp,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = a->imageLayout, .finalLayout = a->imageLayout};
        out->color[n] = (VkAttachmentReference){n, a->imageLayout};
        ++n;
    }
    out->color_count = n;
    const VkRenderingAttachmentInfo *depth =
        info->pDepthAttachment && info->pDepthAttachment->imageView ? info->pDepthAttachment : NULL;
    const VkRenderingAttachmentInfo *stencil =
        info->pStencilAttachment && info->pStencilAttachment->imageView ?
        info->pStencilAttachment : NULL;
    out->depth = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
    if (depth || stencil) {
        const VkRenderingAttachmentInfo *any = depth ? depth : stencil;
        const VkImageLayout ds = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if ((depth && !attachment_info(depth, ds)) || (stencil && !attachment_info(stencil, ds)) ||
            (depth && stencil && (depth->imageView != stencil->imageView ||
                                  depth->imageLayout != stencil->imageLayout)) ||
            any->imageView->device != d)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        const VkImageAspectFlags aspects = ps5vk_format_aspects(any->imageView->format);
        /* Each pointer names an aspect the view's format has
         * (VUID-VkRenderingInfo-pDepthAttachment-06547, -pStencilAttachment-06548). */
        if ((depth && !(aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) ||
            (stencil && !(aspects & VK_IMAGE_ASPECT_STENCIL_BIT)))
            return VK_ERROR_UNKNOWN;
        /* An aspect the instance does not name is not rendered and keeps its
         * contents: it is loaded and stored, never discarded. */
        out->views[n] = any->imageView;
        out->clears[n].depthStencil.depth = depth ? depth->clearValue.depthStencil.depth : 0.0f;
        out->clears[n].depthStencil.stencil =
            stencil ? stencil->clearValue.depthStencil.stencil : 0u;
        out->attachments[n] = (VkAttachmentDescription){
            .format = any->imageView->format, .samples = any->imageView->image->info.samples,
            .loadOp = depth ? depth->loadOp : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = depth ? depth->storeOp : VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = stencil ? stencil->loadOp : VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = stencil ? stencil->storeOp : VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = any->imageLayout, .finalLayout = any->imageLayout};
        out->depth = (VkAttachmentReference){n, any->imageLayout};
        ++n;
    }
    if (!n) return VK_ERROR_FEATURE_NOT_PRESENT;
    out->attachment_count = n;
    return VK_SUCCESS;
}

/* The render area must lie inside every attachment
 * (VUID-VkRenderingInfo-pNext-06079, -06080); the framebuffer extent is the
 * smallest attachment extent, which is what that rule bounds. */
static int framebuffer_extent(const struct ps5vk_dynamic_rendering_shape *shape,
    const VkRect2D *area, uint32_t *width, uint32_t *height)
{
    uint32_t w = UINT32_MAX, h = UINT32_MAX;
    for (uint32_t i = 0; i < shape->attachment_count; ++i) {
        VkImageView view = shape->views[i];
        if (!view->image || view->range.baseMipLevel >= 32) return 0;
        uint32_t vw = view->image->info.extent.width >> view->range.baseMipLevel;
        uint32_t vh = view->image->info.extent.height >> view->range.baseMipLevel;
        if (!vw) vw = 1;
        if (!vh) vh = 1;
        if (vw < w) w = vw;
        if (vh < h) h = vh;
    }
    if ((uint64_t)(uint32_t)area->offset.x + area->extent.width > w ||
        (uint64_t)(uint32_t)area->offset.y + area->extent.height > h) return 0;
    *width = w; *height = h;
    return 1;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderingKHR(VkCommandBuffer c, const VkRenderingInfo *info)
{
    DR_MARK("PS5VK_CMD_BEGIN_RENDERING colors=%u depth=%u",
        info ? info->colorAttachmentCount : 0u,
        info && info->pDepthAttachment && info->pDepthAttachment->imageView ? 1u : 0u);
    if (!c) return;
    VkDevice d = c->pool ? c->pool->device : NULL;
    /* Primary-only here: a secondary would inherit the instance through
     * VkCommandBufferInheritanceRenderingInfo, which is not implemented. */
    if (c->state != PS5VK_RECORDING || !d || !d->dynamic_rendering_enabled ||
        !d->graphics_enabled || c->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY || c->render_pass ||
        c->operation_count >= PS5VK_MAX_OPERATIONS) {
        ps5vk_command_invalidate(c); return;
    }
    struct ps5vk_dynamic_rendering_shape shape;
    uint32_t width = 0, height = 0;
    if (ps5vk_dynamic_rendering_pass(d, info, &shape) != VK_SUCCESS ||
        !framebuffer_extent(&shape, &info->renderArea, &width, &height)) {
        ps5vk_command_invalidate(c); return;
    }
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = shape.color_count, .pColorAttachments = shape.color,
        .pDepthStencilAttachment = shape.depth.attachment == VK_ATTACHMENT_UNUSED ?
            NULL : &shape.depth};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = shape.attachment_count, .pAttachments = shape.attachments,
        .subpassCount = 1, .pSubpasses = &subpass};
    VkRenderPass temporary = VK_NULL_HANDLE;
    if (ps5vk_render_pass_create(d, &pass_info, NULL, NULL, &temporary) != VK_SUCCESS) {
        ps5vk_command_invalidate(c); return;
    }
    struct ps5vk_dynamic_rendering_payload built;
    memset(&built, 0, sizeof(built));
    built.pass = *temporary;
    memcpy(built.attachments, temporary->attachments,
        temporary->attachment_count * sizeof(*built.attachments));
    built.subpass = temporary->subpasses[0];
    built.pass.pending = 0;
    struct VkFramebuffer_T *fb = &built.framebuffer;
    fb->device = d;
    fb->width = width; fb->height = height;
    fb->attachment_count = shape.attachment_count;
    fb->color_count = shape.color_count;
    for (uint32_t i = 0; i < shape.color_count; ++i) fb->color_attachments[i] = i;
    fb->depth_attachment = shape.depth.attachment;
    for (uint32_t i = 0; i < shape.attachment_count; ++i) {
        fb->attachments[i] = shape.views[i];
        fb->formats[i] = shape.attachments[i].format;
        fb->samples[i] = shape.attachments[i].samples;
    }
    /* The same per-view rules vkCreateFramebuffer applies to its views. */
    int views_valid = 1;
    for (uint32_t i = 0; i < shape.attachment_count && views_valid; ++i)
        views_valid = ps5vk_framebuffer_attachment_valid(fb, temporary, i, shape.views[i]) &&
            shape.views[i]->range.layerCount == 1;
    vkDestroyRenderPass(d, temporary, NULL);
    if (!views_valid) { ps5vk_command_invalidate(c); return; }
    struct ps5vk_operation *op = ps5vk_command_reserve_operation_with_payload(c,
        PS5VK_BEGIN_RENDER_PASS, PS5VK_OPERATION_OUTSIDE_RENDER_PASS, &built, sizeof(built));
    if (!op) return;
    struct ps5vk_dynamic_rendering_payload *owned = op->owned_payload;
    owned->pass.attachments = owned->attachments;
    owned->pass.subpasses = &owned->subpass;
    owned->pass.dependencies = NULL;
    owned->pass.inputs = NULL;
    owned->pass.preserves = NULL;
    op->render_pass = &owned->pass;
    op->framebuffer = &owned->framebuffer;
    op->render_area = info->renderArea;
    op->render_pass_contents = VK_SUBPASS_CONTENTS_INLINE;
    op->subpass = 0;
    op->clear_count = shape.attachment_count;
    memcpy(op->clears, shape.clears, shape.attachment_count * sizeof(VkClearValue));
    c->render_pass = op->render_pass;
    c->framebuffer = op->framebuffer;
    c->render_pass_contents = VK_SUBPASS_CONTENTS_INLINE;
    c->subpass = 0;
    c->dynamic_rendering = VK_TRUE;
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderingKHR(VkCommandBuffer c)
{
    DR_MARK("PS5VK_CMD_END_RENDERING");
    if (!c) return;
    if (c->state != PS5VK_RECORDING || !c->dynamic_rendering || !c->render_pass) {
        ps5vk_command_invalidate(c); return;
    }
    /* The instance ends exactly as the equivalent render pass does. */
    c->dynamic_rendering = VK_FALSE;
    vkCmdEndRenderPass(c);
}
