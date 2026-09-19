#include "vertex_descriptor.h"
#include <assert.h>
#include <string.h>
int main(void)
{
    uint32_t out[4]={1,2,3,4};
    assert(!ps5vk_vertex_descriptor(out,UINT64_C(0x200000100),72,24,24));
    assert(out[0]==0x100 && out[1]==0x00180002 && out[2]==3 && out[3]==0x11014fac);
    /* Offset already resolved; incomplete trailing bytes do not expose a record. */
    assert(!ps5vk_vertex_descriptor(out,UINT64_C(0x300000200),95,24,24) && out[2]==3);
    assert(!ps5vk_vertex_descriptor(out,UINT64_C(0x300000200),128,32,24) && out[2]==4);
    assert(!ps5vk_vertex_descriptor(out,UINT64_C(0x300000204),9,1,1) &&
        out[0]==0x204 && out[1]==0x00010003 && out[2]==9);
    /* A stride-8 binding whose second attribute sits at offset 16 is a
     * structure-of-arrays layout: the descriptor's window has to cover the
     * whole bound buffer (72/8 = 9 records) or that attribute is fetched past
     * the end of the window. The pinned upstream rasterization module binds
     * exactly this shape with stride 16 and the second attribute at 96. */
    assert(!ps5vk_vertex_descriptor(out,UINT64_C(0x300000204),72,8,16) && out[2]==9);
    assert(!ps5vk_vertex_descriptor(out,UINT64_C(0x300000100),192,16,112) && out[2]==12);
    uint32_t saved[4];memcpy(saved,out,sizeof(out));
    const struct {uint64_t address,bytes;uint32_t stride,extent;} bad[]={
        {0,72,24,24},{0x1001,72,24,24},{UINT64_C(1)<<48,72,24,24},
        {(UINT64_C(1)<<48)-4,72,24,24},{0x1000,0,24,24},
        {0x1000,23,24,24},{0x1000,72,0,24},
        {0x1000,72,0x4000,24},{0x1000,27,24,28},{0x1000,72,24,0},
        {0x1000,(UINT64_C(1)<<32),1,1}};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        assert(ps5vk_vertex_descriptor(out,bad[i].address,bad[i].bytes,bad[i].stride,bad[i].extent));
        assert(!memcmp(out,saved,sizeof(out)));
    }
    assert(ps5vk_vertex_descriptor(0,0x1000,72,24,24));
    assert(ps5vk_vertex_table_address(UINT64_C(0x200000000),UINT64_C(0x200000100),16));
    assert(ps5vk_vertex_table_address(UINT64_C(0x200000000),UINT64_C(0x2fffffff0),16));
    assert(!ps5vk_vertex_table_address(UINT64_C(0x200000000),UINT64_C(0x2fffffff0),32));
    assert(!ps5vk_vertex_table_address(UINT64_C(0x200000000),UINT64_C(0x300000000),16));
    assert(!ps5vk_vertex_table_address(UINT64_C(0x200000000),UINT64_C(0x200000004),16));
    assert(!ps5vk_vertex_table_address(UINT64_C(0x200000000),UINT64_C(0x200000100),0));
    assert(!ps5vk_vertex_table_address(UINT64_C(0x200000000),UINT64_C(0x200000100),15));
    return 0;
}
