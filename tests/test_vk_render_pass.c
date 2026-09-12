#include "vk_render_pass.h"
#include <assert.h>
#include <stdio.h>
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
    assert(pass->attachments[0].format == VK_FORMAT_B8G8R8A8_UNORM && pass->color.attachment == 0);
    pass->pending=1; vkDestroyRenderPass(&d, pass, NULL);
    assert(d.graphics_objects == 1 && d.lifetime_errors == 1);
    pass->pending=0; vkDestroyRenderPass(&d, pass, NULL); assert(!d.graphics_objects);
    attachments[0].format=VK_FORMAT_B8G8R8A8_UNORM;
    assert(vkCreateRenderPass(&d, &info, NULL, &pass) == VK_ERROR_UNKNOWN && !pass);
    color.attachment=0; attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    assert(vkCreateRenderPass(&d, &info, NULL, &pass) == VK_ERROR_UNKNOWN);
    attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachments[1].samples=VK_SAMPLE_COUNT_4_BIT;
    assert(vkCreateRenderPass(&d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    attachments[1].samples=VK_SAMPLE_COUNT_1_BIT;
    VkSubpassDependency dep={.srcSubpass=VK_SUBPASS_EXTERNAL, .dstSubpass=0,
        .srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT, .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    info.dependencyCount=1; info.pDependencies=&dep;
    assert(vkCreateRenderPass(&d, &info, NULL, &pass) == VK_SUCCESS);
    dep.srcAccessMask=0;
    assert(pass->dependencies[0].srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
    vkDestroyRenderPass(&d, pass, NULL);
    dep.srcSubpass=0;
    assert(vkCreateRenderPass(&d, &info, NULL, &pass) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(!d.graphics_objects);
    puts("Render pass owned attachment/dependency data: host only; graphics backend disabled natively");
}
