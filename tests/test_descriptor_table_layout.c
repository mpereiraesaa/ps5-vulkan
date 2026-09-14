/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "descriptor_table_layout.h"
#include <assert.h>
#include <stdio.h>

static void prefix(struct ps5vk_set_signature *s)
{
    s->count=0;
    for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
        s->binding[b].first=s->count;
        s->count+=s->binding[b].count;
    }
}
static void rejects(unsigned count,struct ps5vk_set_signature *sets)
{
    struct ps5vk_descriptor_table_layout out,before;
    memset(&out,0xa5,sizeof(out));before=out;
    assert(ps5vk_descriptor_table_layout_build(count,sets,&out)!=VK_SUCCESS);
    assert(!memcmp(&out,&before,sizeof(out)));
}
int main(void)
{
    struct ps5vk_descriptor_table_layout out;
    assert(ps5vk_descriptor_table_layout_build(0,NULL,&out)==VK_SUCCESS);
    assert(!out.binding_count && !out.descriptor_count);
    rejects(1,NULL);rejects(PS5VK_MAX_SETS+1,NULL);
    struct ps5vk_set_signature sets[PS5VK_MAX_SETS]={0};
    for(unsigned s=0;s<PS5VK_MAX_SETS;++s) {
        sets[s].binding[2]=(struct ps5vk_binding){.count=2,.stages=VK_SHADER_STAGE_VERTEX_BIT};
        sets[s].type[2]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sets[s].binding[7]=(struct ps5vk_binding){.count=24,.stages=VK_SHADER_STAGE_FRAGMENT_BIT};
        sets[s].type[7]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sets[s].binding[31]=(struct ps5vk_binding){.count=1,
            .stages=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT};
        sets[s].type[31]=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        prefix(&sets[s]);
    }
    assert(ps5vk_descriptor_table_layout_build(4,sets,&out)==VK_SUCCESS);
    assert(out.binding_count==12 && out.descriptor_count==108);
    for(unsigned s=0;s<4;++s) {
        assert(out.binding[s][2].byte_offset==0 && out.binding[s][2].byte_stride==16);
        assert(out.binding[s][7].byte_offset==32 && out.binding[s][7].byte_stride==48);
        assert(out.binding[s][31].byte_offset==1184 && out.binding[s][31].byte_stride==16);
        assert(out.set_bytes[s]==1200);
        for(unsigned b=3;b<7;++b)
            assert(out.binding[s][b].byte_offset==32 && !out.binding[s][b].byte_stride);
    }
    struct ps5vk_set_signature good=sets[0];
    sets[0].binding[7].first++;rejects(4,sets);sets[0]=good;
    sets[0].binding[7].count=UINT32_MAX;rejects(4,sets);sets[0]=good;
    sets[0].count--;rejects(4,sets);sets[0]=good;
    sets[0].binding[0].stages=VK_SHADER_STAGE_VERTEX_BIT;rejects(4,sets);sets[0]=good;
    sets[0].type[0]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;rejects(4,sets);sets[0]=good;
    sets[0].binding[7].stages=0;rejects(4,sets);sets[0]=good;
    sets[0].binding[7].stages=VK_SHADER_STAGE_GEOMETRY_BIT;rejects(4,sets);sets[0]=good;
    sets[0].type[7]=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;rejects(4,sets);
    memset(sets,0,sizeof(sets));
    for(unsigned s=0;s<4;++s) {
        sets[s].binding[31]=(struct ps5vk_binding){.count=PS5VK_MAX_DESCRIPTORS,
            .stages=VK_SHADER_STAGE_FRAGMENT_BIT};
        sets[s].type[31]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        prefix(&sets[s]);
    }
    assert(ps5vk_descriptor_table_layout_build(4,sets,&out)==VK_SUCCESS);
    assert(out.descriptor_count==512 && out.set_bytes[3]==6144);
    sets[3].binding[31].count++;prefix(&sets[3]);rejects(4,sets);
    puts("Descriptor tables: sparse mixed records, arrays, four sets, atomic rejection pass");
}
