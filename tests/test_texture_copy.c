#include "texture_copy.h"
#include "vk_image.h"
#include <assert.h>
#include <string.h>
int main(void)
{
    VkBufferImageCopy r={.bufferOffset=16,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageOffset={1,1,0},.imageExtent={3,2,1}};
    struct ps5vk_texture_copy p={0};
    assert(ps5vk_texture_copy_plan(65,4,40,2048,&r,&p)==VK_SUCCESS);
    assert(p.source_offset==16 && p.destination_offset==516 && p.source_pitch==12 && p.destination_pitch==512 && p.row_bytes==12 && p.rows==2);
    VkBufferImageCopy narrow=r;narrow.bufferOffset=3;
    assert(ps5vk_texture_copy_plan_for_format(VK_FORMAT_R8_UNORM,257,4,9,2048,
        &narrow,&p)==VK_SUCCESS);
    assert(p.source_offset==3 && p.destination_offset==513 && p.source_pitch==3 &&
        p.destination_pitch==512 && p.row_bytes==3 && p.rows==2);
    narrow.bufferOffset=4;
    assert(ps5vk_texture_copy_plan_for_format(VK_FORMAT_R8G8_UNORM,129,4,16,2048,
        &narrow,&p)==VK_SUCCESS);
    assert(p.destination_offset==514 && p.source_pitch==6 && p.row_bytes==6);
    assert(ps5vk_texture_copy_plan_for_format(VK_FORMAT_B8G8R8A8_UNORM,129,4,16,2048,
        &narrow,&p)!=VK_SUCCESS);
    r.bufferRowLength=8;r.bufferImageHeight=3;
    assert(ps5vk_texture_copy_plan(65,4,60,2048,&r,&p)==VK_SUCCESS && p.source_pitch==32);
    assert(ps5vk_texture_copy_plan(65,4,59,2048,&r,&p)!=VK_SUCCESS && p.source_pitch==32);
    assert(ps5vk_texture_copy_plan(65,4,60,2047,&r,&p)!=VK_SUCCESS);
#define BAD(field,value) do { VkBufferImageCopy b=r;b.field=value; \
    assert(ps5vk_texture_copy_plan(65,4,60,2048,&b,&p)!=VK_SUCCESS); } while(0)
    BAD(bufferOffset,UINT64_MAX-3);BAD(bufferOffset,1);BAD(bufferRowLength,2);
    BAD(bufferImageHeight,1);BAD(imageOffset.x,-1);BAD(imageOffset.y,3);
    BAD(imageExtent.width,UINT32_MAX);BAD(imageExtent.height,0);BAD(imageExtent.depth,2);
    BAD(imageSubresource.layerCount,2);BAD(imageSubresource.mipLevel,1);
    BAD(imageSubresource.aspectMask,VK_IMAGE_ASPECT_DEPTH_BIT);
