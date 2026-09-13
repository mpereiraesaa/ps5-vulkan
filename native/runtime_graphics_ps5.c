#include "runtime_graphics_compiler.h"
#include "graphics_pipeline_ps5.h"
#include <stdlib.h>
#include <string.h>

VkResult ps5vk_native_runtime_graphics_create(VkDevice d,const void *data,void **out)
{
    if(!out)return VK_ERROR_UNKNOWN;
    *out=NULL;
    const struct ps5vk_runtime_graphics_program *input=data;
    if(!d || !input || !d->memory.allocate || !d->memory.release || !d->memory.flush)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct ps5vk_runtime_shader check;
    struct ps5vk_runtime_draw_abi arguments;
    if(ps5vk_runtime_shader_build(&check,&input->vertex) ||
       ps5vk_runtime_shader_build(&check,&input->fragment) ||
       ps5vk_runtime_draw_abi_build(&input->vertex.metadata,&input->fragment.metadata,&arguments))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    size_t vs_at=(sizeof(struct ps5vk_graphics_pair)+255u)&~(size_t)255u;
    size_t fs_at=(vs_at+input->vertex.machine_code_size+255u)&~(size_t)255u;
    size_t table_at=(fs_at+input->fragment.machine_code_size+15u)&~(size_t)15u;
    struct ps5vk_native_graphics_pipeline *p=calloc(1,sizeof(*p));
    if(!p)return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->device=d;p->memory=d->memory;p->allocation_bytes=table_at+16;
    void *address=NULL;
    VkResult rc=p->memory.allocate(p->memory.context,p->allocation_bytes,&address,&p->backing);
    if(rc!=VK_SUCCESS){free(p);return rc;}
    if(!address || !p->backing || ((uintptr_t)address&255u) ||
       ((uintptr_t)address>>32)!=2 || p->allocation_bytes>UINT64_C(0x300000000)-(uintptr_t)address) {
        rc=VK_ERROR_MEMORY_MAP_FAILED;goto failed;
    }
    memset(address,0,p->allocation_bytes);
    p->pair=address;p->global_table=(uint32_t *)((unsigned char *)address+table_at);
    struct ps5vk_graphics_pair *pair=p->pair;
    pair->runtime_arguments=arguments;
    if(ps5vk_runtime_shader_build(&pair->runtime_vertex,&input->vertex) ||
       ps5vk_runtime_shader_build(&pair->runtime_fragment,&input->fragment)) {
        rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
    }
    void *vs_code=(unsigned char *)address+vs_at,*fs_code=(unsigned char *)address+fs_at;
    memcpy(vs_code,input->vertex.machine_code,input->vertex.machine_code_size);
    memcpy(fs_code,input->fragment.machine_code,input->fragment.machine_code_size);
    void *vs=NULL,*fs=NULL;
    int32_t agc_rc=sceAgcCreateShader(&vs,&pair->runtime_vertex,vs_code);
    if(!agc_rc && vs==&pair->runtime_vertex) {
        agc_rc=sceAgcCreateShader(&fs,&pair->runtime_fragment,fs_code);
    }
    if(!agc_rc && fs==&pair->runtime_fragment) {
        agc_rc=sceAgcLinkShaders(&pair->cx,&pair->uc,NULL,vs,fs,4u);
    }
    if(agc_rc || vs!=&pair->runtime_vertex || fs!=&pair->runtime_fragment) {
        rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
    }
    pair->vertex_quantization=0x2d;pair->ready=1;
    rc=p->memory.flush(p->memory.context,p->backing,0,p->allocation_bytes);
    if(rc!=VK_SUCCESS)goto failed;
    *out=p;return VK_SUCCESS;
failed:
    if(p->backing)p->memory.release(p->memory.context,p->backing);
    free(p);return rc;
}
