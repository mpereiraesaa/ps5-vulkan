#include "texture_dma.h"
size_t ps5vk_dma_fill(uint32_t *out,size_t capacity,uint64_t destination,uint64_t bytes,uint32_t value)
{
    const uint64_t limit=UINT64_C(1)<<48,chunk=0x1ffffc;
    if(!out || !destination || destination>=limit || destination%4 || !bytes || bytes%4 ||
        bytes>limit-destination)return 0;
    uint64_t packets=bytes/chunk+(bytes%chunk!=0);
    if(packets>capacity/7)return 0;
    for(uint64_t i=0;i<packets;++i) {
        uint32_t n=(uint32_t)(bytes>chunk?chunk:bytes);uint32_t *w=out+(size_t)i*7;
        /* DMA_DATA: immediate DWORD source (2), L2 destination (3).
         * A uniform DWORD fills tiled pixels without linearizing the image. */
        w[0]=0xc0055000;w[1]=0x40300000u|(i+1==packets?0x80000000u:0);
        w[2]=value;w[3]=0;w[4]=(uint32_t)destination;w[5]=(uint32_t)(destination>>32);w[6]=n;
        destination+=n;bytes-=n;
    }
    return (size_t)packets*7;
}
static int span(uint64_t base,uint64_t offset,uint64_t pitch,uint32_t bytes,uint32_t rows,
    uint64_t *start,uint64_t *end)
{
    uint64_t limit=UINT64_C(1)<<48;
    if(!base || base>=limit || offset>=limit-base || pitch<bytes)return 0;
    *start=base+offset;
    if(bytes>limit-*start || rows-1>(limit-*start-bytes)/pitch)return 0;
    *end=*start+(uint64_t)(rows-1)*pitch+bytes;
    return !(*start%4) && !(pitch%4);
}
size_t ps5vk_texture_dma(uint32_t *out,size_t capacity,uint64_t source,uint64_t destination,
    const struct ps5vk_texture_copy *p)
{
    if(!out || !p || !p->rows || !p->slices || !p->row_bytes ||
        p->row_bytes%4 || p->row_bytes>0x1fffff ||
        p->slices>SIZE_MAX/p->rows || (size_t)p->slices*p->rows>capacity/7)
        return 0;
    uint64_t src,src_end,dst,dst_end;
    if(!span(source,p->source_offset,p->source_pitch,p->row_bytes,p->rows,&src,&src_end) ||
        !span(destination,p->destination_offset,p->destination_pitch,p->row_bytes,p->rows,&dst,&dst_end))
        return 0;
    uint64_t src_slice_bytes=src_end-src,dst_slice_bytes=dst_end-dst;
    if(p->source_slice_pitch<src_slice_bytes ||
       p->destination_slice_pitch<dst_slice_bytes ||
       (p->slices-1)>(UINT64_MAX-src_end)/p->source_slice_pitch ||
       (p->slices-1)>(UINT64_MAX-dst_end)/p->destination_slice_pitch)
        return 0;
    uint64_t src_all_end=src_end+(uint64_t)(p->slices-1)*p->source_slice_pitch;
    uint64_t dst_all_end=dst_end+(uint64_t)(p->slices-1)*p->destination_slice_pitch;
    if(src<dst_all_end && dst<src_all_end)return 0;
    /* Mesa RADV radv_cs_emit_cp_dma + public pkt3.json: DMA_DATA opcode 0x50,
     * source/destination selectors 3 use TC L2; graphics CP_SYNC is bit 31. */
    size_t packet=0;
    for(uint32_t slice=0;slice<p->slices;++slice) {
        uint64_t slice_src=src+(uint64_t)slice*p->source_slice_pitch;
        uint64_t slice_dst=dst+(uint64_t)slice*p->destination_slice_pitch;
        for(uint32_t row=0;row<p->rows;++row,++packet) {
            uint32_t *w=out+7*packet;
            w[0]=0xc0055000u;
            w[1]=0x60300000u|
                (packet+1==(size_t)p->slices*p->rows?0x80000000u:0);
            w[2]=(uint32_t)slice_src;w[3]=(uint32_t)(slice_src>>32);
            w[4]=(uint32_t)slice_dst;w[5]=(uint32_t)(slice_dst>>32);
            w[6]=p->row_bytes;
            slice_src+=p->source_pitch;slice_dst+=p->destination_pitch;
        }
    }
    return packet*7;
}
