#include "texture_copy.h"
#include "vk_image.h"
#include <assert.h>
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
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={64,4,1},.arrayLayers=3}};
    VkBufferImageCopy layers={.bufferOffset=16,.bufferRowLength=66,.bufferImageHeight=6,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,1,2},
        .imageOffset={1,1,0},.imageExtent={3,2,1}};
    assert(ps5vk_texture_copy_plan_for_image(&array,4096,3072,&layers,&p)==VK_SUCCESS);
    assert(p.source_offset==16 && p.destination_offset==1284 && p.source_pitch==264 &&
        p.destination_pitch==256 && p.row_bytes==12 && p.rows==2 &&
        p.source_slice_pitch==1584 && p.destination_slice_pitch==1024 && p.slices==2);
    struct VkImage_T volume={.info={.imageType=VK_IMAGE_TYPE_3D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={64,4,3},.arrayLayers=1}};
    layers.imageSubresource.baseArrayLayer=0;layers.imageSubresource.layerCount=1;
    layers.imageOffset.z=1;layers.imageExtent.depth=2;
    assert(ps5vk_texture_copy_plan_for_image(&volume,4096,3072,&layers,&p)==VK_SUCCESS &&
        p.destination_offset==1284 && p.slices==2);
}
