#include "vk_render_pass.h"
#include "attachment_ops.h"
#include <assert.h>
#include <stdio.h>
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
        assert(!s->color.attachment && s->depth.attachment == 1 && !s->preserve_count);
    }
    assert(pass->attachments[0].format == VK_FORMAT_B8G8R8A8_UNORM &&
           !pass->dependencies[0].srcSubpass && pass->dependencies[0].dstSubpass == 1);
    vkDestroyRenderPass(d, pass, NULL);
    color.attachment = 0; depth.attachment = 1; between.srcSubpass = 0;
    attachments[0].format = VK_FORMAT_B8G8R8A8_UNORM;

    /* A preserve list is validated and owned, and it may only name an
     * attachment the subpass does not otherwise use. */
    uint32_t preserved = 1;
    subpasses[0].pDepthStencilAttachment = NULL;
    subpasses[0].preserveAttachmentCount = 1;
    subpasses[0].pPreserveAttachments = &preserved;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_SUCCESS);
    preserved = 0;
    assert(ps5vk_render_pass_subpass(pass, 0)->preserve_count == 1 &&
           ps5vk_render_pass_subpass(pass, 0)->preserve[0] == 1 &&
           !ps5vk_render_pass_subpass(pass, 1)->preserve_count);
    vkDestroyRenderPass(d, pass, NULL);
    preserved = 1;

    /* Refused shapes. Each leaves no object and no accounting behind. */
    const unsigned objects = d->graphics_objects;
    /* preserve naming an attachment the same subpass uses */
    preserved = 0;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_UNKNOWN && !pass);
    /* preserve out of range */
    preserved = 2;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_UNKNOWN);
    preserved = 1;
    subpasses[0].preserveAttachmentCount = 0;
    subpasses[0].pPreserveAttachments = NULL;
    subpasses[0].pDepthStencilAttachment = &depth;

    /* more subpasses than the profile executes */
    VkSubpassDescription three[3] = {subpasses[0], subpasses[1], subpasses[0]};
    info.subpassCount = 3; info.pSubpasses = three;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    info.subpassCount = 0;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    info.subpassCount = 2; info.pSubpasses = subpasses;

    /* an input or resolve attachment is refused rather than ignored */
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

    /* one attachment cannot be colour in one subpass and depth in another */
    VkAttachmentReference swapped_color = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference swapped_depth = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    subpasses[1].pColorAttachments = &swapped_color;
    subpasses[1].pDepthStencilAttachment = &swapped_depth;
    assert(vkCreateRenderPass(d, &info, NULL, &pass) == VK_ERROR_UNKNOWN);
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
    puts("Render pass owned subpass/attachment/dependency data: host only; no execution");
}
