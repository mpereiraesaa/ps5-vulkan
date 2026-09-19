#include "libpsbc/psbc_compile.h"
#include "spirv_graphics_interface.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static void *read_module(const char *path,size_t *size) {
    FILE *f=fopen(path,"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0 && n%4==0);
    rewind(f);void *p=malloc((size_t)n);assert(p);
    assert(fread(p,1,(size_t)n,f)==(size_t)n);fclose(f);*size=(size_t)n;return p;
}
int main(int argc,char **argv) {
    assert(argc==4 || argc==5 || argc==6);
    unsigned patch_points=argc>=5?(unsigned)strtoul(argv[4],NULL,10):3u;
    assert(patch_points>=1u && patch_points<=32u);
    size_t vn,hn,en;
    void *v=read_module(argv[1],&vn),*h=read_module(argv[2],&hn),*e=read_module(argv[3],&en);
    if(argc==6) {
        size_t fn;void *f=read_module(argv[5],&fn);
        struct ps5vk_graphics_key key={
            .vertex={.words=v,.word_count=vn/4,.entry="main"},
            .tess_control={.words=h,.word_count=hn/4,.entry="main"},
            .tess_eval={.words=e,.word_count=en/4,.entry="main"},
            .fragment={.words=f,.word_count=fn/4,.entry="main"},
            .topology=VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,.patch_control_points=patch_points};
        assert(ps5vk_spirv_graphics_interface(&key));
        free(f);
    }
    PsbcCompileOptions options={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_TESS_CTRL,
        .entrypoint="main",.optimise=true,.address32_hi=2,
        .rasterization_samples=1,.patch_control_points=patch_points};
    PsbcShaderOutput hull={0},domain={0};
    fprintf(stderr,"DELIVERY_HULL_BEGIN\n");
    assert(psbc_compile_tess_pipeline(v,vn,h,hn,e,en,&options,&hull)==PSBC_RESULT_OK);
    for(unsigned i=0;i<hull.metadata.context_register_count;++i)
        if(hull.metadata.context_registers[i].offset==0x2db)
            fprintf(stderr,"DELIVERY_TF=%08x\n",hull.metadata.context_registers[i].value);
    fprintf(stderr,"DELIVERY_DOMAIN_BEGIN\n");
    options.stage=PSBC_STAGE_TESS_EVAL;options.ngg=true;
    options.ngg_device_facts=true;options.ngg_pc_lines=1024;
    options.ngg_min_good_cu_per_sa=18;
    assert(psbc_compile_domain_pipeline(h,hn,e,en,&options,&domain)==PSBC_RESULT_OK);
    fprintf(stderr,"DELIVERY_END\n");
    psbc_free_output(&hull);psbc_free_output(&domain);
    free(v);free(h);free(e);return 0;
}
