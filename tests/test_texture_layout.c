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
        l.row_pitch==512 && l.slice_pitch==1024 && l.bytes==1024);
    assert(!ps5vk_texture_layout_for_slices(VK_FORMAT_R8G8B8A8_UNORM,64,3,6,&l) &&
        l.row_pitch==256 && l.slice_pitch==768 && l.bytes==4608);
    struct ps5vk_texture_mip_layout m={0};
    assert(!ps5vk_texture_mip_layout_for_slices(VK_FORMAT_R8G8B8A8_UNORM,
        65,3,2,3,&m));
    /* Descending-level packing from the pinned GPL ps5-opengl contract:
     * 17x1 (256 B), 33x2 (512 B), 65x3 (1536 B), then repeat per layer. */
    assert(m.level_count==3 && m.storage_layers==2 && m.alignment==256 &&
        m.levels[2].offset==0 && m.levels[2].storage_width==17 &&
        m.levels[1].offset==256 && m.levels[1].storage_width==33 &&
        m.levels[0].offset==768 && m.levels[0].row_pitch==512 &&
        m.layer_stride==2304 && m.bytes==4608);
    assert(ps5vk_texture_mip_layout_for_slices(VK_FORMAT_R8G8B8A8_UNORM,
        64,64,1,17,&m));
    assert(ps5vk_texture_layout_for_slices(VK_FORMAT_R8G8B8A8_UNORM,64,3,0,&l));
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
    i.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    i.mipLevels=2;assert(ps5vk_native_image_requirements(NULL,&i,&r)==VK_SUCCESS &&
        r.size==2048 && r.alignment==256);
    i.usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    assert(ps5vk_native_image_requirements(NULL,&i,&r)==VK_ERROR_FORMAT_NOT_SUPPORTED);
}
