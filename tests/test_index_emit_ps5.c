#include "index_emit_ps5.h"
#include "ps5_platform.h"
#include <assert.h>
static unsigned calls;static int malformed;
static uint32_t *emit(void *opaque,uint32_t n,const void *address,uint64_t modifier)
{
    struct ps5_agc_command_buffer *w=opaque;++calls;
    assert(n==6 && (uintptr_t)address==0x1234567800 && modifier==0x100000005);
    assert(w->top-w->up==6);
    for(unsigned i=0;i<6;++i)*w->up++=i;
    if(malformed)--w->up;
    return NULL; /* The reused writer ABI ignores the C return value. */
}
int main(void)
{
    uint32_t words[16]={0},*p=words;
    struct ps5vk_index_fetch f={0x1234567800,9,2};
    assert(ps5vk_native_emit_index(&p,16,&f,6,0x100000005,emit)==VK_SUCCESS);
    assert(p==words+9 && calls==1 && words[0]==0xc0017a00 && words[1]==0x20000243 && words[2]==0);
    p=words;f.element_bytes=4;
    assert(ps5vk_native_emit_index(&p,9,&f,6,0x100000005,emit)==VK_SUCCESS && words[2]==1);
    p=words;unsigned before=calls;
    assert(ps5vk_native_emit_index(&p,8,&f,6,0x100000005,emit)!=VK_SUCCESS && p==words && calls==before);
    f.available_count=5;
    assert(ps5vk_native_emit_index(&p,16,&f,6,0x100000005,emit)!=VK_SUCCESS && calls==before);
    f.available_count=9;f.address++;
    assert(ps5vk_native_emit_index(&p,16,&f,6,0x100000005,emit)!=VK_SUCCESS && calls==before);
    f.address--;malformed=1;
    assert(ps5vk_native_emit_index(&p,16,&f,6,0x100000005,emit)!=VK_SUCCESS && p==words);
    before=calls;
    assert(ps5vk_native_emit_index(&p,16,&f,0,0x100000005,emit)==VK_SUCCESS && p==words && calls==before);
}
