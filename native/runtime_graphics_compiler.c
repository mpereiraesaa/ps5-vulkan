/*
 * Copyright (C) 2026 BlackBearReloaded
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The GFX1013/PSBC vertex format mapping is adapted from the vertex-format
 * contract in BlackBearReloaded's ps5-opengl, src/gallium/ps5/ps5_screen.c at
 * commit 7f9bfabdddb187a11e4401058eba8c9e55194d0a (GPL-3.0-or-later).
 */
#include "runtime_graphics_compiler.h"
#include "spirv_graphics_interface.h"
#include "descriptor_table_layout.h"
#include <stdlib.h>
#include <string.h>

/* Structural/entry screening only; PSBC is responsible for shader validity. */
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
            /* Sampled images (UniformConstant) and uniform blocks (Uniform) are
             * admitted only through the separately checked descriptor profile,
             * which now delivers both record kinds from one table. Storage
             * buffers stay outside this bounded graphics compiler. */
            if(w[3]==12)return 0;
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

int ps5vk_runtime_graphics_feature_use_ok(const PsbcShaderMetadata *pre_raster,
    const PsbcShaderMetadata *fragment,uint32_t feature_mask)
{
    if(!pre_raster || !fragment)return 0;
    if(pre_raster->clip_distance_mask &&
       !(feature_mask & PS5VK_FEATURE_SHADER_CLIP_DISTANCE))return 0;
    if(pre_raster->cull_distance_mask &&
       !(feature_mask & PS5VK_FEATURE_SHADER_CULL_DISTANCE))return 0;
    /* A merged pre-raster stage that reports the geometry source stage is a
     * geometry pipeline, and it needs the feature like any other stage. */
    if(pre_raster->source_stage==PSBC_STAGE_GEOMETRY &&
       !(feature_mask & PS5VK_FEATURE_GEOMETRY_SHADER))return 0;
    if(pre_raster->source_stage==PSBC_STAGE_TESS_EVAL &&
       !(feature_mask & PS5VK_FEATURE_TESSELLATION_SHADER))return 0;
    return 1;
}

static int descriptor_profile_supported(const struct ps5vk_graphics_key *key)
{
    struct ps5vk_descriptor_table_layout tables;
    if (ps5vk_descriptor_table_layout_build(key->descriptor_set_count,
            key->descriptor_sets,&tables)!=VK_SUCCESS) return 0;
    if(tables.binding_count>PSBC_MAX_DESCRIPTOR_BINDINGS)return 0;
    for(unsigned s=0;s<key->descriptor_set_count;++s)
        for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
            const struct ps5vk_set_signature *set=&key->descriptor_sets[s];
            /* The canonical table validates the full core visibility mask;
             * descriptor options project it onto the executing VS/FS stage.
             * Combined image samplers coexist with the mandatory uniform-buffer
             * resources a real pipeline layout carries, and an input attachment
             * is admitted as fragment-only resource-only image data: it is read
             * by subpassLoad in a fragment shader, so a layout that exposes it
             * to the vertex stage is refused rather than projected onto a stage
             * that cannot read it. Every other descriptor type stays outside the
             * profile instead of being half-delivered. */
            if(set->binding[b].count &&
                (!(set->binding[b].stages&(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT)) ||
                (set->type[b]!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                 set->type[b]!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                 set->type[b]!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
                 set->type[b]!=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)))return 0;
            if(set->binding[b].count &&
               set->type[b]==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT &&
               set->binding[b].stages!=VK_SHADER_STAGE_FRAGMENT_BIT)return 0;
        }
    return 1;
}

