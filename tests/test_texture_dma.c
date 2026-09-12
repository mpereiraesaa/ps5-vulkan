#include "texture_dma.h"
#include <assert.h>
#include <string.h>
int main(void)
{
    uint32_t out[21],saved[21];memset(out,0x55,sizeof(out));memcpy(saved,out,sizeof(out));
    struct ps5vk_texture_copy p={16,512,32,256,12,2};
    assert(!ps5vk_texture_dma(out,13,0x100000000,0x200000000,&p) && !memcmp(out,saved,sizeof(out)));
    assert(ps5vk_texture_dma(out,21,0x100000000,0x200000000,&p)==14);
    assert(out[0]==0xc0055000 && out[1]==0x60300000 && out[2]==16 && out[3]==1);
    assert(out[4]==512 && out[5]==2 && out[6]==12);
    assert(out[7]==0xc0055000 && out[8]==0xe0300000 && out[9]==48 && out[11]==768 && out[13]==12);
    memcpy(saved,out,sizeof(out));
    assert(!ps5vk_texture_dma(out,21,0x1000,0x1000,&(struct ps5vk_texture_copy){0,4,16,16,12,2}));
    p.source_offset=UINT64_MAX;
    assert(!ps5vk_texture_dma(out,21,0x1000,0x2000,&p) && !memcmp(out,saved,sizeof(out)));
    p.source_offset=0;p.source_pitch=UINT64_MAX;
    assert(!ps5vk_texture_dma(out,21,0x1000,0x2000,&p));
    p.source_pitch=32;p.rows=0;assert(!ps5vk_texture_dma(out,21,0x1000,0x2000,&p));
    assert(ps5vk_dma_fill(out,21,0x200000000,0x200004,0x3f800000)==14);
    assert(out[0]==0xc0055000 && out[1]==0x40300000 && out[2]==0x3f800000 && !out[3]);
    assert(out[4]==0 && out[5]==2 && out[6]==0x1ffffc);
    assert(out[8]==0xc0300000 && out[11]==0x1ffffc && out[12]==2 && out[13]==8);
    memcpy(saved,out,sizeof(out));
    assert(!ps5vk_dma_fill(out,13,0x200000000,0x200004,0));
    assert(!ps5vk_dma_fill(out,21,0x200000001,4,0));
    assert(!ps5vk_dma_fill(out,21,(UINT64_C(1)<<48)-4,8,0));
    assert(!ps5vk_dma_fill(out,21,0x1000,0,0));
    assert(!ps5vk_dma_fill(out,21,0x1000,6,0));
    assert(!memcmp(out,saved,sizeof(out)));
}
