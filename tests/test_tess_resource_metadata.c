// Copyright (C) 2026 Manuel Pereira
// SPDX-License-Identifier: GPL-3.0-or-later
#include "psbc_compile.h"
#include "runtime_shader.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t *read_module(const char *path,size_t *size) {
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));
    long n=ftell(f);assert(n>0 && !(n&3));rewind(f);
    uint32_t *p=malloc((size_t)n);assert(p);
    assert(fread(p,1,(size_t)n,f)==(size_t)n);fclose(f);*size=(size_t)n;return p;
}
int main(int argc,char **argv) {
    assert(argc==6 || argc==7 || argc==8);size_t a,b,c;
    uint32_t *v=read_module(argv[1],&a),*h=read_module(argv[2],&b),*e=read_module(argv[3],&c);
    PsbcCompileOptions o={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_TESS_CTRL,
        .entrypoint="main",.optimise=true,.address32_hi=2,.patch_control_points=3,
        .rasterization_samples=1,.static_descriptor_use=true,.force_indirect_push_constants=true,
        .descriptor_binding_count=2,.descriptor_bindings={
            {.set=0,.binding=0,.type=PSBC_DESCRIPTOR_UNIFORM_BUFFER,.array_size=1,.offset=0,.stride=16},
            {.set=0,.binding=1,.type=PSBC_DESCRIPTOR_UNIFORM_BUFFER,.array_size=1,.offset=16,.stride=16}}};
    const bool two_sets=getenv("TESS_TEST_TWO_SETS")!=NULL;
    if(two_sets) {
        o.descriptor_bindings[1].set=1;
        o.descriptor_bindings[1].binding=0;
        o.descriptor_bindings[1].offset=0;
    }
    if(argc==8) {
        uint32_t vs=(uint32_t)strtoul(argv[6],NULL,0),hs=(uint32_t)strtoul(argv[7],NULL,0);
        o.specialization_constant_count=1;
        o.specialization_constants[0]=(PsbcSpecializationConstant){.constant_id=0,.size=4};
        memcpy(o.specialization_constants[0].data,&hs,4);
        o.previous_parameters.enabled=true;
        o.previous_parameters.entrypoint="main";
        /* 2 means explicitly empty map: preserve the VS source defaultfalse. */
        o.previous_parameters.specialization_constant_count=vs==2?0:1;
        o.previous_parameters.specialization_constants[0]=(PsbcSpecializationConstant){.constant_id=0,.size=4};
        memcpy(o.previous_parameters.specialization_constants[0].data,&vs,4);
        o.next_link_parameters.enabled=true;
        o.next_link_parameters.entrypoint="main";
    }
    PsbcShaderOutput out={0};PsbcResult rc=psbc_compile_tess_pipeline(v,a,h,b,e,c,&o,&out);
    printf("rc=%d valid=%u mask=%llx slot=%u push_valid=%u push_size=%u push_slot=%u unresolved=%llx\n",
        (int)rc,out.metadata.descriptor_set_valid[0],
        (unsigned long long)out.metadata.descriptor_used_binding_mask[0],
        out.metadata.descriptor_set_user_data_dword[0],out.metadata.push_constants_valid,
        out.metadata.push_constant_size,out.metadata.push_constants_user_data_dword,
        (unsigned long long)out.metadata.unresolved_fields);
    assert(rc==PSBC_RESULT_OK);
    assert(out.metadata.descriptor_used_binding_mask[0]==
        (two_sets?1:strtoull(argv[4],NULL,0)));
    if(two_sets) {
        assert(out.metadata.descriptor_set_valid[0]);
        assert(out.metadata.descriptor_set_valid[1]);
        assert(out.metadata.descriptor_used_binding_mask[1]==1);
        assert(out.metadata.descriptor_set_user_data_dword[0]!=
               out.metadata.descriptor_set_user_data_dword[1]);
        assert(out.metadata.user_sgpr_count<=16);
    }
    assert(out.metadata.push_constant_size==strtoul(argv[5],NULL,0));
    unsigned bindings=(unsigned)strtoul(argv[4],NULL,0);
    assert(out.metadata.hull_push_use_valid);
    assert(out.metadata.push_use_valid);
    assert(out.metadata.previous_stage_push_dwords==out.metadata.hull_vertex_push_dwords);
    assert(out.metadata.stage_push_dwords==out.metadata.hull_control_push_dwords);
    assert(out.metadata.hull_vertex_push_dwords==((bindings&1)?1u:0u));
    fprintf(stderr,"push-use vertex=%llx control=%llx bytes=%u\n",
        (unsigned long long)out.metadata.hull_vertex_push_dwords,
        (unsigned long long)out.metadata.hull_control_push_dwords,out.metadata.push_constant_size);
    assert(out.metadata.hull_control_push_dwords==(argc==7?
        strtoull(argv[6],NULL,0):((bindings&2)?0x70u:0u)));
    struct ps5vk_runtime_shader loaded,saved;
    assert(!ps5vk_runtime_hull_build(&loaded,&out));
    saved=loaded;
    if(out.metadata.push_constants_valid) {
        PsbcShaderMetadata original=out.metadata;
        out.metadata.push_constants_user_data_dword=out.metadata.ps5_ring_table_user_data_dword;
        assert(ps5vk_runtime_hull_build(&loaded,&out));
        assert(!memcmp(&loaded,&saved,sizeof(loaded)));
        out.metadata=original;out.metadata.push_constants_valid=false;
        assert(ps5vk_runtime_hull_build(&loaded,&out));
        assert(!memcmp(&loaded,&saved,sizeof(loaded)));
        out.metadata=original;
    }
    if(out.metadata.descriptor_set_valid[0]) {
        PsbcShaderMetadata original=out.metadata;
        out.metadata.descriptor_set_user_data_dword[0]=out.metadata.ps5_ring_table_user_data_dword;
        assert(ps5vk_runtime_hull_build(&loaded,&out));
        assert(!memcmp(&loaded,&saved,sizeof(loaded)));
        out.metadata=original;
    }
    psbc_free_output(&out);
    if(argc==8) {
        PsbcLinkedStageParameters saved=o.previous_parameters;
        o.previous_parameters.specialization_constant_count=PSBC_MAX_SPECIALIZATION_CONSTANTS+1;
        assert(psbc_compile_tess_pipeline(v,a,h,b,e,c,&o,&out)==PSBC_RESULT_INTERNAL_ERROR);
        assert(!out.data && !out.machine_code);
        o.previous_parameters=saved;
        o.previous_parameters.specialization_constant_count=2;
        o.previous_parameters.specialization_constants[1]=o.previous_parameters.specialization_constants[0];
        assert(psbc_compile_tess_pipeline(v,a,h,b,e,c,&o,&out)==PSBC_RESULT_INTERNAL_ERROR);
        assert(!out.data && !out.machine_code);
        o.previous_parameters=saved;
        o.next_link_parameters.specialization_constant_count=1;
        o.next_link_parameters.specialization_constants[0].size=9;
        assert(psbc_compile_tess_pipeline(v,a,h,b,e,c,&o,&out)==PSBC_RESULT_INTERNAL_ERROR);
        assert(!out.data && !out.machine_code);
    }
    free(v);free(h);free(e);return rc!=PSBC_RESULT_OK;
}
