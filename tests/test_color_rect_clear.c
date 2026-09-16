#include "color_rect_clear.h"
#include <assert.h>
#include <string.h>

enum { STRIDE=131072, LAYERS=6, WIDTH=130, HEIGHT=70 };
static uint32_t words[32768],saved[32768];
static unsigned char image[STRIDE*LAYERS],expected[STRIDE*LAYERS];

static void exercise(VkRect2D rect,uint32_t mask)
{
    const uint64_t base=UINT64_C(0x200000000);
    const uint32_t color=0xff1248abu;
    memset(words,0xa5,sizeof(words));
    memset(image,0x59,sizeof(image));memcpy(expected,image,sizeof(image));
    size_t count=ps5vk_color_rect_clear(words,32768,base,sizeof(image),STRIDE,
        WIDTH,HEIGHT,LAYERS,mask,rect,color);
    assert(count && count%7==0);
    /* Decode every actual emitted DMA packet and replay only its byte writes. */
    for(size_t i=0;i<count;i+=7) {
        assert(words[i]==0xc0055000 && words[i+1]==0xc0300000 &&
            words[i+2]==color && !words[i+3]);
        uint64_t address=words[i+4]|((uint64_t)words[i+5]<<32);
        size_t offset=(size_t)(address-base),bytes=words[i+6];
        assert(address>=base && offset<=sizeof(image) && bytes<=sizeof(image)-offset);
        for(size_t j=0;j<bytes;j+=4)memcpy(image+offset+j,&color,4);
    }
    if(!mask)mask=1;
    for(unsigned layer=0;layer<LAYERS;++layer) {
        if(!(mask&(1u<<layer)))continue;
        if(!rect.offset.x && !rect.offset.y && rect.extent.width==WIDTH && rect.extent.height==HEIGHT) {
            for(size_t i=0;i<STRIDE;i+=4)memcpy(expected+layer*STRIDE+i,&color,4);
        } else for(unsigned y=(unsigned)rect.offset.y;y<(unsigned)rect.offset.y+rect.extent.height;++y)
            for(unsigned x=(unsigned)rect.offset.x;x<(unsigned)rect.offset.x+rect.extent.width;++x) {
                size_t at=layer*STRIDE+ps5vk_rgba8_64k_rx_offset(x,y,WIDTH);
                memcpy(expected+at,&color,4);
            }
    }
    /* Full allocation oracle includes every untouched texel, layer and padding. */
    assert(!memcmp(image,expected,sizeof(image)));
    for(size_t i=count;i<32768;++i)assert(words[i]==0xa5a5a5a5);
    memset(words,0x37,sizeof(words));memcpy(saved,words,sizeof(words));
    assert(!ps5vk_color_rect_clear(words,count-1,base,sizeof(image),STRIDE,
        WIDTH,HEIGHT,LAYERS,mask,rect,color));
    assert(!memcmp(words,saved,sizeof(words)));
}

int main(void)
{
    exercise((VkRect2D){{0,0},{WIDTH,HEIGHT}},0x3f);
    exercise((VkRect2D){{32,16},{64,32}},0x15);
    exercise((VkRect2D){{1,3},{128,65}},0x2a);
    exercise((VkRect2D){{127,63},{3,7}},0);
    exercise((VkRect2D){{4,8},{1,1}},0x20);
    const VkRect2D rect={{0,0},{WIDTH,HEIGHT}};
    for(unsigned bad=0;bad<5;++bad) {
        VkRect2D r=rect;
        if(bad==0)r.offset.x=-1;
        if(bad==1)r.extent.height=HEIGHT+1;
        if(bad==2)r.extent.width=0;
        memset(words,0x37,sizeof(words));memcpy(saved,words,sizeof(words));
        assert(!ps5vk_color_rect_clear(words,32768,UINT64_C(0x200000000),
            bad==3?sizeof(image)-1:sizeof(image),STRIDE,WIDTH,HEIGHT,LAYERS,
            bad==4?0x40:0x3f,r,0));
        assert(!memcmp(words,saved,sizeof(words)));
    }
}
