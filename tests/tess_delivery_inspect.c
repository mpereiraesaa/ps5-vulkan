#include "libpsbc/psbc_compile.h"
#include "spirv_graphics_interface.h"
#include "runtime_shader.h"
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
            .topology=VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,.patch_control_points=patch_points,
            /* A colour subpass: this harness's fragment stage writes a colour,
             * so the key has to name the attachment it writes into. An unset
             * format now means a depth-only pass, whose fragment stage exports
             * nothing. */
            .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
            .color_write_mask={15}};
        VkVertexInputBindingDescription binding={0,8,VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attribute={0,0,VK_FORMAT_R32G32_SFLOAT,0};
        if(getenv("TESS_VERTEX_INPUT")) {
            key.vertex_binding_count=1;key.vertex_bindings=&binding;
            key.vertex_attribute_count=1;key.vertex_attributes=&attribute;
        }
        if(getenv("TESS_EXPECT_INVALID_INTERFACE")) {
            assert(!ps5vk_spirv_graphics_interface(&key));
            free(f);free(v);free(h);free(e);
            return 0;
        }
        assert(ps5vk_spirv_graphics_interface(&key));
        free(f);
    }
    PsbcCompileOptions options={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_TESS_CTRL,
        .entrypoint="main",.optimise=true,.address32_hi=2,
        .rasterization_samples=1,.patch_control_points=patch_points};
    PsbcShaderOutput hull={0},domain={0};
    if(getenv("TESS_VERTEX_INPUT")) {
        options.vertex_attribute_count=1;
        options.vertex_attributes[0]=(PsbcVertexAttribute){.location=0,.binding=0,
            .format=PSBC_VERTEX_FORMAT_R32G32_FLOAT,.offset=0,.stride=8,.alignment=1};
    }
    fprintf(stderr,"DELIVERY_HULL_BEGIN\n");
    assert(psbc_compile_tess_pipeline(v,vn,h,hn,e,en,&options,&hull)==PSBC_RESULT_OK);
    struct ps5vk_runtime_draw_abi checked_hull_abi;
    struct ps5vk_runtime_shader checked_hull_header;
    assert(!ps5vk_runtime_hull_abi_build(&hull.metadata,&checked_hull_abi));
    assert(!ps5vk_runtime_hull_build(&checked_hull_header,&hull));
    if(getenv("TESS_VERTEX_INPUT")) {
        struct ps5vk_runtime_draw_abi abi;
        fprintf(stderr,"HULL_FETCH valid=%u mask=%x slot=%u count=%u window=%u\n",
            hull.metadata.vertex_buffer_table_valid,hull.metadata.vertex_buffer_usage_mask,
            hull.metadata.vertex_buffer_table_user_data_dword,hull.metadata.user_sgpr_count,
            hull.metadata.user_data_window_base);
        assert(!ps5vk_runtime_hull_abi_build(&hull.metadata,&abi));
        struct ps5vk_runtime_shader header;
        assert(!ps5vk_runtime_hull_build(&header,&hull));
        assert(abi.vertex_buffer_valid && abi.vertex_buffer_usage_mask==1);
        uint32_t bank[16],pixel[16],tables[4]={0};
        assert(!ps5vk_runtime_draw_values_sets(&abi,0,0,0,0,0x2000,0,tables,bank,pixel));
        assert(bank[abi.vertex_buffer_slot]==0x2000);
    }
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
