#include "vk_command.h"
#include "texture_copy.h"
static void invalid(VkCommandBuffer c)
{
    if(c){++c->pool->device->lifetime_errors;if(c->state!=PS5VK_PENDING)c->state=PS5VK_INVALID;}
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer c,VkBuffer source,VkImage image,
    VkImageLayout layout,uint32_t count,const VkBufferImageCopy *regions)
{
    if(!c || c->state!=PS5VK_RECORDING || c->render_pass || !count || !regions ||
        count>PS5VK_MAX_OPERATIONS-c->operation_count || !image || image->device!=c->pool->device ||
        (layout!=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && layout!=VK_IMAGE_LAYOUT_GENERAL)) {invalid(c);return;}
    VkDevice d=c->pool->device;
    if(!ps5vk_buffer_usage(d,source,VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        image->info.format!=VK_FORMAT_R8G8B8A8_UNORM || image->info.mipLevels!=1 ||
        !(image->info.usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT) || !(image->info.usage&VK_IMAGE_USAGE_SAMPLED_BIT) ||
        (image->info.usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {invalid(c);return;}
    void *src,*dst;VkDeviceSize src_bytes,dst_bytes;
    if(ps5vk_buffer_span(d,source,0,VK_WHOLE_SIZE,&src,&src_bytes)!=VK_SUCCESS ||
        ps5vk_image_span(d,image,&dst,&dst_bytes)!=VK_SUCCESS) {invalid(c);return;}
    /* Validate the whole call before appending any operation. Region structs
     * are copied; later application mutations cannot change recorded work. */
    for(uint32_t i=0;i<count;++i) {
        struct ps5vk_texture_copy plan;
        if(ps5vk_texture_copy_plan(image->info.extent.width,image->info.extent.height,
            src_bytes,dst_bytes,&regions[i],&plan)!=VK_SUCCESS) {invalid(c);return;}
    }
    for(uint32_t i=0;i<count;++i)c->operations[c->operation_count++]=(struct ps5vk_operation){
        .type=PS5VK_COPY_BUFFER_IMAGE,.copy_source=source,.copy_image=image,.copy_layout=layout,.copy_region=regions[i]};
}
