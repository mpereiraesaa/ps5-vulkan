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
    size_t start=find_sh(words,n,0xb810);assert(start<n);
    assert(words[start+2]==0 && words[start+3]==0 && words[start+4]==0);
    d.group_base[0]=4;d.group_base[1]=5;d.group_base[2]=1;
    n=ps5vk_dispatch_encode(words,128,&d);assert(n);
    start=find_sh(words,n,0xb810);assert(start<n);
    assert(words[start+2]==4 && words[start+3]==5 && words[start+4]==1);
    memcpy(saved,words,sizeof(saved));
    d.group_base[0]=65535;assert(!ps5vk_dispatch_encode(words,128,&d));
    assert(!memcmp(words,saved,sizeof(saved)));
    d.group_base[0]=65534;assert(!ps5vk_dispatch_encode(words,128,&d));
    assert(!memcmp(words,saved,sizeof(saved)));
    d.groups[0]=1;
    n=ps5vk_dispatch_encode(words,128,&d);assert(n);
    start=find_sh(words,n,0xb810);assert(start<n && words[start+2]==65534);
    d.group_base[0]=d.group_base[1]=d.group_base[2]=0;
    d.groups[0]=3;
    n=ps5vk_dispatch_encode(words,128,&d);assert(n);
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
    /* Real scratch ABI: raw address at s0:1, odd KiB stride, bounded ring. */
    d.program=&p;d.addresses.descriptor_table=d.descriptor_tables[0];
    d.descriptor_tables[2]=0x20000a000;d.push_constants=0x20000c000;
    p.scratch_bytes_per_wave=16384;
    d.scratch=0x201000000;
    d.scratch_bytes=17408u*(uint64_t)PS5VK_COMPUTE_SCRATCH_WAVES;
    n=ps5vk_dispatch_encode(words,128,&d);assert(n);
    user=find_sh(words,n,0xb900);assert(user<n);
    assert(words[user+2]==(uint32_t)d.scratch && words[user+3]==0x80000002u);
    size_t ring=find_sh(words,n,0xb860);assert(ring<n);
    assert(words[ring+2]==(1152u|(17u<<12)));
    size_t resource=find_sh(words,n,0xb848);assert(resource<n);
    assert(words[resource+3]&1u);
    memcpy(saved,words,sizeof(saved));
    d.scratch_bytes--;assert(!ps5vk_dispatch_encode(words,128,&d));
    assert(!memcmp(words,saved,sizeof(saved)));
    d.scratch_bytes++;d.scratch=d.addresses.code;
    assert(!ps5vk_dispatch_encode(words,128,&d));
    d.scratch=0x201000001;assert(!ps5vk_dispatch_encode(words,128,&d));
    d.scratch=0x201000000;p.scratch_bytes_per_wave=16385;
    assert(!ps5vk_dispatch_encode(words,128,&d));
    p.scratch_bytes_per_wave=0;d.scratch=0;d.scratch_bytes=0;
    n=ps5vk_dispatch_encode(words,128,&d);assert(n);
    ring=find_sh(words,n,0xb860);resource=find_sh(words,n,0xb848);
    assert(words[ring+2]==0 && !(words[resource+3]&1u));
    /* The dispatch is followed by a CP DMA prefetch of its code into GL2. */
    size_t prefetch=n;
    for(size_t i=0;i<n;i+=((words[i]>>16)&0x3fff)+2)
        if(((words[i]>>8)&0xff)==0x15){prefetch=i+5;break;}
    assert(prefetch<n && words[prefetch]==0xc0055000u && words[prefetch+1]==0x60200000u);
    assert(words[prefetch+2]==0x00004000u && words[prefetch+3]==2 && words[prefetch+4]==0x00004000u);
    assert(words[prefetch+5]==2 && words[prefetch+6]==(0x80000000u|640u));
    /* Placement: prefetch mode 3, a branch to the program, s_code_end padding. */
    uint32_t program[80],placed[160];
    for(unsigned i=0;i<80;++i)program[i]=0x7e000200u+i;
    assert(ps5vk_placed_code_bytes(80)==sizeof(placed));
    ps5vk_place_code(placed,program,80);
    assert(placed[0]==0xbfa00003u && placed[1]==0xbf82000eu && placed[15]==0xbf9f0000u);
    assert(!memcmp(placed+16,program,sizeof(program)));
    assert(placed[96]==0xbf9f0000u && placed[159]==0xbf9f0000u);
    puts("Multi-set and scratch dispatch encoding: pass (host packets only)");
}
