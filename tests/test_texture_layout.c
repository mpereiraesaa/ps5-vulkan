#include "texture_layout.h"
#include "vk_internal.h"
#include <assert.h>
int main(void)
{
    struct ps5vk_texture_layout l={0};
    assert(!ps5vk_texture_layout(1,1,&l) && l.row_pitch==256 && l.bytes==256 && l.alignment==256);
    assert(!ps5vk_texture_layout(65,3,&l) && l.row_pitch==512 && l.bytes==1536);
    assert(!ps5vk_texture_layout(16384,16384,&l) && l.bytes==UINT64_C(1073741824));
    assert(ps5vk_texture_layout(0,3,&l) && ps5vk_texture_layout(UINT32_MAX,3,&l));
    assert(ps5vk_texture_layout(1,16385,&l) && ps5vk_texture_layout(1,1,NULL));
    assert(!ps5vk_texture_layout_for_format(VK_FORMAT_R8_UNORM,257,2,&l) &&
        l.row_pitch==512 && l.bytes==1024);
    assert(!ps5vk_texture_layout_for_format(VK_FORMAT_R8G8_UNORM,129,2,&l) &&
        l.row_pitch==512 && l.bytes==1024);
    assert(!ps5vk_texture_layout_for_format(VK_FORMAT_R8G8B8A8_SRGB,65,2,&l) &&
        l.row_pitch==512 && l.bytes==1024);
    assert(ps5vk_texture_layout_for_format(VK_FORMAT_B8G8R8A8_UNORM,1,1,&l));
    VkImageCreateInfo i={.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
        .extent={65,3,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkMemoryRequirements r;
    assert(ps5vk_native_image_requirements(NULL,&i,&r)==VK_SUCCESS && r.size==1536 && r.alignment==256);
    i.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    assert(ps5vk_native_image_requirements(NULL,&i,&r)==VK_SUCCESS && r.size==1536 && r.alignment==256);
    i.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    i.format=VK_FORMAT_B8G8R8A8_UNORM;
    assert(ps5vk_native_image_requirements(NULL,&i,&r)==VK_ERROR_FORMAT_NOT_SUPPORTED);
    i.format=VK_FORMAT_R8G8B8A8_UNORM;i.usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    assert(ps5vk_native_image_requirements(NULL,&i,&r)==VK_ERROR_FORMAT_NOT_SUPPORTED && !r.size);
    i.mipLevels=2;assert(ps5vk_native_image_requirements(NULL,&i,&r)==VK_ERROR_FORMAT_NOT_SUPPORTED);
}
