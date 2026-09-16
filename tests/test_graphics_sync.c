#include "graphics_sync.h"
#include <assert.h>
#include <string.h>
int main(void)
{
    uint32_t probe[PS5VK_GRAPHICS_PROBE_WORDS];memset(probe,0x5a,sizeof(probe));
    assert(!ps5vk_graphics_register_probe(probe,PS5VK_GRAPHICS_PROBE_WORDS-1,0x12340000));
    assert(probe[0]==0x5a5a5a5a);
    assert(!ps5vk_graphics_register_probe(probe,PS5VK_GRAPHICS_PROBE_WORDS,(uintptr_t)probe));
    assert(!ps5vk_graphics_register_probe(probe,PS5VK_GRAPHICS_PROBE_WORDS,(UINT64_C(1)<<48)-4));
    assert(ps5vk_graphics_register_probe(probe,PS5VK_GRAPHICS_PROBE_WORDS,0x12340000)==PS5VK_GRAPHICS_PROBE_WORDS);
    for(unsigned i=0;i<PS5VK_GRAPHICS_PROBE_REGISTERS;++i) {
        assert(probe[i*6]==0xc0044000 && probe[i*6+1]==0x00100200);
        assert(probe[i*6+2]==ps5vk_graphics_probe_registers[i] && probe[i*6+3]==0);
        assert(probe[i*6+4]==0x12340000+i*4 && probe[i*6+5]==0);
    }
    uint32_t out[10], saved[10]; memset(out,0x5a,sizeof(out)); memcpy(saved,out,sizeof(out));
    assert(!ps5vk_graphics_acquire(out,9) && !memcmp(out,saved,sizeof(out)));
    assert(ps5vk_graphics_acquire(out,10)==10 && out[0]==0xc0004200 && out[1]==0 && out[2]==0xc0065800 && out[9]==0x4381);
    memcpy(out,saved,sizeof(out));
    assert(!ps5vk_graphics_color_to_texture(out,7) && !memcmp(out,saved,sizeof(out)));
    assert(ps5vk_graphics_color_to_texture(out,8)==8 &&
        out[0]==0xc0064900 && out[1]==0x0070f52d &&
        out[2]==0x00010000 && !out[3] && !out[4] && !out[5] && !out[6] && !out[7]);
    const uint64_t address=UINT64_C(0x123456789000), serial=UINT64_C(0xfedcba9876543210);
    assert(ps5vk_graphics_release(out,8,address,serial)==8);
    assert(out[1]==0x0070f514 && out[2]==0x42010000);
    /* gfx10 VGT_EVENT_TYPE: CACHE_FLUSH_AND_INV_TS_EVENT=20, whereas
     * BOTTOM_OF_PIPE_TS=40 does not request CB/DB writeback. Check decoded
     * semantics as well as packet bytes so a copied compute token is caught. */
    assert((out[1]&0x3f)==20 && (out[1]&0x3f)!=40);
    assert(((out[1]>>8)&0xf)==5);
    assert(out[3]==(uint32_t)address && out[4]==address>>32 && out[5]==(uint32_t)serial && out[6]==serial>>32);
    memcpy(saved,out,sizeof(out));
    assert(!ps5vk_graphics_release(out,8,address,0));
    assert(!ps5vk_graphics_release(out,8,address+1,serial));
    assert(!ps5vk_graphics_release(out,7,address,serial));
    assert(!ps5vk_graphics_release(out,8,UINT64_C(1)<<48,serial));
    assert(!ps5vk_graphics_release(out,8,(uintptr_t)out,serial));
    assert(!memcmp(out,saved,sizeof(out)));
    /* GFX10 occlusion-counter event write. The opcode must sit at bits 15:8
     * (PKT3_EVENT_WRITE is 0x46 with a two-dword payload, so the header is
     * 0xC0024600) and the event word must decode to ZPASS_DONE (21) with
     * EVENT_INDEX(1), which is the dump-to-memory form the slot layout needs.
     * A header that puts the opcode in the low bits is a different packet
     * entirely and was observed on hardware to stall the submit stream. */
    const uint64_t slot=UINT64_C(0x208980000);
    memset(out,0x5a,sizeof(out));
    assert(ps5vk_graphics_occlusion_event(out,4,slot)==4);
    assert(out[0]==UINT32_C(0xC0024600));
    assert(((out[0]>>30)&3)==3 && ((out[0]>>16)&0x3fff)==2 && ((out[0]>>8)&0xff)==0x46);
    assert(out[1]==UINT32_C(0x0115));
    assert((out[1]&0x3f)==21 && ((out[1]>>8)&0xf)==1);
    assert(out[2]==(uint32_t)slot && out[3]==(uint32_t)(slot>>32));
    memcpy(saved,out,sizeof(out));
    assert(!ps5vk_graphics_occlusion_event(out,3,slot));
    assert(!ps5vk_graphics_occlusion_event(out,4,0));
    assert(!ps5vk_graphics_occlusion_event(out,4,slot+1));
    assert(!ps5vk_graphics_occlusion_event(out,4,(UINT64_C(1)<<48)-4));
    assert(!ps5vk_graphics_occlusion_event(NULL,4,slot));
    assert(!memcmp(out,saved,sizeof(out)));
    /* The end write is the same packet at base+8, which is what makes the
     * 16-byte start/end pair per render backend. */
    assert(ps5vk_graphics_occlusion_event(out,4,slot+8)==4);
    assert(out[2]==(uint32_t)(slot+8) && out[3]==(uint32_t)((slot+8)>>32));
}