int ps5vk_runtime_graphics_supported(const struct ps5vk_graphics_key *key)
{
    if(!key || key->vertex.specialization_count>64 || key->fragment.specialization_count>64 ||
       key->push_constant_size>PS5VK_MAX_PUSH_CONSTANT_BYTES)return 0;
    /* The merged vertex+geometry pre-raster stage is the next slice: until the
     * adapter can compile and package it, a key with a geometry stage is
     * refused instead of silently compiling its vertex stage alone. */
    if(ps5vk_graphics_has_geometry(key))return 0;
    for(unsigned i=0;i<PS5VK_MAX_PUSH_CONSTANT_DWORDS;++i)
        if(key->push_constant_stages[i]&~(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT))return 0;
    if(key->vertex_binding_count>16 || key->vertex_attribute_count>PSBC_MAX_VERTEX_ATTRIBUTES ||
       (key->vertex_binding_count && !key->vertex_bindings) ||
       (key->vertex_attribute_count && !key->vertex_attributes))return 0;
    for(uint32_t i=0;i<key->vertex_binding_count;++i) {
        const VkVertexInputBindingDescription *b=&key->vertex_bindings[i];
        if(b->binding>=16 || b->inputRate!=VK_VERTEX_INPUT_RATE_VERTEX ||
           !b->stride || b->stride>0x3fff)return 0;
        for(uint32_t j=0;j<i;++j)if(key->vertex_bindings[j].binding==b->binding)return 0;
    }
    for(uint32_t i=0;i<key->vertex_attribute_count;++i) {
        const VkVertexInputAttributeDescription *a=&key->vertex_attributes[i];
        if(a->binding>=16 || a->location>=PSBC_MAX_VERTEX_ATTRIBUTES)return 0;
        unsigned found=0;
        for(uint32_t j=0;j<key->vertex_binding_count;++j)found|=key->vertex_bindings[j].binding==a->binding;
        if(!found)return 0;
        for(uint32_t j=0;j<i;++j)if(key->vertex_attributes[j].location==a->location)return 0;
    }
    uint32_t primitive_type=0;
    return module_supported(&key->vertex,0) && module_supported(&key->fragment,4) &&
        !ps5vk_agc_primitive_type(key->topology,&primitive_type) &&
        (key->color_format==VK_FORMAT_B8G8R8A8_UNORM ||
         key->color_format==VK_FORMAT_R8G8B8A8_UNORM) && key->samples==VK_SAMPLE_COUNT_1_BIT &&
        key->color_write_mask==15 && !key->blend_enable &&
        key->vertex_binding_count<=16 && key->vertex_attribute_count<=PSBC_MAX_VERTEX_ATTRIBUTES &&
        (!key->vertex_binding_count || key->vertex_bindings) &&
        (!key->vertex_attribute_count || key->vertex_attributes) &&
        descriptor_profile_supported(key) &&
        ps5vk_spirv_graphics_interface(key);
}

