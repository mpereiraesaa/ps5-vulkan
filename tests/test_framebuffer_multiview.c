/* DXVK262-T02 slice D1a: the private diagnostic gate and the framebuffer
 * contract a six-view native pass needs.
 *
 * This file is compiled TWICE - as the shipping build and with
 * -DPS5VK_MULTIVIEW_DIAGNOSTIC=1 - and asserts the SAME pass in both: with the
 * gate it is created and served by a layered framebuffer, and without it the
 * mask is refused exactly as before. A pass with zero view masks behaves
 * identically in both builds, because the layer requirement below exists only
 * where a mask names views. No capability is advertised anywhere in either
 * build: the gate changes what a render pass may contain, never what the device
 * reports. */
#include "vk_image.h"
#include "vk_framebuffer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static VkResult requirements(VkDevice d, const VkImageCreateInfo *info, VkMemoryRequirements *out)
{ (void)d; (void)info; *out = (VkMemoryRequirements){4096u, 256u, 1u}; return VK_SUCCESS; }
static VkResult allocate(void *c, VkDeviceSize n, void **a, void **b)
{ (void)c; *a = *b = calloc(1, (size_t)n); return *a ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY; }
static void release_memory(void *c, void *b) { (void)c; free(b); }
static VkResult sync_memory(void *c, void *b, VkDeviceSize o, VkDeviceSize n)
{ (void)c; (void)b; (void)o; (void)n; return VK_SUCCESS; }

static VkImage make_image(struct VkDevice_T *d, VkFormat format, uint32_t layers,
    VkDeviceMemory *memory_out)
{
    const VkImageCreateInfo info = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D, .format=format, .extent={16u,16u,1u},
        .mipLevels=1u, .arrayLayers=layers, .samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkImage image = VK_NULL_HANDLE;
    assert(vkCreateImage(d, &info, NULL, &image) == VK_SUCCESS);
    const VkMemoryAllocateInfo allocation = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=8192u};
    assert(vkAllocateMemory(d, &allocation, NULL, memory_out) == VK_SUCCESS);
    assert(vkBindImageMemory(d, image, *memory_out, 0) == VK_SUCCESS);
    return image;
}

/* One attachment view. `expected` is the outcome this build must produce, so a
 * scene that is legal under the gate and impossible without it stays readable
 * in one place. */
static VkImageView make_view(struct VkDevice_T *d, VkImage image, VkFormat format,
    VkImageViewType type, uint32_t base, uint32_t layers, VkResult expected)
{
    const VkImageViewCreateInfo info = {.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=image, .viewType=type, .format=format,
        .subresourceRange={format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT :
            VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, base, layers}};
    VkImageView view = VK_NULL_HANDLE;
    const VkResult rc = vkCreateImageView(d, &info, NULL, &view);
    if (rc != expected || (rc == VK_SUCCESS) != (view != VK_NULL_HANDLE)) {
        fprintf(stderr, "view %ux%u rc=%d expected=%d\n", base, layers, rc, expected);
        assert(0);
    }
    return view;
}

/* One colour+depth pass. A NULL mask builds the pass with no multiview
 * structure at all; a zero mask builds one that carries zeroes. */
