#include "readback_commands_ps5.h"
#include <assert.h>
#include <string.h>

static unsigned char source[6*131072],destination[6*64*64*4+32];
static VkResult span_result;
static VkDeviceSize source_size=65536,destination_size=64*64*4;
VkResult ps5vk_image_span(VkDevice d,VkImage i,void **p,VkDeviceSize *n)
{ assert(d && i);*p=source;*n=source_size;return span_result; }
VkResult ps5vk_buffer_span(VkDevice d,VkBuffer b,VkDeviceSize offset,VkDeviceSize size,void **p,VkDeviceSize *n)
{ assert(d && b && !offset && size==VK_WHOLE_SIZE);*p=destination;*n=destination_size;return span_result; }

int main(void)
{
    struct VkDevice_T device={0};
    struct VkImage_T image={.device=&device,.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .info={.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .extent={64,64,1},.mipLevels=1,.arrayLayers=1,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT}};
    struct VkImage_T other=image;
    VkBuffer buffer=(VkBuffer)(uintptr_t)1;
    struct ps5vk_operation ops[4]={
        {.type=PS5VK_IMAGE_BARRIER,.src_stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dst_stage=VK_PIPELINE_STAGE_TRANSFER_BIT,.image_barrier={.image=&image,
         .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT}},
        {.type=PS5VK_COPY_IMAGE_BUFFER,.copy_image=&image,.copy_destination=buffer,
         .copy_layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.copy_region={
         .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={64,64,1}}},
        {.type=PS5VK_BARRIER,.src_stage=VK_PIPELINE_STAGE_TRANSFER_BIT,.dst_stage=VK_PIPELINE_STAGE_HOST_BIT,
         .src_access=VK_ACCESS_TRANSFER_WRITE_BIT,.dst_access=VK_ACCESS_HOST_READ_BIT,
         .buffer_barrier={.buffer=buffer,.size=VK_WHOLE_SIZE}},
        {.type=PS5VK_BARRIER,.src_stage=VK_PIPELINE_STAGE_TRANSFER_BIT,.dst_stage=VK_PIPELINE_STAGE_HOST_BIT}
    };
    struct ps5vk_layout_state layouts={0};struct ps5vk_readback_plan plan={0};
    memset(destination,0xa5,sizeof(destination));
    for(unsigned standalone=0;standalone<2;++standalone) {
        for(unsigned all=0;all<2;++all) {
            image.layout=standalone?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
            layouts=(struct ps5vk_layout_state){0};plan=(struct ps5vk_readback_plan){0};
            if(!standalone)assert(ps5vk_layout_transition(&layouts,&image,VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
            ops[0].src_stage=all?VK_PIPELINE_STAGE_ALL_COMMANDS_BIT:VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            assert(ps5vk_readback_commands(&device,ops,4,standalone?NULL:&image,&layouts,&plan)==VK_SUCCESS);
            assert(plan.image==&image && plan.buffer==buffer);
            assert(image.layout==(standalone?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED));
            assert(ps5vk_layout_require(&layouts,&image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)==VK_SUCCESS);
            for(unsigned i=0;i<sizeof(destination);++i)assert(destination[i]==0xa5);
            assert(ps5vk_layout_commit(&layouts)==VK_SUCCESS);
            assert(image.layout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
    }
    image.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const struct ps5vk_operation saved[4]={ops[0],ops[1],ops[2],ops[3]};
    for(unsigned failure=0;failure<16;++failure) {
        memcpy(ops,saved,sizeof(ops));layouts=(struct ps5vk_layout_state){0};
        plan=(struct ps5vk_readback_plan){0};span_result=VK_SUCCESS;
        source_size=65536;destination_size=64*64*4;
        unsigned count=4;VkImage color=NULL;
        switch(failure) {
        case 0:count=3;break;
        case 1:ops[1].type=PS5VK_DISPATCH;break;
        case 2:ops[0].src_stage=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;break;
        case 3:ops[0].image_barrier.srcAccessMask=0;break;
        case 4:ops[1].copy_region.bufferOffset=4;break;
        case 5:ops[1].copy_region.imageExtent.width=63;break;
        case 6:ops[2].dst_access=VK_ACCESS_HOST_WRITE_BIT;break;
        case 7:ops[2].buffer_barrier.size=4;break;
        case 8:ops[3].src_access=VK_ACCESS_TRANSFER_WRITE_BIT;break;
        case 9:color=&other;break;
        case 10:span_result=VK_ERROR_MEMORY_MAP_FAILED;break;
        case 11:source_size--;break;
        case 12:destination_size--;break;
        case 13:ops[1].copy_image=&other;break;
        case 14:ops[1].copy_destination=NULL;break;
        case 15:image.layout=VK_IMAGE_LAYOUT_UNDEFINED;break;
        }
        assert(ps5vk_readback_commands(&device,ops,count,color,&layouts,&plan)!=VK_SUCCESS);
        assert(!layouts.count && !plan.image && !plan.buffer);
        for(unsigned i=0;i<sizeof(destination);++i)assert(destination[i]==0xa5);
    }
    /* Failed preparation must not mutate a render pass's existing transaction. */
    layouts=(struct ps5vk_layout_state){0};
    assert(ps5vk_layout_transition(&layouts,&image,VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    struct ps5vk_layout_state before=layouts;
    ops[1].copy_region.bufferOffset=4;
    assert(ps5vk_readback_commands(&device,ops,4,&image,&layouts,&plan)!=VK_SUCCESS);
    assert(!memcmp(&layouts,&before,sizeof(layouts)));

    /* Six tiled layers become six tight buffer slices, including explicit
     * tight rowLength/imageHeight. Distinct per-layer ramps detect a repeated
     * layer zero, a tight GPU stride, or a copy that touches padding. */
    memcpy(ops,saved,sizeof(ops));
    image.info.imageType=VK_IMAGE_TYPE_2D;
    image.info.arrayLayers=6;
    image.info.usage|=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    image.layout=VK_IMAGE_LAYOUT_GENERAL;
    ops[0].image_barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
    ops[1].copy_region.imageSubresource.layerCount=6;
    ops[1].copy_region.bufferRowLength=64;
    ops[1].copy_region.bufferImageHeight=64;
    source_size=sizeof(source);destination_size=6*64*64*4;
    layouts=(struct ps5vk_layout_state){0};plan=(struct ps5vk_readback_plan){0};
    assert(ps5vk_readback_commands(&device,ops,4,NULL,&layouts,&plan)==VK_SUCCESS);
    assert(plan.layer_stride==131072 && image.layout==VK_IMAGE_LAYOUT_GENERAL);
    memset(source,0x37,sizeof(source));
    for(unsigned layer=0;layer<6;++layer)for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
        uint32_t value=0xff000000u|(layer<<16)|(y<<8)|x;
        memcpy(source+layer*131072+ps5vk_rgba8_64k_rx_offset(x,y,64),&value,4);
    }
    assert(!ps5vk_readback_detile(&image,plan.layer_stride,destination,destination_size,source,source_size));
    for(unsigned layer=0;layer<6;++layer)for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
        uint32_t value;
        memcpy(&value,destination+((layer*64+y)*64+x)*4,4);
        assert(value==(0xff000000u|(layer<<16)|(y<<8)|x));
    }
    for(unsigned i=destination_size;i<sizeof(destination);++i)assert(destination[i]==0xa5);
    for(unsigned failure=0;failure<4;++failure) {
        layouts=(struct ps5vk_layout_state){0};plan=(struct ps5vk_readback_plan){0};
        source_size=sizeof(source);destination_size=6*64*64*4;
        ops[1].copy_region.imageSubresource.layerCount=6;
        ops[1].copy_region.bufferImageHeight=64;
        if(failure==0)--source_size;
        if(failure==1)--destination_size;
        if(failure==2)ops[1].copy_region.imageSubresource.layerCount=5;
        if(failure==3)ops[1].copy_region.bufferImageHeight=63;
        assert(ps5vk_readback_commands(&device,ops,4,NULL,&layouts,&plan)!=VK_SUCCESS);
        assert(!layouts.count && !plan.image);
    }
}
