#include "image_layout_state.h"
static int supported(VkImageLayout l)
{
    return l==VK_IMAGE_LAYOUT_GENERAL || l==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        l==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        l==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
        l==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
        l==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}
VkResult ps5vk_layout_require(const struct ps5vk_layout_state *s,VkImage image,VkImageLayout expected)
{
    if(!s || !image || !supported(expected) || s->count>PS5VK_LAYOUT_IMAGES)return VK_ERROR_UNKNOWN;
    VkImageLayout current=image->layout;
    for(unsigned i=0;i<s->count;++i)if(s->entries[i].image==image) {
        current=s->entries[i].current;break;
    }
    return current==expected?VK_SUCCESS:VK_ERROR_UNKNOWN;
}
VkResult ps5vk_layout_transition(struct ps5vk_layout_state *s,VkImage image,VkImageLayout old,VkImageLayout next)
{
    if(!s || !image || !supported(next) ||
        (!supported(old) && old!=VK_IMAGE_LAYOUT_UNDEFINED) || s->count>PS5VK_LAYOUT_IMAGES)
        return VK_ERROR_UNKNOWN;
    unsigned i=0;
    while(i<s->count && s->entries[i].image!=image)++i;
    if(i==PS5VK_LAYOUT_IMAGES)return VK_ERROR_TOO_MANY_OBJECTS;
    VkImageLayout current=i<s->count?s->entries[i].current:image->layout;
    if(old!=VK_IMAGE_LAYOUT_UNDEFINED && current!=old)return VK_ERROR_UNKNOWN;
    if(i==s->count)s->entries[s->count++]=(struct ps5vk_layout_entry){image,image->layout,current};
    s->entries[i].current=next;
    return VK_SUCCESS;
}
VkResult ps5vk_layout_commit(struct ps5vk_layout_state *s)
{
    if(!s || s->count>PS5VK_LAYOUT_IMAGES)return VK_ERROR_UNKNOWN;
    for(unsigned i=0;i<s->count;++i)
        if(!s->entries[i].image || s->entries[i].image->layout!=s->entries[i].initial)
            return VK_ERROR_UNKNOWN;
    for(unsigned i=0;i<s->count;++i) {
        VkImage image=s->entries[i].image;
        /* Native transactions describe whole images. Keep the committed
         * subresource table coherent even when this role normally routes its
         * barriers through the frontend executor. */
        if(image->subresource_layouts) {
            size_t count=(size_t)image->info.mipLevels*image->info.arrayLayers;
            for(size_t j=0;j<count;++j)
                image->subresource_layouts[j]=s->entries[i].current;
        }
        image->layout=s->entries[i].current;
    }
    s->count=0;
    return VK_SUCCESS;
}