#undef BAD
    struct VkImage_T array={.info={.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={64,4,1},.mipLevels=1,.arrayLayers=3}};
    VkBufferImageCopy layers={.bufferOffset=16,.bufferRowLength=66,.bufferImageHeight=6,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,1,2},
        .imageOffset={1,1,0},.imageExtent={3,2,1}};
    assert(ps5vk_texture_copy_plan_for_image(&array,4096,3072,&layers,&p)==VK_SUCCESS);
    assert(p.source_offset==16 && p.destination_offset==1284 && p.source_pitch==264 &&
        p.destination_pitch==256 && p.row_bytes==12 && p.rows==2 &&
        p.source_slice_pitch==1584 && p.destination_slice_pitch==1024 && p.slices==2);
    struct VkImage_T volume={.info={.imageType=VK_IMAGE_TYPE_3D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={64,4,3},.mipLevels=1,.arrayLayers=1}};
    layers.imageSubresource.baseArrayLayer=0;layers.imageSubresource.layerCount=1;
    layers.imageOffset.z=1;layers.imageExtent.depth=2;
    assert(ps5vk_texture_copy_plan_for_image(&volume,4096,3072,&layers,&p)==VK_SUCCESS &&
        p.destination_offset==1284 && p.slices==2);
    struct VkImage_T mip_image={.info={.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={65,3,1},.mipLevels=3,.arrayLayers=2}};
    VkBufferImageCopy mip={.bufferOffset=0,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,2,1,1},
        .imageExtent={16,1,1}};
    assert(ps5vk_texture_copy_plan_for_image(&mip_image,64,4608,&mip,&p)==VK_SUCCESS);
    assert(p.destination_offset==2304 && p.destination_pitch==256 &&
        p.destination_slice_pitch==2304 && p.row_bytes==64);
    mip.imageSubresource.mipLevel=1;mip.imageExtent=(VkExtent3D){32,1,1};
    assert(ps5vk_texture_copy_plan_for_image(&mip_image,128,4608,&mip,&p)==VK_SUCCESS &&
        p.destination_offset==256+2304 && p.destination_pitch==256);
    mip.imageExtent.width=33;
    assert(ps5vk_texture_copy_plan_for_image(&mip_image,132,4608,&mip,&p)!=VK_SUCCESS);

    /* Only formats with an implemented padded-linear encoding have a copy
     * plan; the colour attachment, the depth target, the three-component
     * vertex rows and the packed 10-bit row have no image encoding. */
    VkBufferImageCopy single={.bufferOffset=0,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={2,2,1}};
    assert(ps5vk_texture_copy_plan_for_format(VK_FORMAT_R32G32B32_SFLOAT,4,4,
        UINT64_C(64),UINT64_C(64),&single,&p)!=VK_SUCCESS);
    assert(ps5vk_texture_copy_plan_for_format(VK_FORMAT_D32_SFLOAT,4,4,
        UINT64_C(64),UINT64_C(64),&single,&p)!=VK_SUCCESS);
    assert(ps5vk_texture_copy_plan_for_format(VK_FORMAT_A2B10G10R10_UNORM_PACK32,4,4,
        UINT64_C(64),UINT64_C(64),&single,&p)!=VK_SUCCESS);

    /* A packed sampled row has the same plan as its byte-identical unpacked
     * counterpart, so promotion changes only the published capability. */
    VkBufferImageCopy sample={.bufferOffset=16,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageOffset={1,1,0},.imageExtent={3,2,1}};
    struct ps5vk_texture_copy rgba8={0},packed={0};
    assert(ps5vk_texture_copy_plan_for_format(VK_FORMAT_R8G8B8A8_UNORM,65,4,40,2048,
        &sample,&rgba8)==VK_SUCCESS);
    assert(ps5vk_texture_copy_plan_for_format(VK_FORMAT_A8B8G8R8_UNORM_PACK32,65,4,40,
        2048,&sample,&packed)==VK_SUCCESS);
    assert(!memcmp(&rgba8,&packed,sizeof(rgba8)));
    assert(ps5vk_texture_copy_plan_for_format(VK_FORMAT_A8B8G8R8_SRGB_PACK32,65,4,40,
        2048,&sample,&packed)==VK_SUCCESS && !memcmp(&rgba8,&packed,sizeof(rgba8)));

    /* An unbounded bufferRowLength/bufferImageHeight pair would overflow the
     * 64-bit source span; it must be refused with the output untouched. */
    VkBufferImageCopy unbounded={.bufferOffset=0,.bufferRowLength=UINT32_MAX,
        .bufferImageHeight=UINT32_MAX,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={1,1,1}};
    struct ps5vk_texture_copy sentinel;
    memset(&sentinel,0x5a,sizeof(sentinel));
    struct ps5vk_texture_copy guard=sentinel;
    assert(ps5vk_texture_copy_plan_for_format(VK_FORMAT_R32G32B32A32_SFLOAT,16384,16384,
        UINT64_MAX,UINT64_MAX,&unbounded,&guard)!=VK_SUCCESS &&
        !memcmp(&guard,&sentinel,sizeof(guard)));
    /* The same geometry at the largest representable row is still refused
     * without touching the caller's structure. */
    VkBufferImageCopy widest={.bufferOffset=0,.bufferRowLength=UINT32_MAX,
        .bufferImageHeight=UINT32_MAX,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={16384,16384,1}};
    guard=sentinel;
    assert(ps5vk_texture_copy_plan_for_format(VK_FORMAT_R32G32B32A32_SFLOAT,16384,16384,
        UINT64_MAX,UINT64_MAX,&widest,&guard)!=VK_SUCCESS &&
        !memcmp(&guard,&sentinel,sizeof(guard)));
}
