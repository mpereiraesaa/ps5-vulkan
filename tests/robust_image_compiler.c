/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ps5vk_compiler.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    assert(argc == 3 || argc == 4);
    const unsigned first_binding=argc==4?1:0;
    VkDescriptorType type;
    if (!strcmp(argv[1], "storage_image")) type=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    else if (!strcmp(argv[1], "sampled_image")) type=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    else if (!strcmp(argv[1], "uniform_texel")) type=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    else { assert(!strcmp(argv[1], "storage_texel")); type=VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER; }
    FILE *f=fopen(argv[2], "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long bytes=ftell(f); rewind(f);
    assert(bytes>=20 && !(bytes%4));
    uint32_t *words=malloc((size_t)bytes); assert(words);
    assert(fread(words,1,(size_t)bytes,f)==(size_t)bytes); fclose(f);
    struct VkPipelineLayout_T layout={.set_count=1};
    layout.sets[0].count=3;
    for (unsigned b=0; b<PS5VK_MAX_BINDINGS; ++b) {
        layout.sets[0].binding[b].first=b<3?b:3;
        if(b<3) {
            layout.sets[0].binding[b].count=1;
            layout.sets[0].binding[b].stages=VK_SHADER_STAGE_COMPUTE_BIT;
            layout.sets[0].type[b]=b?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:type;
        }
    }
    struct ps5vk_compiled_program program={0}; uint32_t *code=NULL;
    VkResult result=ps5vk_runtime_compile_compute(words,(size_t)bytes/4,"main",&layout,NULL,&program,&code);
    assert(result==VK_SUCCESS && code && program.code_words);
    assert(program.gfx==1013 && program.wave_size==32);
    assert(program.local_size[0]==32 && program.local_size[1]==1 && program.local_size[2]==1);
    assert(program.descriptor_count==3-first_binding && program.descriptor_set_mask==1);
    unsigned resource_words=type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER?12:
        type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?8:4;
    for(unsigned i=0;i<program.descriptor_count;++i) {
        const struct ps5vk_program_descriptor *desc=&program.descriptors[i];
        unsigned binding=i+first_binding;
        assert(desc->set==0 && desc->binding==binding && desc->element==0);
        assert(desc->type==(binding?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:type));
        assert(desc->table_dword==(binding?resource_words+4*(binding-1):0));
    }
    printf("compiled descriptors=%u gfx=1013 wave=32 words=%zu\n",program.descriptor_count,program.code_words);
    free(code); free(words); return 0;
}
