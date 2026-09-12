#include "runtime_graphics_compiler.h"
#include "spirv_graphics_interface.h"
#include <stdlib.h>
#include <string.h>

/* Structural/entry screening only; PSBC is responsible for shader validity.
 * Descriptor and push-constant storage have no runtime graphics ABI yet. */
static int module_supported(const struct ps5vk_graphics_module_key *m,unsigned model)
{
    if(!m->words || m->word_count<5 || m->word_count>4u*1024u*1024u || !m->entry ||
        !*m->entry || strlen(m->entry)>=64 || m->words[0]!=0x07230203u)return 0;
    unsigned entries=0,functions=0,in_function=0;
    for(size_t at=5;at<m->word_count;) {
        unsigned n=m->words[at]>>16,op=m->words[at]&65535u;
        if(!n || n>m->word_count-at)return 0;
        const uint32_t *w=m->words+at;
        if(op==15) {
            if(n<4)return 0;
            const char *name=(const char *)(w+3);
            if(!memchr(name,0,(n-3u)*4u))return 0;
            if(w[1]==model && !strcmp(name,m->entry))++entries;
        }
        if(op==59) {
            if(n<4)return 0;
            if(w[3]==0 || w[3]==2 || w[3]==9 || w[3]==12)return 0;
        }
        if(op==54) {
            if(n!=5 || in_function)return 0;
            in_function=1;++functions;
        } else if(op==56) {
            if(n!=1 || !in_function)return 0;
            in_function=0;
        }
        at+=n;
    }
    return entries==1 && functions && !in_function;
}

void ps5vk_runtime_graphics_free(void *context,const void *data)
{
    (void)context;
    struct ps5vk_runtime_graphics_program *p=(void *)data;
    if(!p)return;
    psbc_free_output(&p->vertex);psbc_free_output(&p->fragment);free(p);
}

int ps5vk_runtime_graphics_supported(const struct ps5vk_graphics_key *key)
{
    return key && module_supported(&key->vertex,0) && module_supported(&key->fragment,4) &&
        key->topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST &&
        key->color_format==VK_FORMAT_B8G8R8A8_UNORM && key->samples==VK_SAMPLE_COUNT_1_BIT &&
        key->color_write_mask==15 && !key->blend_enable && !key->vertex_binding_count &&
        !key->vertex_attribute_count && !key->descriptor_set_count &&
        ps5vk_spirv_graphics_interface(key);
}

VkResult ps5vk_runtime_graphics_compile(void *context,const struct ps5vk_graphics_key *key,const void **out)
{
    (void)context;
    if(!out)return VK_ERROR_UNKNOWN;
    *out=NULL;
    if(!ps5vk_runtime_graphics_supported(key))return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_runtime_graphics_program *p=calloc(1,sizeof(*p));
    if(!p)return VK_ERROR_OUT_OF_HOST_MEMORY;
    PsbcCompileOptions options={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_FRAGMENT,
        .entrypoint=key->fragment.entry,.optimise=true,.address32_hi=2,
        .primitive_type=4,.rasterization_samples=1};
    PsbcResult result=psbc_compile_shader(key->fragment.words,key->fragment.word_count*4u,&options,&p->fragment);
    VkResult failure=VK_ERROR_FEATURE_NOT_PRESENT;
    if(result!=PSBC_RESULT_OK)goto failed;
    /* Compile FS first so its actual metadata can prove PrimitiveID is unused. */
    if(p->fragment.metadata.input_semantic_count>PSBC_MAX_SEMANTICS)goto failed;
    for(unsigned i=0;i<p->fragment.metadata.input_semantic_count;++i)
        if((p->fragment.metadata.input_semantics[i]&255u)==PSBC_SEMANTIC_PRIMITIVE_ID)goto failed;
    options.stage=PSBC_STAGE_VERTEX;options.ngg=true;
    options.entrypoint=key->vertex.entry;options.omit_implicit_primitive_id=true;
    result=psbc_compile_shader(key->vertex.words,key->vertex.word_count*4u,&options,&p->vertex);
    if(result!=PSBC_RESULT_OK)goto failed;
    struct ps5vk_runtime_shader header;
    if(ps5vk_runtime_shader_build(&header,&p->vertex) ||
       ps5vk_runtime_shader_build(&header,&p->fragment) ||
       ps5vk_runtime_draw_abi_build(&p->vertex.metadata,&p->fragment.metadata,&p->arguments))goto failed;
    *out=p;
    return VK_SUCCESS;
failed:
    if(result==PSBC_RESULT_OUT_OF_MEMORY)failure=VK_ERROR_OUT_OF_HOST_MEMORY;
    ps5vk_runtime_graphics_free(NULL,p);
    return failure;
}
