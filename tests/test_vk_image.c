#include "vk_image.h"
#include "vk_framebuffer.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static int calls;
static VkResult requirements(VkDevice d, const VkImageCreateInfo *info, VkMemoryRequirements *out)
{
    (void)d; assert(info->extent.width == 17); ++calls;
    /* Deliberately padded synthetic backend layout, not a GPU surface formula. */
    *out = (VkMemoryRequirements){4096, 256, 1}; return VK_SUCCESS;
}
static VkResult allocate(void *c, VkDeviceSize n, void **a, void **b)
{ (void)c; *a = *b = calloc(1, n); return *a ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY; }
static void release(void *c, void *b) { (void)c; free(b); }
static VkResult sync_memory(void *c, void *b, VkDeviceSize o, VkDeviceSize n)
{ (void)c; (void)b; (void)o; (void)n; return VK_SUCCESS; }
int main(void)
{
    struct VkDevice_T d = {.max_allocation=65536, .noncoherent_atom=64,
        .memory={NULL, allocate, release, sync_memory, sync_memory}, .image_requirements=requirements};
    VkImageCreateInfo info = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM, .extent={17,19,1}, .mipLevels=1, .arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT, .tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkImage image;
    assert(vkCreateImage(&d, &info, NULL, &image) == VK_ERROR_FEATURE_NOT_PRESENT && !calls);
    d.graphics_enabled=1;
    VkImageCreateInfo delegated = info;
    delegated.format = VK_FORMAT_R8_UNORM;
    delegated.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage delegated_image;
    assert(vkCreateImage(&d, &delegated, NULL, &delegated_image) == VK_SUCCESS && calls == 1);
    vkDestroyImage(&d, delegated_image, NULL);
    assert(vkCreateImage(&d, &info, NULL, &image) == VK_SUCCESS && calls == 2);
    VkMemoryRequirements req; vkGetImageMemoryRequirements(&d, image, &req);
    assert(req.size == 4096 && req.alignment == 256);
    /* DXVK262-T06: a multisampled colour attachment exists only on a device
     * whose platform serves the counts, and only for that one role. The
     * backend's requirements are never reached for a refused shape, so the
     * platform gate is what decides before any allocation question is asked. */
    {
        VkImageCreateInfo sampled = info;
        sampled.samples = VK_SAMPLE_COUNT_4_BIT;
        VkImage sampled_image;
        const int before = calls;
        assert(vkCreateImage(&d, &sampled, NULL, &sampled_image) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !sampled_image && calls == before);
        d.platform_features |= PS5VK_FEATURE_SAMPLE_RATE_SHADING;
        assert(vkCreateImage(&d, &sampled, NULL, &sampled_image) == VK_SUCCESS &&
               sampled_image && calls == before + 1);
        vkDestroyImage(&d, sampled_image, NULL);
        /* The role combinations the pinned multisample oracle builds are the
         * accepted multisampled shapes: the colour attachment with its readback
         * source, and the per-sample fetch form that adds the input role. */
        sampled.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        assert(vkCreateImage(&d, &sampled, NULL, &sampled_image) == VK_SUCCESS &&
               sampled_image && calls == before + 2);
        vkDestroyImage(&d, sampled_image, NULL);
        sampled.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        assert(vkCreateImage(&d, &sampled, NULL, &sampled_image) == VK_SUCCESS &&
               sampled_image && calls == before + 3);
        vkDestroyImage(&d, sampled_image, NULL);
        /* Neighbouring combinations stay refused: an input role without the
         * readback source, the transfer-destination pair, and the sampled role
         * all name a path no multisampled shape here serves. */
        const VkImageUsageFlags refused_roles[] = {
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
        };
        for (unsigned role = 0; role < sizeof(refused_roles) / sizeof(refused_roles[0]); ++role) {
            sampled.usage = refused_roles[role];
            assert(vkCreateImage(&d, &sampled, NULL, &sampled_image) ==
                   VK_ERROR_FEATURE_NOT_PRESENT && !sampled_image && calls == before + 3);
        }
        sampled.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        /* Every other role stays single-sample: a sampled multisampled image
         * and a multisampled depth surface are both refused. */
        sampled.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        assert(vkCreateImage(&d, &sampled, NULL, &sampled_image) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !sampled_image && calls == before + 3);
        sampled.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        sampled.format = VK_FORMAT_D32_SFLOAT;
        assert(vkCreateImage(&d, &sampled, NULL, &sampled_image) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !sampled_image && calls == before + 3);
        /* 8x is outside the envelope the platform may serve at all. */
        sampled.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sampled.format = info.format;
        sampled.samples = VK_SAMPLE_COUNT_8_BIT;
        assert(vkCreateImage(&d, &sampled, NULL, &sampled_image) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !sampled_image && calls == before + 3);
        d.platform_features &= ~(uint32_t)PS5VK_FEATURE_SAMPLE_RATE_SHADING;
        /* The one accepted creation above reached the backend; the counters the
         * rest of this test pins are restored to what they were before it. */
        calls = before;
    }
    VkSubresourceLayout layout = {.rowPitch = 1234, .size = 5678};
    VkImageSubresource img_sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    vkGetImageSubresourceLayout(&d, image, &img_sub, &layout);
    /* In ps5vk all images are optimal-tiling; linear layout is never fabricated */
    assert(layout.rowPitch == 0 && layout.size == 0 && layout.offset == 0);
    vkGetImageSubresourceLayout(&d, image, &img_sub, NULL); /* no crash */
    layout = (VkSubresourceLayout){.rowPitch = 1234};
    vkGetImageSubresourceLayout(NULL, image, &img_sub, &layout);
    assert(layout.rowPitch == 0);
    layout = (VkSubresourceLayout){.rowPitch = 1234};
    vkGetImageSubresourceLayout(&d, NULL, &img_sub, &layout);
    assert(layout.rowPitch == 0);
    layout = (VkSubresourceLayout){.rowPitch = 1234};
    vkGetImageSubresourceLayout(&d, image, NULL, &layout);
    assert(layout.rowPitch == 0);
    struct VkDevice_T other_d = {0};
    layout = (VkSubresourceLayout){.rowPitch = 1234};
    vkGetImageSubresourceLayout(&other_d, image, &img_sub, &layout);
    assert(layout.rowPitch == 0);
    VkMemoryAllocateInfo ai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=8192};
    VkDeviceMemory m; assert(vkAllocateMemory(&d, &ai, NULL, &m) == VK_SUCCESS);
    assert(vkBindImageMemory(&d, image, m, 1) == VK_ERROR_UNKNOWN);
    assert(vkBindImageMemory(&d, image, m, 4352) == VK_ERROR_UNKNOWN);
    assert(vkBindImageMemory(&d, image, m, 256) == VK_SUCCESS);
    void *mapped, *resolved; VkDeviceSize span;
    assert(vkMapMemory(&d, m, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS);
    assert(ps5vk_image_span(&d, image, &resolved, &span) == VK_SUCCESS);
    assert(resolved == (unsigned char *)mapped + 256 && span == 4096);
    vkUnmapMemory(&d, m);
    assert(vkBindImageMemory(&d, image, m, 0) == VK_ERROR_UNKNOWN);
    VkImageViewCreateInfo vi = {.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=image, .viewType=VK_IMAGE_VIEW_TYPE_2D, .format=info.format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkImageView view;
    assert(vkCreateImageView(&d, &vi, NULL, &view) == VK_SUCCESS);
    assert(view->range.levelCount == 1 && view->range.layerCount == 1 && image->views == 1);
    VkAttachmentDescription attachment = {.format=info.format, .samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1, .pColorAttachments=&color};
    VkRenderPassCreateInfo ri = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=1, .pAttachments=&attachment, .subpassCount=1, .pSubpasses=&sub};
    VkRenderPass pass; assert(vkCreateRenderPass(&d, &ri, NULL, &pass) == VK_SUCCESS);
    VkFramebufferCreateInfo fi = {.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=pass, .attachmentCount=1, .pAttachments=&view, .width=18, .height=19, .layers=1};
    VkFramebuffer fb;
    assert(vkCreateFramebuffer(&d, &fi, NULL, &fb) == VK_ERROR_UNKNOWN && !fb);
    fi.width=17; assert(vkCreateFramebuffer(&d, &fi, NULL, &fb) == VK_SUCCESS);
    assert(ps5vk_framebuffer_compatible(fb, pass));
    pass->attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    assert(ps5vk_framebuffer_compatible(fb, pass));
    pass->attachments[0].format=VK_FORMAT_D32_SFLOAT;
    assert(!ps5vk_framebuffer_compatible(fb, pass));
    vkDestroyRenderPass(&d, pass, NULL); /* Framebuffer retains no pass pointer. */
    vkDestroyImageView(&d, view, NULL); assert(view->framebuffers == 1 && d.lifetime_errors == 1);
    fb->pending=1; vkDestroyFramebuffer(&d, fb, NULL); assert(view->framebuffers == 1);
    fb->pending=0; vkDestroyFramebuffer(&d, fb, NULL); assert(!view->framebuffers);
    d.lifetime_errors=0;
    vkDestroyImage(&d, image, NULL); assert(d.lifetime_errors == 1 && d.images == image);
    view->pending=1; vkDestroyImageView(&d, view, NULL); assert(d.lifetime_errors == 2);
    view->pending=0; vkDestroyImageView(&d, view, NULL); assert(!image->views);
    vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    assert(vkCreateImageView(&d, &vi, NULL, &view) == VK_ERROR_UNKNOWN && !view);
    vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel=1;
    assert(vkCreateImageView(&d, &vi, NULL, &view) == VK_ERROR_UNKNOWN);
    d.lifetime_errors=0;
    image->pending=1; vkFreeMemory(&d, m, NULL); assert(image->memory == m && d.lifetime_errors == 1);
    image->pending=0; vkFreeMemory(&d, m, NULL); assert(!image->memory);
    assert(ps5vk_image_span(&d, image, &resolved, &span) == VK_ERROR_UNKNOWN && !resolved && !span);
    vkDestroyImage(&d, image, NULL); assert(!d.images && !d.graphics_objects && !d.memories);
    info.mipLevels=32;
    assert(vkCreateImage(&d, &info, NULL, &image) == VK_ERROR_UNKNOWN && calls == 2);
    puts("Image layout delegation, binding and lifetime: host backend only");
}
