#include "image_layout_state.h"
#include <assert.h>
int main(void)
{
    struct VkImage_T image={0},other={0};struct ps5vk_layout_state s={0};
    assert(ps5vk_layout_require(&s,&image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)!=VK_SUCCESS);
    assert(ps5vk_layout_transition(&s,&image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)==VK_SUCCESS);
    assert(image.layout==VK_IMAGE_LAYOUT_UNDEFINED && s.count==1);
    assert(ps5vk_layout_require(&s,&image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_transition(&s,&image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)!=VK_SUCCESS);
    assert(s.count==1 && s.entries[0].current==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(ps5vk_layout_transition(&s,&image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_transition(&s,&other,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL)==VK_SUCCESS);
    /* Preparation rollback consists of discarding this local transaction. */
    struct ps5vk_layout_state discard=s;(void)discard;
    assert(image.layout==VK_IMAGE_LAYOUT_UNDEFINED && other.layout==VK_IMAGE_LAYOUT_UNDEFINED);
    other.layout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    assert(ps5vk_layout_commit(&s)!=VK_SUCCESS && image.layout==VK_IMAGE_LAYOUT_UNDEFINED && s.count==2);
    other.layout=VK_IMAGE_LAYOUT_UNDEFINED;
    assert(ps5vk_layout_commit(&s)==VK_SUCCESS && !s.count);
    assert(image.layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && other.layout==VK_IMAGE_LAYOUT_GENERAL);
    assert(ps5vk_layout_require(&s,&image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_transition(&s,&image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_UNDEFINED)!=VK_SUCCESS);
    struct VkImage_T many[PS5VK_LAYOUT_IMAGES+1]={0};
    for(unsigned i=0;i<PS5VK_LAYOUT_IMAGES;++i)
        assert(ps5vk_layout_transition(&s,&many[i],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL)==VK_SUCCESS);
    assert(ps5vk_layout_transition(&s,&many[PS5VK_LAYOUT_IMAGES],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL)!=VK_SUCCESS);
    assert(s.count==PS5VK_LAYOUT_IMAGES);
    /* A discard transition remains valid even when a prior layout exists. */
    assert(ps5vk_layout_transition(&s,&many[0],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)==VK_SUCCESS);
    /* Render-pass target layouts are pending until completion, just like the
     * texture upload transaction. Failed/stale commits change neither target. */
    struct VkImage_T color={0},depth={0};struct ps5vk_layout_state targets={0};
    assert(ps5vk_layout_transition(&targets,&color,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_transition(&targets,&depth,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    assert(color.layout==VK_IMAGE_LAYOUT_UNDEFINED && depth.layout==VK_IMAGE_LAYOUT_UNDEFINED);
    assert(ps5vk_layout_require(&targets,&color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_require(&targets,&depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    depth.layout=VK_IMAGE_LAYOUT_GENERAL;
    assert(ps5vk_layout_commit(&targets)!=VK_SUCCESS && color.layout==VK_IMAGE_LAYOUT_UNDEFINED);
    depth.layout=VK_IMAGE_LAYOUT_UNDEFINED;
    assert(ps5vk_layout_commit(&targets)==VK_SUCCESS);
    assert(color.layout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && depth.layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    assert(ps5vk_layout_transition(&targets,&color,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_commit(&targets)==VK_SUCCESS);
    assert(ps5vk_layout_transition(&targets,&color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)!=VK_SUCCESS);
    /* Whole-image native commits keep optional subresource state coherent. */
    VkImageLayout cells[6]={0};
    struct VkImage_T layered={.info={.mipLevels=2,.arrayLayers=3},.subresource_layouts=cells};
    struct ps5vk_layout_state upload={0};
    assert(ps5vk_layout_transition(&upload,&layered,VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_commit(&upload)==VK_SUCCESS);
    for(unsigned i=0;i<6;++i)assert(cells[i]==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange selected={VK_IMAGE_ASPECT_COLOR_BIT,1,1,2,1};
    assert(ps5vk_image_layout_transition(&layered,&selected,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
    assert(ps5vk_layout_require(&upload,&layered,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)!=VK_SUCCESS);
    assert(ps5vk_layout_transition(&upload,&layered,VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_commit(&upload)==VK_SUCCESS);
    for(unsigned i=0;i<6;++i)assert(cells[i]==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