static PsbcVertexFormat vertex_format(VkFormat format)
{
    switch(format) {
    case VK_FORMAT_R32_SFLOAT: return PSBC_VERTEX_FORMAT_R32_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT: return PSBC_VERTEX_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R32G32B32_SFLOAT: return PSBC_VERTEX_FORMAT_R32G32B32_FLOAT;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return PSBC_VERTEX_FORMAT_R32G32B32A32_FLOAT;
    case VK_FORMAT_R32_SINT: return PSBC_VERTEX_FORMAT_R32_SINT;
    case VK_FORMAT_R32G32_SINT: return PSBC_VERTEX_FORMAT_R32G32_SINT;
    case VK_FORMAT_R32G32B32_SINT: return PSBC_VERTEX_FORMAT_R32G32B32_SINT;
    case VK_FORMAT_R32G32B32A32_SINT: return PSBC_VERTEX_FORMAT_R32G32B32A32_SINT;
    case VK_FORMAT_R32_UINT: return PSBC_VERTEX_FORMAT_R32_UINT;
    case VK_FORMAT_R32G32_UINT: return PSBC_VERTEX_FORMAT_R32G32_UINT;
    case VK_FORMAT_R32G32B32_UINT: return PSBC_VERTEX_FORMAT_R32G32B32_UINT;
    case VK_FORMAT_R32G32B32A32_UINT: return PSBC_VERTEX_FORMAT_R32G32B32A32_UINT;
    case VK_FORMAT_R8_UNORM: return PSBC_VERTEX_FORMAT_R8_UNORM;
    case VK_FORMAT_R8_SNORM: return PSBC_VERTEX_FORMAT_R8_SNORM;
    case VK_FORMAT_R8_UINT: return PSBC_VERTEX_FORMAT_R8_UINT;
    case VK_FORMAT_R8_SINT: return PSBC_VERTEX_FORMAT_R8_SINT;
    case VK_FORMAT_R8G8_UNORM: return PSBC_VERTEX_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8G8_SNORM: return PSBC_VERTEX_FORMAT_R8G8_SNORM;
    case VK_FORMAT_R8G8_UINT: return PSBC_VERTEX_FORMAT_R8G8_UINT;
    case VK_FORMAT_R8G8_SINT: return PSBC_VERTEX_FORMAT_R8G8_SINT;
    case VK_FORMAT_R8G8B8A8_UNORM: return PSBC_VERTEX_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return PSBC_VERTEX_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return PSBC_VERTEX_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_A8B8G8R8_SNORM_PACK32: return PSBC_VERTEX_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_A8B8G8R8_UINT_PACK32: return PSBC_VERTEX_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_A8B8G8R8_SINT_PACK32: return PSBC_VERTEX_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return PSBC_VERTEX_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16_UNORM: return PSBC_VERTEX_FORMAT_R16_UNORM;
    case VK_FORMAT_R16_SNORM: return PSBC_VERTEX_FORMAT_R16_SNORM;
    case VK_FORMAT_R16_UINT: return PSBC_VERTEX_FORMAT_R16_UINT;
    case VK_FORMAT_R16_SINT: return PSBC_VERTEX_FORMAT_R16_SINT;
    case VK_FORMAT_R16_SFLOAT: return PSBC_VERTEX_FORMAT_R16_FLOAT;
    case VK_FORMAT_R16G16_UNORM: return PSBC_VERTEX_FORMAT_R16G16_UNORM;
    case VK_FORMAT_R16G16_SNORM: return PSBC_VERTEX_FORMAT_R16G16_SNORM;
    case VK_FORMAT_R16G16_UINT: return PSBC_VERTEX_FORMAT_R16G16_UINT;
    case VK_FORMAT_R16G16_SINT: return PSBC_VERTEX_FORMAT_R16G16_SINT;
    case VK_FORMAT_R16G16_SFLOAT: return PSBC_VERTEX_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R16G16B16A16_UNORM: return PSBC_VERTEX_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16A16_SNORM: return PSBC_VERTEX_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16A16_UINT: return PSBC_VERTEX_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16A16_SINT: return PSBC_VERTEX_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return PSBC_VERTEX_FORMAT_R16G16B16A16_FLOAT;
    default: return PSBC_VERTEX_FORMAT_NONE;
    }
}

