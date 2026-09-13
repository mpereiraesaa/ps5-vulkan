#include "texture_descriptor.h"
#include "texture_layout.h"
#include "texture_format.h"
#include <string.h>
VkResult ps5vk_texture_descriptor(VkDevice d,VkImageView view,VkSampler sampler,uint32_t out[12])
{
    if(!d || !view || !sampler || !out || view->device!=d || sampler->device!=d || !view->image)return VK_ERROR_UNKNOWN;
    VkImage image=view->image;
    const struct ps5vk_texture_format *format=ps5vk_texture_format_lookup(view->format);
    if(!format || !ps5vk_texture_format_supported(view->format) || image->info.format!=view->format ||
        view->range.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT || view->range.baseMipLevel ||
        view->range.levelCount!=1 || view->range.baseArrayLayer || view->range.layerCount!=1 ||
        image->info.mipLevels!=1 || !(image->info.usage&VK_IMAGE_USAGE_SAMPLED_BIT) ||
        (image->info.usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_texture_layout layout;
    if(ps5vk_texture_layout_for_format(view->format,image->info.extent.width,image->info.extent.height,&layout))return VK_ERROR_UNKNOWN;
    void *base;VkDeviceSize bytes;
    VkResult rc=ps5vk_image_span(d,image,&base,&bytes);if(rc!=VK_SUCCESS)return rc;
    uint64_t address=(uintptr_t)base,limit=UINT64_C(1)<<48;
    if(!address || address>=limit || address%layout.alignment || bytes<layout.bytes || layout.bytes>limit-address)
        return VK_ERROR_UNKNOWN;
    /* Public Mesa gfx10-rsrc.json descriptor fields plus the pinned
     * ps5-opengl format and identity-swizzle mapping. */
    uint32_t words[12]={0},width=image->info.extent.width-1;
    words[0]=(uint32_t)(address>>8);
    words[1]=(uint32_t)(address>>40)|format->descriptor_format_word|((width&3u)<<30);
    words[2]=(width>>2)|((image->info.extent.height-1)<<14)|(1u<<31);
    words[3]=format->selectors[0]|((uint32_t)format->selectors[1]<<3)|
        ((uint32_t)format->selectors[2]<<6)|((uint32_t)format->selectors[3]<<9)|(9u<<28);
    if(layout.row_pitch/format->bytes_per_texel!=image->info.extent.width)
        words[4]=layout.row_pitch/format->bytes_per_texel-1;
    words[5]=4u<<20;
    memcpy(words+8,sampler->words,16);memcpy(out,words,sizeof(words));return VK_SUCCESS;
}