static VkRenderPass make_pass(struct VkDevice_T *d, const uint32_t *mask, VkResult expected)
{
    const VkAttachmentDescription attachments[2] = {
        {.format=VK_FORMAT_R8G8B8A8_UNORM, .samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.format=VK_FORMAT_D32_SFLOAT, .samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    const VkAttachmentReference color = {0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth = {1u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass = {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1u, .pColorAttachments=&color, .pDepthStencilAttachment=&depth};
    const VkRenderPassMultiviewCreateInfo multiview = {
        .sType=VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
        .subpassCount=1u, .pViewMasks=mask};
    VkRenderPassCreateInfo info = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=2u, .pAttachments=attachments,
        .subpassCount=1u, .pSubpasses=&subpass};
    if (mask) info.pNext = &multiview;
    VkRenderPass pass = VK_NULL_HANDLE;
    const VkResult rc = vkCreateRenderPass(d, &info, NULL, &pass);
    if (rc != expected || (rc == VK_SUCCESS) != (pass != VK_NULL_HANDLE)) {
        fprintf(stderr, "pass rc=%d expected=%d\n", rc, expected);
        assert(0);
    }
    return pass;
}

static VkFramebuffer make_framebuffer(struct VkDevice_T *d, VkRenderPass pass,
    VkImageView color, VkImageView depth, uint32_t layers, VkResult expected)
{
    const VkImageView attachments[2] = {color, depth};
    const VkFramebufferCreateInfo info = {.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=pass, .attachmentCount=2u, .pAttachments=attachments,
        .width=16u, .height=16u, .layers=layers};
    VkFramebuffer fb = VK_NULL_HANDLE;
    const VkResult rc = vkCreateFramebuffer(d, &info, NULL, &fb);
    if (rc != expected || (rc == VK_SUCCESS) != (fb != VK_NULL_HANDLE)) {
        fprintf(stderr, "framebuffer layers=%u rc=%d expected=%d\n", layers, rc, expected);
        assert(0);
    }
    return fb;
}

static void drop(struct VkDevice_T *d, VkFramebuffer fb, VkImageView color, VkImageView depth,
    VkImage color_image, VkImage depth_image, VkDeviceMemory color_memory, VkDeviceMemory depth_memory)
{
    if (fb) vkDestroyFramebuffer(d, fb, NULL);
    if (color) vkDestroyImageView(d, color, NULL);
    if (depth) vkDestroyImageView(d, depth, NULL);
    if (color_image) vkDestroyImage(d, color_image, NULL);
    if (depth_image) vkDestroyImage(d, depth_image, NULL);
    if (color_memory) vkFreeMemory(d, color_memory, NULL);
    if (depth_memory) vkFreeMemory(d, depth_memory, NULL);
}

int main(void)
{
    struct VkDevice_T d = {.max_allocation=65536u, .noncoherent_atom=64u,
        .memory={NULL, allocate, release_memory, sync_memory, sync_memory},
        .image_requirements=requirements, .graphics_enabled=1};
    const uint32_t six_views[1] = {0x3fu};
    const uint32_t zero_views[1] = {0u};
    VkDeviceMemory color_memory = VK_NULL_HANDLE, depth_memory = VK_NULL_HANDLE;

    /* Zero view masks: unchanged in BOTH builds. A pass that carries a multiview
     * structure with a zero mask, and one that carries none at all, both serve
     * six-layer array attachments exactly as the profile always served a single
     * layer, because the layer requirement exists only where a mask names
     * views. */
    {
        VkRenderPass plain = make_pass(&d, NULL, VK_SUCCESS);
        VkRenderPass zero = make_pass(&d, zero_views, VK_SUCCESS);
        VkImage color_image = make_image(&d, VK_FORMAT_R8G8B8A8_UNORM, 6u, &color_memory);
        VkImage depth_image = make_image(&d, VK_FORMAT_D32_SFLOAT, 6u, &depth_memory);
        VkImageView color = make_view(&d, color_image, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0u, 6u, VK_SUCCESS);
        VkImageView depth = make_view(&d, depth_image, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0u, 6u, VK_SUCCESS);
        VkFramebuffer fb = make_framebuffer(&d, plain, color, depth, 1u, VK_SUCCESS);
        VkFramebuffer zero_fb = make_framebuffer(&d, zero, color, depth, 1u, VK_SUCCESS);
        vkDestroyFramebuffer(&d, zero_fb, NULL);
        vkDestroyRenderPass(&d, zero, NULL);
        drop(&d, fb, color, depth, color_image, depth_image, color_memory, depth_memory);
        vkDestroyRenderPass(&d, plain, NULL);
        color_memory = depth_memory = VK_NULL_HANDLE;
    }

#if PS5VK_MULTIVIEW_DIAGNOSTIC
    /* The gate is ON: this is the pass D1b will measure, and the framebuffer
     * contract it needs. The pass really owns the six-view mask. */
    {
        VkRenderPass pass = make_pass(&d, six_views, VK_SUCCESS);
        assert(pass->multiview.present && pass->multiview.subpass_count == 1u &&
               pass->multiview.view_masks[0] == 0x3fu);
        VkImage color_image = make_image(&d, VK_FORMAT_R8G8B8A8_UNORM, 6u, &color_memory);
        VkImage depth_image = make_image(&d, VK_FORMAT_D32_SFLOAT, 6u, &depth_memory);
        VkImageView color = make_view(&d, color_image, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0u, 6u, VK_SUCCESS);
        VkImageView depth = make_view(&d, depth_image, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0u, 6u, VK_SUCCESS);
        VkFramebuffer fb = make_framebuffer(&d, pass, color, depth, 1u, VK_SUCCESS);
        /* The framebuffer itself still has exactly one layer. */
        assert(make_framebuffer(&d, pass, color, depth, 2u,
            VK_ERROR_FEATURE_NOT_PRESENT) == VK_NULL_HANDLE);
        /* A colour view that carries five views cannot serve a six-view pass,
         * and neither can a depth view that does: the rule is per attachment. */
        VkImageView five_color = make_view(&d, color_image, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0u, 5u, VK_SUCCESS);
        VkImageView five_depth = make_view(&d, depth_image, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0u, 5u, VK_SUCCESS);
        assert(make_framebuffer(&d, pass, five_color, depth, 1u,
            VK_ERROR_FEATURE_NOT_PRESENT) == VK_NULL_HANDLE);
        assert(make_framebuffer(&d, pass, color, five_depth, 1u,
            VK_ERROR_FEATURE_NOT_PRESENT) == VK_NULL_HANDLE);
        /* A single-layer view is the one-layer case of the same rule. */
        VkImageView one_layer = make_view(&d, depth_image, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0u, 1u, VK_SUCCESS);
        assert(make_framebuffer(&d, pass, color, one_layer, 1u,
            VK_ERROR_FEATURE_NOT_PRESENT) == VK_NULL_HANDLE);
        /* The view has to start at layer zero even when it carries enough
         * layers: a seven-layer backing with a view at layer one is refused. */
        VkDeviceMemory seven_memory = VK_NULL_HANDLE;
        VkImage seven_image = make_image(&d, VK_FORMAT_D32_SFLOAT, 7u, &seven_memory);
        VkImageView offset_depth = make_view(&d, seven_image, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1u, 6u, VK_SUCCESS);
        assert(make_framebuffer(&d, pass, color, offset_depth, 1u,
            VK_ERROR_FEATURE_NOT_PRESENT) == VK_NULL_HANDLE);
        /* A backing with five layers cannot even hand out a six-layer view, so
         * that scene cannot reach the framebuffer at all. */
        VkDeviceMemory five_memory = VK_NULL_HANDLE;
        VkImage five_image = make_image(&d, VK_FORMAT_D32_SFLOAT, 5u, &five_memory);
        assert(make_view(&d, five_image, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0u, 6u, VK_ERROR_UNKNOWN) == VK_NULL_HANDLE);
        /* A mask that names a view at or beyond the diagnostic limit is refused
         * where the caller sees it, before any framebuffer exists. */
        const uint32_t too_wide[1] = {0x40u};
        assert(make_pass(&d, too_wide, VK_ERROR_FEATURE_NOT_PRESENT) == VK_NULL_HANDLE);
        vkDestroyImageView(&d, offset_depth, NULL);
        vkDestroyImage(&d, seven_image, NULL);
        vkFreeMemory(&d, seven_memory, NULL);
        vkDestroyImage(&d, five_image, NULL);
        vkFreeMemory(&d, five_memory, NULL);
        vkDestroyImageView(&d, one_layer, NULL);
        vkDestroyImageView(&d, five_color, NULL);
        vkDestroyImageView(&d, five_depth, NULL);
        drop(&d, fb, color, depth, color_image, depth_image, color_memory, depth_memory);
        vkDestroyRenderPass(&d, pass, NULL);
        color_memory = depth_memory = VK_NULL_HANDLE;
    }
#else
    /* The shipping build keeps the profile's answer: the very same mask is
     * refused when the pass is created, so no framebuffer can ever name it and
     * the layer contract above is unreachable here. */
    assert(make_pass(&d, six_views, VK_ERROR_FEATURE_NOT_PRESENT) == VK_NULL_HANDLE);
    {
        const uint32_t too_wide[1] = {0x40u};
        assert(make_pass(&d, too_wide, VK_ERROR_FEATURE_NOT_PRESENT) == VK_NULL_HANDLE);
    }
#endif

    assert(!d.images && !d.graphics_objects && !d.memories);
    printf("Framebuffer view masks: %s build, host backend only\n",
        PS5VK_MULTIVIEW_DIAGNOSTIC ? "diagnostic" : "shipping");
    return 0;
}
