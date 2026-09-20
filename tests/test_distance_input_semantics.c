// SPDX-License-Identifier: GPL-3.0-or-later
#include "psbc_compile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t *module(const char *path,size_t *size) {
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));
    long n=ftell(f);assert(n>0 && n%4==0);rewind(f);
    uint32_t *p=malloc((size_t)n);assert(p);
    assert(fread(p,1,(size_t)n,f)==(size_t)n);fclose(f);*size=(size_t)n;return p;
}
int main(int argc,char **argv) {
    if(argc==4 && !strcmp(argv[1],"--domain")) {
        size_t hn,en;uint32_t *h=module(argv[2],&hn),*e=module(argv[3],&en);
        PsbcCompileOptions o={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_TESS_EVAL,
            .entrypoint="main",.optimise=true,.address32_hi=2,.rasterization_samples=1,
            .patch_control_points=3,.ngg=true,.ngg_device_facts=true,
            .ngg_pc_lines=1024,.ngg_min_good_cu_per_sa=18};
        PsbcShaderOutput out={0};
        assert(psbc_compile_domain_pipeline(h,hn,e,en,&o,&out)==PSBC_RESULT_OK);
        const uint32_t expected[]={0xf,0x12f,0x230,0x331};
        assert(out.metadata.output_semantic_count==4);
        for(unsigned i=0;i<4;i++) {
            fprintf(stderr,"export%u=%x expected=%x\n",i,out.metadata.output_semantics[i],expected[i]);
            assert(out.metadata.output_semantics[i]==expected[i]);
        }
        psbc_free_output(&out);free(h);free(e);return 0;
    }
    assert(argc>=3);FILE *f=fopen(argv[1],"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0 && n%4==0);
    rewind(f);uint32_t *p=malloc((size_t)n);assert(p);
    assert(fread(p,1,(size_t)n,f)==(size_t)n);fclose(f);
    PsbcCompileOptions o={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_FRAGMENT,
        .entrypoint="main",.optimise=true,.address32_hi=2,.rasterization_samples=1};
    const char *prefix=getenv("PSBC_TEST_CLIP_PREFIX");
    if(prefix){o.fragment_distance_layout_valid=true;
        o.fragment_clip_distance_count=(uint32_t)strtoul(prefix,NULL,0);}
    PsbcShaderOutput out={0};PsbcResult rc=psbc_compile_shader(p,n,&o,&out);
    if(getenv("PSBC_TEST_REJECT")) {
        assert(rc!=PSBC_RESULT_OK);assert(!out.data && !out.machine_code);
        free(p);return 0;
    }
    assert(rc==PSBC_RESULT_OK);
    assert(out.metadata.input_semantic_count==(unsigned)argc-2);
    for(unsigned i=0;i<out.metadata.input_semantic_count;i++) {
        unsigned expected=(unsigned)strtoul(argv[i+2],NULL,0);
        fprintf(stderr,"attribute%u semantic=%x expected=%x\n",i,out.metadata.input_semantics[i],expected);
        assert(out.metadata.input_semantics[i]==expected);
    }
    psbc_free_output(&out);free(p);return 0;
}
