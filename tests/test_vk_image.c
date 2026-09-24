#include "vk_image.h"
#include "vk_framebuffer.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static int calls;
static VkResult requirements(VkDevice d, const VkImageCreateInfo *info, VkMemoryRequirements *out)
{
    (void)d; (void)info; ++calls;
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
    VkImageViewMinLodCreateInfoEXT min_lod = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT,
        .minLod = 0.0f,
    };
    vi.pNext = &min_lod;
    VkImageView min_lod_view = VK_NULL_HANDLE;
    assert(vkCreateImageView(&d, &vi, NULL, &min_lod_view) == VK_SUCCESS);
    assert(min_lod_view->image == image && image->views == 2);
    vkDestroyImageView(&d, min_lod_view, NULL);
    assert(image->views == 1);
    min_lod.minLod = 0.5f;
    assert(vkCreateImageView(&d, &vi, NULL, &min_lod_view) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !min_lod_view);
    min_lod.minLod = 0.0f;
    VkImageViewMinLodCreateInfoEXT chained_min_lod = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT,
        .pNext = &min_lod,
        .minLod = 0.0f,
    };
    vi.pNext = &chained_min_lod;
    assert(vkCreateImageView(&d, &vi, NULL, &min_lod_view) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !min_lod_view);
    vi.pNext = NULL;
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

    /* The framebuffer the pinned multisample oracle builds (DXVK262-T06): one
     * multisampled colour attachment, its resolve target and two per-sample
     * targets, so four attachments and four slots. The model used to hold two,
     * which wrote past itself the moment the render pass's own bound followed
     * that shape; this case fails under the sanitizer build if that returns. */
    {
        enum { ORACLE_ATTACHMENTS = 4 };
        /* The counters the rest of this test pins are restored below. */
        const int oracle_calls_before = calls;
        VkImage oracle_images[ORACLE_ATTACHMENTS] = {0};
        VkDeviceMemory oracle_memory[ORACLE_ATTACHMENTS] = {0};
        VkImageView oracle_views[ORACLE_ATTACHMENTS] = {0};
        VkAttachmentDescription attachments[ORACLE_ATTACHMENTS];
        VkAttachmentReference color0 = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference resolve0 = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference input0 = {0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkAttachmentReference color1 = {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference color2 = {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        uint32_t preserve1[1] = {3}, preserve2[1] = {2};
        d.platform_features |= PS5VK_FEATURE_SAMPLE_RATE_SHADING;
        for (unsigned i = 0; i < ORACLE_ATTACHMENTS; ++i) {
            attachments[i] = (VkAttachmentDescription){
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .samples = i == 0 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkImageCreateInfo oi = info;
            oi.extent = (VkExtent3D){17, 17, 1};
            oi.format = VK_FORMAT_R8G8B8A8_UNORM;
            oi.samples = attachments[i].samples;
            /* The multisampled attachment carries exactly the roles the oracle
             * gives it; the single-sample targets carry the readback pair. */
            oi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       (i == 0 ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
                               : VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            assert(vkCreateImage(&d, &oi, NULL, &oracle_images[i]) == VK_SUCCESS);
            VkMemoryRequirements oreq;
            vkGetImageMemoryRequirements(&d, oracle_images[i], &oreq);
            VkMemoryAllocateInfo oa = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = oreq.size};
            assert(vkAllocateMemory(&d, &oa, NULL, &oracle_memory[i]) == VK_SUCCESS);
            assert(vkBindImageMemory(&d, oracle_images[i], oracle_memory[i], 0) == VK_SUCCESS);
            VkImageViewCreateInfo ovi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = oracle_images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            assert(vkCreateImageView(&d, &ovi, NULL, &oracle_views[i]) == VK_SUCCESS);
        }
        const VkSubpassDescription oracle_subpasses[3] = {
            {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
             .colorAttachmentCount = 1, .pColorAttachments = &color0,
             .pResolveAttachments = &resolve0},
            {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
             .inputAttachmentCount = 1, .pInputAttachments = &input0,
             .colorAttachmentCount = 1, .pColorAttachments = &color1,
             .preserveAttachmentCount = 1, .pPreserveAttachments = preserve1},
            {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
             .inputAttachmentCount = 1, .pInputAttachments = &input0,
             .colorAttachmentCount = 1, .pColorAttachments = &color2,
             .preserveAttachmentCount = 1, .pPreserveAttachments = preserve2}};
        VkRenderPassCreateInfo oracle_pass_info = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = ORACLE_ATTACHMENTS, .pAttachments = attachments,
            .subpassCount = 3, .pSubpasses = oracle_subpasses};
        VkRenderPass oracle_pass = VK_NULL_HANDLE;
        assert(vkCreateRenderPass(&d, &oracle_pass_info, NULL, &oracle_pass) == VK_SUCCESS);
        VkFramebufferCreateInfo oracle_fb_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = oracle_pass,
            .attachmentCount = ORACLE_ATTACHMENTS, .pAttachments = oracle_views,
            .width = 17, .height = 17, .layers = 1};
        VkFramebuffer oracle_fb = VK_NULL_HANDLE;
        assert(vkCreateFramebuffer(&d, &oracle_fb_info, NULL, &oracle_fb) == VK_SUCCESS);
        /* Every slot is owned, including the ones past the old two-slot bound. */
        assert(oracle_fb->attachment_count == ORACLE_ATTACHMENTS &&
               oracle_fb->attachments[3] == oracle_views[3] &&
               oracle_fb->formats[3] == VK_FORMAT_R8G8B8A8_UNORM &&
               oracle_fb->samples[3] == VK_SAMPLE_COUNT_1_BIT &&
               oracle_fb->color_count == 1 && oracle_fb->resolve_count == 1 &&
               oracle_fb->resolve_attachments[0] == 1);
        vkDestroyFramebuffer(&d, oracle_fb, NULL);
        vkDestroyRenderPass(&d, oracle_pass, NULL);
        for (unsigned i = 0; i < ORACLE_ATTACHMENTS; ++i) {
            vkDestroyImageView(&d, oracle_views[i], NULL);
            vkDestroyImage(&d, oracle_images[i], NULL);
            vkFreeMemory(&d, oracle_memory[i], NULL);
        }
        d.platform_features &= ~(uint32_t)PS5VK_FEATURE_SAMPLE_RATE_SHADING;
        d.lifetime_errors = 0;
        calls = oracle_calls_before;
    }
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

    /* A cube-compatible image may hold multiple cubes when the layer count is
     * a six-layer multiple. Cube-array views require the feature at device
     * creation and cover a whole number of complete cubes. */
    {
        VkImageCreateInfo cube_array = info;
        cube_array.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        cube_array.extent = (VkExtent3D){17, 17, 1};
        cube_array.mipLevels = 1;
        cube_array.arrayLayers = 12;
        cube_array.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage cube_image = VK_NULL_HANDLE;
        assert(vkCreateImage(&d, &cube_array, NULL, &cube_image) == VK_SUCCESS &&
               cube_image && calls == 3);
        VkImageCreateInfo invalid_cube_array = cube_array;
        invalid_cube_array.arrayLayers = 7;
        VkImage invalid_image = VK_NULL_HANDLE;
        assert(vkCreateImage(&d, &invalid_cube_array, NULL, &invalid_image) ==
               VK_ERROR_UNKNOWN && !invalid_image && calls == 3);
        invalid_cube_array = cube_array;
        invalid_cube_array.samples = VK_SAMPLE_COUNT_4_BIT;
        assert(vkCreateImage(&d, &invalid_cube_array, NULL, &invalid_image) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !invalid_image && calls == 3);
        VkMemoryAllocateInfo cube_ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 8192};
        VkDeviceMemory cube_memory = VK_NULL_HANDLE;
        assert(vkAllocateMemory(&d, &cube_ai, NULL, &cube_memory) == VK_SUCCESS);
        assert(vkBindImageMemory(&d, cube_image, cube_memory, 0) == VK_SUCCESS);
        VkImageViewCreateInfo cube_view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = cube_image, .viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
            .format = cube_array.format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 12}};
        VkImageView cube_view = VK_NULL_HANDLE;
        assert(vkCreateImageView(&d, &cube_view_info, NULL, &cube_view) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !cube_view);
        d.enabled_features |= PS5VK_FEATURE_IMAGE_CUBE_ARRAY;
        assert(vkCreateImageView(&d, &cube_view_info, NULL, &cube_view) == VK_SUCCESS &&
               cube_view->range.baseArrayLayer == 0 && cube_view->range.layerCount == 12);
        vkDestroyImageView(&d, cube_view, NULL);
        cube_view_info.subresourceRange.baseArrayLayer = 6;
        cube_view_info.subresourceRange.layerCount = 6;
        assert(vkCreateImageView(&d, &cube_view_info, NULL, &cube_view) == VK_SUCCESS &&
               cube_view->range.baseArrayLayer == 6 && cube_view->range.layerCount == 6);
        vkDestroyImageView(&d, cube_view, NULL);
        cube_view_info.subresourceRange.baseArrayLayer = 1;
        assert(vkCreateImageView(&d, &cube_view_info, NULL, &cube_view) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !cube_view);
        cube_view_info.subresourceRange.baseArrayLayer = 0;
        cube_view_info.subresourceRange.layerCount = 7;
        assert(vkCreateImageView(&d, &cube_view_info, NULL, &cube_view) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !cube_view);
        cube_view_info.subresourceRange.baseArrayLayer = 6;
        cube_view_info.subresourceRange.layerCount = 12;
        assert(vkCreateImageView(&d, &cube_view_info, NULL, &cube_view) ==
               VK_ERROR_UNKNOWN && !cube_view);
        d.enabled_features &= ~(uint32_t)PS5VK_FEATURE_IMAGE_CUBE_ARRAY;
        vkDestroyImage(&d, cube_image, NULL);
        vkFreeMemory(&d, cube_memory, NULL);
    }
    puts("Image layout delegation, binding and lifetime: host backend only");
}
