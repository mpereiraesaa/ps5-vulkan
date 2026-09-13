#include "dispatch_encode.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t find_sh(const uint32_t *words,size_t count,uint32_t reg)
{
    for(size_t i=0;i<count;) {
        assert((words[i]>>30)==3);
        size_t n=((words[i]>>16)&0x3fff)+2;
        if(((words[i]>>8)&0xff)==0x76 && n>=3 && 0xb000+words[i+1]*4==reg)return i;
        i+=n;
    }
    return count;
}
int main(void)
{
    struct ps5vk_compiled_program p={.gfx=1013,.code_words=80,.wave_size=32,
        .local_size={8,4,2},.vgprs=17,.sgprs=10,.float_mode=192,.ieee_mode=1,
        .mem_ordered=1,.user_sgprs=4,.tg_size=1,.tgid={1,1,0},.tidig_components=2,
        .descriptor_set_mask=(1u<<0)|(1u<<2),.descriptor_set_sgpr={2,0,3,0},
        .descriptor_count=2,.descriptors={
            {.set=0,.binding=1,.table_dword=0,.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
            {.set=2,.binding=0,.table_dword=4,.type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}}};
    struct ps5vk_dispatch_encoding d={.program=&p,
        .addresses={.code=0x200004000,.descriptor_table=0x200008000,
            .completion=0x200010000,.readback=0x200010040},
        .descriptor_tables={0x200008000,0,0x20000a000,0},
        .groups={3,7,2},.completion_value=UINT64_C(0x123456789abcdef0)};
    uint32_t words[128]={0},saved[128];
    size_t n=ps5vk_dispatch_encode(words,128,&d);assert(n);
    size_t user=find_sh(words,n,0xb900);assert(user<n);
    assert(words[user+2]==0 && words[user+3]==0);
    assert(words[user+4]==(uint32_t)d.descriptor_tables[0]);
    assert(words[user+5]==(uint32_t)d.descriptor_tables[2]);
    memcpy(saved,words,sizeof(saved));
    d.descriptor_tables[2]=0;assert(!ps5vk_dispatch_encode(words,128,&d));
    assert(!memcmp(words,saved,sizeof(saved)));
    d.descriptor_tables[2]=0x20000a000;p.descriptor_set_sgpr[2]=4;
    assert(!ps5vk_dispatch_encode(words,128,&d));
    p.descriptor_set_sgpr[2]=3;p.grid_size_sgpr=4;p.user_sgprs=7;
    n=ps5vk_dispatch_encode(words,128,&d);assert(n);
    user=find_sh(words,n,0xb900);assert(user<n);
    assert(words[user+6]==3 && words[user+7]==7 && words[user+8]==2);
    d.descriptor_tables[1]=0x20000c000;assert(!ps5vk_dispatch_encode(words,128,&d));
    d.descriptor_tables[1]=0;d.groups[0]=65536;assert(!ps5vk_dispatch_encode(words,128,&d));
    d.groups[0]=3;p.grid_size_sgpr=0;p.user_sgprs=5;p.push_constant_size=16;
    p.push_constant_sgpr=4;d.push_constants=0x20000c000;
    n=ps5vk_dispatch_encode(words,128,&d);assert(n);
    user=find_sh(words,n,0xb900);assert(user<n);
    assert(words[user+4]==(uint32_t)d.descriptor_tables[0]);
    assert(words[user+5]==(uint32_t)d.descriptor_tables[2]);
    assert(words[user+6]==(uint32_t)d.push_constants);
    d.push_constants=d.addresses.completion;
    assert(!ps5vk_dispatch_encode(words,128,&d));
    d.push_constants=0x20000c002;
    assert(!ps5vk_dispatch_encode(words,128,&d));
    d.push_constants=0x10000c000;
    assert(!ps5vk_dispatch_encode(words,128,&d));
    struct ps5vk_compiled_program legacy=p;
    legacy.user_sgprs=2;legacy.descriptor_set_mask=1;
    legacy.descriptor_set_sgpr[0]=1;legacy.descriptor_set_sgpr[2]=0;
    legacy.descriptor_count=1;legacy.descriptors[0].set=0;
    legacy.grid_size_sgpr=0;legacy.push_constant_size=0;legacy.push_constant_sgpr=0;
    d.program=&legacy;d.descriptor_tables[2]=0;d.push_constants=0;
    d.addresses.descriptor_table=d.descriptor_tables[0];
    n=ps5vk_dispatch_encode(words,128,&d);assert(n);
    user=find_sh(words,n,0xb900);assert(user<n);
    assert(words[user+2]==0 && words[user+3]==(uint32_t)d.descriptor_tables[0]);
    legacy.descriptor_set_sgpr[0]=2;assert(!ps5vk_dispatch_encode(words,128,&d));
    legacy.descriptor_set_sgpr[0]=1;d.addresses.descriptor_table+=16;
    assert(!ps5vk_dispatch_encode(words,128,&d));
    puts("Multi-set dispatch SGPR encoding: pass (host packets only)");
}
