#include "texture_layout.h"
#include "vk_internal.h"
#include <assert.h>
#include <string.h>
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
    /* The pinned cube-array image-view case's exact sampled/attachment
     * combination is backed as a tiled layered target. Neighbouring shapes
     * remain outside the executor profile. */
    i=(VkImageCreateInfo){.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={64,64,1},.mipLevels=1,
        .arrayLayers=12,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT};
    assert(ps5vk_native_image_requirements(NULL,&i,&r)==VK_SUCCESS &&
        r.size && r.alignment==131072);
    i.flags=0;
    assert(ps5vk_native_image_requirements(NULL,&i,&r)==
        VK_ERROR_FORMAT_NOT_SUPPORTED && !r.size);
    i.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    i.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    assert(ps5vk_native_image_requirements(NULL,&i,&r)==
        VK_ERROR_FORMAT_NOT_SUPPORTED && !r.size);

    i.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    i.extent=(VkExtent3D){4,4,1};
    i.arrayLayers=13;
    assert(ps5vk_native_image_requirements(NULL,&i,&r)==VK_SUCCESS && r.size==13312);
    i.arrayLayers=5;
    assert(ps5vk_native_image_requirements(NULL,&i,&r)!=VK_SUCCESS && !r.size);

    /* Only formats with a sampled encoding have this generic arithmetic.
     * D32 has one for descriptors, though image creation uses tiled storage. */
    assert(ps5vk_texture_layout_for_format(VK_FORMAT_B8G8R8A8_UNORM,4,4,&l));
    assert(!ps5vk_texture_layout_for_format(VK_FORMAT_D32_SFLOAT,4,4,&l));
    assert(ps5vk_texture_layout_for_format(VK_FORMAT_R32G32B32_SFLOAT,4,4,&l));
    assert(ps5vk_texture_layout_for_format(VK_FORMAT_A2B10G10R10_UNORM_PACK32,4,4,&l));
    assert(ps5vk_texture_layout_for_format(VK_FORMAT_UNDEFINED,4,4,&l));

    /* The packed sampled rows share the same arithmetic: the packed
     * A8B8G8R8 order is byte-identical to R8G8B8A8, so the layout matches the
     * witnessed row exactly. */
    struct ps5vk_texture_mip_layout rgba8={0}, packed={0};
    assert(!ps5vk_texture_mip_layout_for_slices(VK_FORMAT_R8G8B8A8_UNORM,
        65,3,2,3,&rgba8));
    assert(!ps5vk_texture_mip_layout_for_slices(VK_FORMAT_A8B8G8R8_UNORM_PACK32,
        65,3,2,3,&packed));
    assert(rgba8.bytes==packed.bytes && rgba8.layer_stride==packed.layer_stride &&
        rgba8.levels[0].row_pitch==packed.levels[0].row_pitch &&
        rgba8.levels[2].storage_width==packed.levels[2].storage_width);
    struct ps5vk_texture_mip_layout srgb_packed={0};
    assert(!ps5vk_texture_mip_layout_for_slices(VK_FORMAT_A8B8G8R8_SRGB_PACK32,
        65,3,1,3,&srgb_packed));
    assert(srgb_packed.bytes==rgba8.layer_stride);

    /* BC data is stored in 4x4 blocks. Odd extents round up in block space,
     * then each block row receives the same 256-byte hardware alignment. */
    struct ps5vk_texture_mip_layout bc={0};
    assert(!ps5vk_texture_mip_layout_for_slices(VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        5,5,2,1,&bc));
    assert(bc.level_count==1 && bc.storage_layers==2 && bc.layer_stride==512 &&
        bc.bytes==1024 && bc.levels[0].row_pitch==256 &&
        bc.levels[0].storage_width==5 && bc.levels[0].storage_height==5);
    assert(!ps5vk_texture_mip_layout_for_slices(VK_FORMAT_BC7_UNORM_BLOCK,
        5,5,1,3,&bc));
    /* Descending mips 2x2, 3x3, 5x5 use one, one and two 16-byte block rows. */
    assert(bc.layer_stride==1024 && bc.bytes==1024 && bc.levels[0].offset==512 &&
        bc.levels[1].offset==256 && bc.levels[2].offset==0 &&
        bc.levels[0].row_pitch==256 && bc.levels[0].storage_width==5);

    /* Overflow and bound rejections must not mutate the caller's structure.
     * 16384x16384 at 16 bytes per texel is 2^32 bytes per level, so two levels
     * plus UINT32_MAX layers cannot be represented and must be refused rather
     * than wrapped. */
    struct ps5vk_texture_mip_layout sentinel;
    memset(&sentinel,0x5a,sizeof(sentinel));
    struct ps5vk_texture_mip_layout guard=sentinel;
    assert(ps5vk_texture_mip_layout_for_slices(VK_FORMAT_R32G32B32A32_SFLOAT,
        16384,16384,UINT32_MAX,2,&guard) && !memcmp(&guard,&sentinel,sizeof(guard)));
    guard=sentinel;
    assert(ps5vk_texture_mip_layout_for_slices(VK_FORMAT_R8G8B8A8_UNORM,
        16385,4,1,1,&guard) && !memcmp(&guard,&sentinel,sizeof(guard)));
    guard=sentinel;
    assert(ps5vk_texture_mip_layout_for_slices(VK_FORMAT_R8G8B8A8_UNORM,
        4,16385,1,1,&guard) && !memcmp(&guard,&sentinel,sizeof(guard)));
    guard=sentinel;
    assert(ps5vk_texture_mip_layout_for_slices(VK_FORMAT_R8G8B8A8_UNORM,
        4,4,1,17,&guard) && !memcmp(&guard,&sentinel,sizeof(guard)));
    guard=sentinel;
    assert(ps5vk_texture_mip_layout_for_slices(VK_FORMAT_R8G8B8A8_UNORM,
        4,4,1,0,&guard) && !memcmp(&guard,&sentinel,sizeof(guard)));
    guard=sentinel;
    assert(ps5vk_texture_mip_layout_for_slices(VK_FORMAT_R8G8B8A8_UNORM,
        4,4,0,1,&guard) && !memcmp(&guard,&sentinel,sizeof(guard)));
    guard=sentinel;
    assert(ps5vk_texture_mip_layout_for_slices(VK_FORMAT_R8G8B8A8_UNORM,
        0,4,1,1,&guard) && !memcmp(&guard,&sentinel,sizeof(guard)));
    assert(ps5vk_texture_mip_layout_for_slices(VK_FORMAT_R8G8B8A8_UNORM,
        4,4,1,1,NULL));
    assert(ps5vk_texture_layout_for_format(VK_FORMAT_R8_UNORM,1,1,NULL));
    assert(ps5vk_texture_layout(1,1,NULL));
}
