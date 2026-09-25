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
    struct ps5vk_descriptor_table_layout reference=out;
    const VkShaderStageFlags visibility[]={VK_SHADER_STAGE_ALL,VK_SHADER_STAGE_ALL_GRAPHICS,
        VK_SHADER_STAGE_GEOMETRY_BIT,VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
        VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT};
    for(unsigned i=0;i<sizeof(visibility)/sizeof(visibility[0]);++i) {
        sets[0].binding[7].stages=visibility[i];
        assert(ps5vk_descriptor_table_layout_build(4,sets,&out)==VK_SUCCESS);
        assert(!memcmp(&reference,&out,sizeof(out))); /* no offset/size compaction */
        assert(sets[0].binding[7].stages==visibility[i]);
    }
    sets[0]=good;
    sets[0].binding[7].first++;rejects(4,sets);sets[0]=good;
    sets[0].binding[7].count=UINT32_MAX;rejects(4,sets);sets[0]=good;
    sets[0].count--;rejects(4,sets);sets[0]=good;
    sets[0].binding[0].stages=VK_SHADER_STAGE_VERTEX_BIT;rejects(4,sets);sets[0]=good;
    sets[0].type[0]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;rejects(4,sets);sets[0]=good;
    sets[0].binding[7].stages=0;rejects(4,sets);sets[0]=good;
    sets[0].binding[7].stages=UINT32_C(0x40000000);rejects(4,sets);sets[0]=good;
    sets[0].type[7]=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    assert(ps5vk_descriptor_table_layout_build(4,sets,&out)==VK_SUCCESS);
    assert(out.binding[0][7].byte_offset==32 && out.binding[0][7].byte_stride==32);
    assert(out.binding[0][31].byte_offset==800 && out.set_bytes[0]==816);
    sets[0]=good;
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
    /* The input-attachment role is a resource-only record: the encoder's eight
     * DWORDs and no sampler payload. Its stride is half a combined T#/S#
     * record, so a mixed table's canonical offsets are neither compacted nor
     * re-based around it. */
    assert(ps5vk_descriptor_record_bytes(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)==32);
    /* DXVK's separate forms: a sampled image is the T# alone and a sampler the
     * S# alone; a storage texel buffer is one V# like the uniform one. The
     * compute path encodes all of them, never a combined or input record. */
    assert(ps5vk_descriptor_record_bytes(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)==32);
    assert(ps5vk_descriptor_record_bytes(VK_DESCRIPTOR_TYPE_SAMPLER)==16);
    assert(ps5vk_descriptor_record_bytes(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)==16);
    assert(ps5vk_descriptor_record_bytes(VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)==0);
    assert(ps5vk_compute_record_dwords(VK_DESCRIPTOR_TYPE_SAMPLER)==4 &&
           ps5vk_compute_record_dwords(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)==8 &&
           ps5vk_compute_record_dwords(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)==4 &&
           ps5vk_compute_record_dwords(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)==8 &&
           ps5vk_compute_record_dwords(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)==4);
    assert(!ps5vk_compute_record_dwords(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
           !ps5vk_compute_record_dwords(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT));
    memset(sets,0,sizeof(sets));
    for(unsigned s=0;s<2;++s) {
        sets[s].binding[1]=(struct ps5vk_binding){.count=1,.stages=VK_SHADER_STAGE_FRAGMENT_BIT};
        sets[s].type[1]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sets[s].binding[4]=(struct ps5vk_binding){.count=2,.stages=VK_SHADER_STAGE_FRAGMENT_BIT};
        sets[s].type[4]=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        sets[s].binding[6]=(struct ps5vk_binding){.count=1,
            .stages=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT};
        sets[s].type[6]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        prefix(&sets[s]);
    }
    assert(ps5vk_descriptor_table_layout_build(2,sets,&out)==VK_SUCCESS);
    assert(out.binding_count==6 && out.descriptor_count==8);
    for(unsigned s=0;s<2;++s) {
        assert(out.binding[s][0].byte_offset==0 && !out.binding[s][0].byte_stride);
        assert(out.binding[s][1].byte_offset==0 && out.binding[s][1].byte_stride==16);
        assert(out.binding[s][4].byte_offset==16 && out.binding[s][4].byte_stride==32);
        assert(out.binding[s][6].byte_offset==80 && out.binding[s][6].byte_stride==48);
        assert(out.set_bytes[s]==128);
    }
    struct ps5vk_set_signature mixed=sets[0];
    struct ps5vk_descriptor_table_layout mixed_reference=out;
    for(unsigned i=0;i<sizeof(visibility)/sizeof(visibility[0]);++i) {
        sets[0].binding[4].stages=visibility[i];
        assert(ps5vk_descriptor_table_layout_build(2,sets,&out)==VK_SUCCESS);
        assert(!memcmp(&mixed_reference,&out,sizeof(out))); /* no re-basing */
    }
    sets[0]=mixed;
    /* A zero-count input attachment still instantiates nothing, and declaring
     * the type without descriptors stays a rejection rather than free storage. */
    assert(ps5vk_descriptor_table_layout_build(2,sets,&out)==VK_SUCCESS &&
        !memcmp(&mixed_reference,&out,sizeof(out)));
    sets[0].binding[4]=(struct ps5vk_binding){0};
    sets[0].type[4]=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    prefix(&sets[0]);rejects(2,sets);
    puts("Descriptor tables: sparse mixed records, arrays, four sets, atomic rejection pass");
}