VkResult ps5vk_runtime_graphics_descriptor_options(const struct ps5vk_graphics_key *key,
    VkShaderStageFlagBits stage,PsbcCompileOptions *options)
{
    if(!key || !options || (stage!=VK_SHADER_STAGE_VERTEX_BIT &&
            stage!=VK_SHADER_STAGE_FRAGMENT_BIT))return VK_ERROR_UNKNOWN;
    struct ps5vk_descriptor_table_layout tables;
    VkResult rc=ps5vk_descriptor_table_layout_build(key->descriptor_set_count,
        key->descriptor_sets,&tables);
    if(rc!=VK_SUCCESS)return rc;
    PsbcDescriptorBinding bindings[PSBC_MAX_DESCRIPTOR_BINDINGS]={0};
    uint32_t count=0;
    for(uint32_t s=0;s<key->descriptor_set_count;++s)
        for(uint32_t b=0;b<PS5VK_MAX_BINDINGS;++b) {
            const struct ps5vk_binding *source=&key->descriptor_sets[s].binding[b];
            /* An input attachment is fragment-visible resource-only image data.
             * A declaration that exposes it to any other stage is not a
             * declaration this profile can honour, so it fails closed here
             * rather than being projected into a table that no longer says what
             * the caller wrote. */
            if(source->count &&
               key->descriptor_sets[s].type[b]==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT &&
               source->stages!=VK_SHADER_STAGE_FRAGMENT_BIT)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            if(!source->count || !(source->stages&stage))continue;
            if(count==PSBC_MAX_DESCRIPTOR_BINDINGS)return VK_ERROR_FEATURE_NOT_PRESENT;
            PsbcDescriptorType type;
            switch(key->descriptor_sets[s].type[b]) {
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: type=PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER;break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: type=PSBC_DESCRIPTOR_UNIFORM_BUFFER;break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: type=PSBC_DESCRIPTOR_STORAGE_BUFFER;break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: type=PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER;break;
            /* Fragment-only resource-only image data: the canonical table gives
             * this role its own 32-byte record, so the PSBC type is the
             * resource-only one and never the combined T#/S# pair. The stage
             * projection above already keeps it out of every other stage. */
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: type=PSBC_DESCRIPTOR_INPUT_ATTACHMENT;break;
            default:return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            bindings[count++]=(PsbcDescriptorBinding){.set=s,.binding=b,.type=type,
                .array_size=source->count,.offset=tables.binding[s][b].byte_offset,
                .stride=tables.binding[s][b].byte_stride};
        }
    memcpy(options->descriptor_bindings,bindings,sizeof(bindings));
    options->descriptor_binding_count=count;
    /* The layout is a declaration, not evidence of use: the compiler reports
     * the sets and bindings the optimized NIR really dereferences, so a layout
     * set no shader touches cannot acquire a native descriptor dependency. */
    options->static_descriptor_use=true;
    return VK_SUCCESS;
}

static int apply_parameters(PsbcCompileOptions *options,
                            const struct ps5vk_graphics_module_key *module,
                            const struct ps5vk_graphics_key *key,VkShaderStageFlagBits stage)
{
    options->specialization_constant_count=module->specialization_count;
    for(uint32_t i=0;i<module->specialization_count;++i) {
        if(!module->specializations[i].size || module->specializations[i].size>8)return 0;
        options->specialization_constants[i].constant_id=module->specializations[i].constant_id;
        options->specialization_constants[i].size=module->specializations[i].size;
        memcpy(options->specialization_constants[i].data,module->specializations[i].data,8);
    }
    for(uint32_t i=0;i<module->specialization_count;++i)
        for(uint32_t j=0;j<i;++j)
            if(module->specializations[i].constant_id==module->specializations[j].constant_id)return 0;
    options->force_indirect_push_constants=false;
    for(unsigned i=0;i<PS5VK_MAX_PUSH_CONSTANT_DWORDS;++i)
        if(key->push_constant_stages[i]&stage)options->force_indirect_push_constants=true;
    options->vertex_attribute_count=0;
    if(ps5vk_runtime_graphics_descriptor_options(key,stage,options)!=VK_SUCCESS)return 0;
    if(stage==VK_SHADER_STAGE_VERTEX_BIT) {
        for(uint32_t i=0;i<key->vertex_attribute_count;++i) {
            const VkVertexInputAttributeDescription *source=&key->vertex_attributes[i];
            const VkVertexInputBindingDescription *binding=NULL;
            for(uint32_t j=0;j<key->vertex_binding_count;++j)
                if(key->vertex_bindings[j].binding==source->binding)binding=&key->vertex_bindings[j];
            PsbcVertexFormat format=vertex_format(source->format);
            if(!binding || !format || binding->inputRate!=VK_VERTEX_INPUT_RATE_VERTEX ||
               !binding->stride || binding->stride>0x3fff)
                return 0;
            options->vertex_attributes[options->vertex_attribute_count++]=(PsbcVertexAttribute){
                .location=(uint8_t)source->location,.binding=(uint8_t)source->binding,
                .format=format,.offset=source->offset,.stride=binding->stride,
                /* Vulkan vertex bindings and offsets are byte-granular. */
                .alignment=1,.instance_divisor=0};
        }
    }
    return 1;
}

static int push_metadata_supported(const PsbcShaderMetadata *metadata,
                                   const struct ps5vk_graphics_key *key,
                                   VkShaderStageFlagBits stage)
{
    if(!metadata->push_constants_valid)return !metadata->push_constant_size;
    if(!metadata->push_constant_size || metadata->push_constant_size>key->push_constant_size)return 0;
    for(uint32_t i=0;i<(metadata->push_constant_size+3u)/4u;++i)
        if(!(key->push_constant_stages[i]&stage))return 0;
    return 1;
}

VkResult ps5vk_runtime_graphics_compile(void *context,const struct ps5vk_graphics_key *key,const void **out)
{
    (void)context;
    if(!out)return VK_ERROR_UNKNOWN;
    *out=NULL;
    if(!ps5vk_runtime_graphics_supported(key))return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_runtime_graphics_program *p=calloc(1,sizeof(*p));
    if(!p)return VK_ERROR_OUT_OF_HOST_MEMORY;
    PsbcResult result=PSBC_RESULT_INTERNAL_ERROR;
    VkResult failure=VK_ERROR_FEATURE_NOT_PRESENT;
    PsbcCompileOptions options={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_FRAGMENT,
        .entrypoint=key->fragment.entry,.optimise=true,.address32_hi=2,
        .primitive_type=0,.rasterization_samples=1};
    if(ps5vk_agc_primitive_type(key->topology,&options.primitive_type))goto failed;
    if(!apply_parameters(&options,&key->fragment,key,VK_SHADER_STAGE_FRAGMENT_BIT))goto failed;
    result=psbc_compile_shader(key->fragment.words,key->fragment.word_count*4u,&options,&p->fragment);
    if(result!=PSBC_RESULT_OK)goto failed;
    /* Compile FS first so its actual metadata can prove PrimitiveID is unused. */
    if(p->fragment.metadata.input_semantic_count>PSBC_MAX_SEMANTICS)goto failed;
    for(unsigned i=0;i<p->fragment.metadata.input_semantic_count;++i)
        if((p->fragment.metadata.input_semantics[i]&255u)==PSBC_SEMANTIC_PRIMITIVE_ID)goto failed;
    options.stage=PSBC_STAGE_VERTEX;options.ngg=true;
    options.entrypoint=key->vertex.entry;options.omit_implicit_primitive_id=true;
    if(!apply_parameters(&options,&key->vertex,key,VK_SHADER_STAGE_VERTEX_BIT))goto failed;
    result=psbc_compile_shader(key->vertex.words,key->vertex.word_count*4u,&options,&p->vertex);
    if(result!=PSBC_RESULT_OK)goto failed;
    if(!push_metadata_supported(&p->vertex.metadata,key,VK_SHADER_STAGE_VERTEX_BIT) ||
       !push_metadata_supported(&p->fragment.metadata,key,VK_SHADER_STAGE_FRAGMENT_BIT))goto failed;
    /* The compiled stages are the usage evidence: refuse a pair that really
     * consumes a capability the application never enabled. */
    if(!ps5vk_runtime_graphics_feature_use_ok(&p->vertex.metadata,&p->fragment.metadata,
        key->feature_mask))goto failed;
    struct ps5vk_runtime_shader header;
    if(ps5vk_runtime_shader_build(&header,&p->vertex) ||
       ps5vk_runtime_shader_build(&header,&p->fragment) ||
       ps5vk_runtime_draw_abi_build(&p->vertex.metadata,&p->fragment.metadata,&p->arguments))goto failed;
    /* Recorded from the same resolved value the compiler was given, so the
     * native create path can refuse a pipeline that asks to link this pair for
     * a different primitive. */
    p->primitive_type=options.primitive_type;
    *out=p;
    return VK_SUCCESS;
failed:
    if(result==PSBC_RESULT_OUT_OF_MEMORY)failure=VK_ERROR_OUT_OF_HOST_MEMORY;
    ps5vk_runtime_graphics_free(NULL,p);
    return failure;
}
