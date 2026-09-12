#include "texture_copy.h"
#include <assert.h>
int main(void)
{
    VkBufferImageCopy r={.bufferOffset=16,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageOffset={1,1,0},.imageExtent={3,2,1}};
    struct ps5vk_texture_copy p={0};
    assert(ps5vk_texture_copy_plan(65,4,40,2048,&r,&p)==VK_SUCCESS);
    assert(p.source_offset==16 && p.destination_offset==516 && p.source_pitch==12 && p.destination_pitch==512 && p.row_bytes==12 && p.rows==2);
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
}
