#include "runtime_graphics_compiler.h"
#include "compilation_cache.h"
#include "spirv_graphics_interface.h"
#include <stdlib.h>
#include <string.h>

/* Private, in-process ABI. No pointers or legacy wrapper bytes are persisted.
 * Revisit version/options whenever the pinned compiler or supported profile
 * changes. This is not an on-disk Vulkan pipeline cache format. */
struct pair_payload {
    uint32_t version, reserved, primitive_type;
    uint64_t vertex_bytes, fragment_bytes;
    PsbcShaderMetadata vertex, fragment;
    uint64_t hull_bytes, domain_bytes;
    uint32_t patch_control_points, tess_output_points;
    PsbcShaderMetadata hull, domain;
};
struct pair_lease {
    struct ps5vk_runtime_graphics_program program; /* consumer view, first */
    struct ps5vk_cache_entry *entry;
    /* An uncached (tessellation) lease's owning allocation: the program the
     * adapter compiled, released with the lease instead of a cache entry. */
    void *uncached_program;
};

static uint32_t *pair_key(const struct ps5vk_graphics_key *key,
                          struct ps5vk_cache_key *cache_key)
{
    /* Validation bounds each module at 4M words and entry names at 63 bytes.
     * Fixed 64-byte strings and explicit lengths prevent concatenation aliases. */
    enum { DESCRIPTOR_WORDS=1+PS5VK_MAX_SETS*(1+PS5VK_MAX_BINDINGS*4),
        VERTEX_WORDS=2+16*4+32*5,
        /* The optional geometry stage is part of the program identity: a
         * pipeline that carries one must never reuse the pair compiled for the
         * vertex stage alone. */
        GEOMETRY_WORDS=1+1+16+1+64*4,
        /* The optional tessellation pair is part of the identity for the same
         * reason, and carries the two entry points, the two specialization maps
         * and the patch control point count the control stage's output vertices
         * and the evaluation stage's input arrays are derived from. */
        TESS_WORDS=1+1+1+16+16+1+1+64*4*2+1+1,
        BLEND_WORDS=11,
        HEADER_WORDS=48+PS5VK_MAX_PUSH_CONSTANT_DWORDS+2+64*4*2+DESCRIPTOR_WORDS+VERTEX_WORDS+GEOMETRY_WORDS+TESS_WORDS+BLEND_WORDS };
    const int has_geometry=ps5vk_graphics_has_geometry(key);
    const int has_tessellation=ps5vk_graphics_tessellation_key_valid(key);
    size_t count=HEADER_WORDS+key->vertex.word_count+key->fragment.word_count+
        (has_geometry?key->geometry.word_count:0)+
        (has_tessellation?key->tess_control.word_count+key->tess_eval.word_count:0);
    uint32_t *words=calloc(count,sizeof(*words));
    if(!words)return NULL;
    words[0]=6; /* explicit fixed blend state in the in-process key */
    words[13]=PSBC_SHADER_METADATA_VERSION;
    words[1]=(uint32_t)key->vertex.word_count;
    words[2]=(uint32_t)key->fragment.word_count;
    words[3]=key->topology;words[4]=key->color_format;
    words[5]=key->samples;words[6]=key->color_write_mask;
    words[7]=2; /* address32_hi */
    words[8]=1; /* optimise */
    words[9]=1; /* vertex NGG */
    words[10]=1; /* omit implicit PrimitiveID (FS verified by compiler adapter) */
    words[11]=PSBC_TARGET_PS5;
    words[12]=key->push_constant_size;
    memcpy(words+16,key->vertex.entry,strlen(key->vertex.entry));
    memcpy(words+32,key->fragment.entry,strlen(key->fragment.entry));
    memcpy(words+48,key->push_constant_stages,sizeof(key->push_constant_stages));
    size_t at=48+PS5VK_MAX_PUSH_CONSTANT_DWORDS;
    words[at++]=key->vertex.specialization_count;
    words[at++]=key->fragment.specialization_count;
    const struct ps5vk_graphics_module_key *modules[]={&key->vertex,&key->fragment};
    for(unsigned stage=0;stage<2;++stage)for(unsigned i=0;i<64;++i) {
        words[at++]=modules[stage]->specializations[i].constant_id;
        words[at++]=modules[stage]->specializations[i].size;
        memcpy(words+at,modules[stage]->specializations[i].data,8);at+=2;
    }
    words[at++]=key->descriptor_set_count;
    for(unsigned set=0;set<PS5VK_MAX_SETS;++set) {
        const struct ps5vk_set_signature *sig=set<key->descriptor_set_count?
            &key->descriptor_sets[set]:NULL;
        words[at++]=sig?sig->count:0;
        for(unsigned binding=0;binding<PS5VK_MAX_BINDINGS;++binding) {
            words[at++]=sig?sig->binding[binding].count:0;
            words[at++]=sig?sig->binding[binding].first:0;
            words[at++]=sig?sig->binding[binding].stages:0;
            words[at++]=sig?sig->type[binding]:0;
        }
    }
    words[at++]=key->vertex_binding_count;
    words[at++]=key->vertex_attribute_count;
    /* PSBC bakes type conversions, offsets and strides into vertex ISA.
     * Vulkan description order is irrelevant, so serialize by binding and
     * location, including explicit presence markers for empty slots. */
    for(unsigned binding=0;binding<16;++binding) {
        const VkVertexInputBindingDescription *b=NULL;
        for(unsigned i=0;i<key->vertex_binding_count;++i)
            if(key->vertex_bindings[i].binding==binding)b=&key->vertex_bindings[i];
        words[at++]=b!=NULL;words[at++]=b?b->binding:0;
        words[at++]=b?b->stride:0;words[at++]=b?b->inputRate:0;
    }
    for(unsigned location=0;location<32;++location) {
        const VkVertexInputAttributeDescription *a=NULL;
        for(unsigned i=0;i<key->vertex_attribute_count;++i)
            if(key->vertex_attributes[i].location==location)a=&key->vertex_attributes[i];
        words[at++]=a!=NULL;words[at++]=a?a->location:0;
        words[at++]=a?a->binding:0;words[at++]=a?a->format:0;words[at++]=a?a->offset:0;
    }
    /* The geometry stage, present or absent, with its entry point and
     * specialization map, so a three-stage pipeline can never alias a
     * two-stage one. */
    words[at++]=has_geometry?1u:0u;
    words[at++]=has_geometry?(uint32_t)key->geometry.word_count:0u;
    if(has_geometry) {
        memcpy(words+at,key->geometry.entry,strlen(key->geometry.entry));at+=16;
    } else at+=16;
    words[at++]=key->geometry.specialization_count;
    for(unsigned i=0;i<64;++i) {
        words[at++]=key->geometry.specializations[i].constant_id;
        words[at++]=key->geometry.specializations[i].size;
        memcpy(words+at,key->geometry.specializations[i].data,8);at+=2;
    }
    /* The tessellation pair, present or absent, with both entry points, both
     * specialization maps and the patch control points. A pipeline that adds,
     * removes or retargets the pair can never reuse another pipeline's pair. */
    words[at++]=has_tessellation?1u:0u;
    words[at++]=has_tessellation?(uint32_t)key->tess_control.word_count:0u;
    words[at++]=has_tessellation?(uint32_t)key->tess_eval.word_count:0u;
    if(has_tessellation) {
        memcpy(words+at,key->tess_control.entry,strlen(key->tess_control.entry));at+=16;
        memcpy(words+at,key->tess_eval.entry,strlen(key->tess_eval.entry));at+=16;
    } else at+=32;
    words[at++]=has_tessellation?key->tess_control.specialization_count:0u;
    words[at++]=has_tessellation?key->tess_eval.specialization_count:0u;
    const struct ps5vk_graphics_module_key *tess_modules[]={&key->tess_control,&key->tess_eval};
    for(unsigned stage=0;stage<2;++stage)for(unsigned i=0;i<64;++i) {
        words[at++]=has_tessellation?tess_modules[stage]->specializations[i].constant_id:0u;
        words[at++]=has_tessellation?tess_modules[stage]->specializations[i].size:0u;
        if(has_tessellation)
            memcpy(words+at,tess_modules[stage]->specializations[i].data,8);
        else
            memset(words+at,0,8);
        at+=2;
    }
    words[at++]=has_tessellation?key->patch_control_points:0u;
    words[at++]=0u; /* reserved, so the region stays a fixed size */
    words[at++]=key->blend_enable?1u:0u;
    if(key->blend_enable) {
        words[at++]=key->src_color_blend_factor;
        words[at++]=key->dst_color_blend_factor;
        words[at++]=key->color_blend_op;
        words[at++]=key->src_alpha_blend_factor;
        words[at++]=key->dst_alpha_blend_factor;
        words[at++]=key->alpha_blend_op;
        memcpy(words+at,key->blend_constants,sizeof(key->blend_constants));at+=4;
    } else at+=10; /* calloc canonicalizes ignored disabled state. */
    if(at!=HEADER_WORDS){free(words);return NULL;}
    memcpy(words+HEADER_WORDS,key->vertex.words,key->vertex.word_count*4);
    memcpy(words+HEADER_WORDS+key->vertex.word_count,key->fragment.words,key->fragment.word_count*4);
    size_t tail=HEADER_WORDS+key->vertex.word_count+key->fragment.word_count;
    if(has_geometry) {
        memcpy(words+HEADER_WORDS+key->vertex.word_count+key->fragment.word_count,
               key->geometry.words,key->geometry.word_count*4);
        tail+=key->geometry.word_count;
    }
    if(has_tessellation) {
        memcpy(words+tail,key->tess_control.words,key->tess_control.word_count*4);
        tail+=key->tess_control.word_count;
        memcpy(words+tail,key->tess_eval.words,key->tess_eval.word_count*4);
        tail+=key->tess_eval.word_count;
    }
    /* The key stream changed shape with the tessellation pair, so the name that
     * identifies the layout moves with it: a cache populated by the earlier
     * layout must never be read as if it had this one. */
    if(!ps5vk_cache_build_stage_key(words,count,"graphics-pair-v8",
            VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|
            (has_geometry?VK_SHADER_STAGE_GEOMETRY_BIT:0)|
            (has_tessellation?(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT|
                               VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT):0),0,cache_key)) {
        free(words);return NULL;
    }
    return words;
}

static struct ps5vk_cache_entry *store_pair(struct ps5vk_compilation_cache *cache,
    const struct ps5vk_cache_key *key,const uint32_t *words,
    const struct ps5vk_runtime_graphics_program *p)
{
    /* Header validation has bounded each ISA at 16MiB. */
    size_t vs=p->vertex.machine_code_size,fs=p->fragment.machine_code_size;
    size_t hs=p->hull.machine_code_size,ds=p->domain.machine_code_size;
    if(p->domain_legacy_valid || vs>16u*1024u*1024u || fs>16u*1024u*1024u ||
       hs>16u*1024u*1024u || ds>16u*1024u*1024u)return NULL;
    size_t bytes=sizeof(struct pair_payload)+vs+fs+hs+ds;
    struct pair_payload *payload=calloc(1,bytes);
    if(!payload)return NULL;
    payload->version=2;payload->vertex_bytes=vs;payload->fragment_bytes=fs;
    payload->primitive_type=p->primitive_type;
    payload->vertex=p->vertex.metadata;payload->fragment=p->fragment.metadata;
    payload->hull_bytes=hs;payload->domain_bytes=ds;
    payload->hull=p->hull.metadata;payload->domain=p->domain.metadata;
    payload->patch_control_points=p->patch_control_points;
    payload->tess_output_points=p->tess_output_points;
    if(vs)memcpy(payload+1,p->vertex.machine_code,vs);
    memcpy((char *)(payload+1)+vs,p->fragment.machine_code,fs);
    if(hs)memcpy((char *)(payload+1)+vs+fs,p->hull.machine_code,hs);
    if(ds)memcpy((char *)(payload+1)+vs+fs+hs,p->domain.machine_code,ds);
    struct ps5vk_cache_entry *entry=ps5vk_compilation_cache_insert_payload(
        cache,key,words,payload,bytes);
    free(payload);return entry;
}

VkResult ps5vk_runtime_graphics_cached_acquire(void *context,
    const struct ps5vk_graphics_key *key,const void **out)
{
    if(!out)return VK_ERROR_UNKNOWN;
    *out=NULL;
    if(!ps5vk_runtime_graphics_supported(key))return VK_ERROR_FEATURE_NOT_PRESENT;
#if defined(PS5VK_TESS_LEGACY_DOMAIN) && PS5VK_TESS_LEGACY_DOMAIN
    /* The experimental second legacy-domain image is intentionally not part
     * of the normal cache payload. Keep this diagnostic ownership explicit. */
    if(ps5vk_graphics_has_tessellation(key)) {
        const void *compiled=NULL;
        VkResult rc=ps5vk_runtime_graphics_compile(NULL,key,&compiled);
        if(rc!=VK_SUCCESS)return rc;
        struct pair_lease *uncached=calloc(1,sizeof(*uncached));
        if(!uncached){ps5vk_runtime_graphics_free(NULL,compiled);
            return VK_ERROR_OUT_OF_HOST_MEMORY;}
        uncached->entry=NULL; /* uncached: the program is the owner */
        uncached->uncached_program=(void *)compiled;
        uncached->program=*(struct ps5vk_runtime_graphics_program *)compiled;
        *out=&uncached->program;
        return VK_SUCCESS;
    }
#endif
    struct ps5vk_compilation_cache *cache=context;
    if(!cache)return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct ps5vk_cache_key identity;
    uint32_t *words=pair_key(key,&identity);
    if(!words)return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct ps5vk_cache_entry *entry=ps5vk_compilation_cache_lookup(cache,&identity,words);
    if(!entry) {
        const void *compiled=NULL;
        VkResult rc=ps5vk_runtime_graphics_compile(NULL,key,&compiled);
        if(rc!=VK_SUCCESS){free(words);return rc;}
        entry=store_pair(cache,&identity,words,compiled);
        ps5vk_runtime_graphics_free(NULL,compiled);
    }
    free(words);
    if(!entry)return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult failure=VK_ERROR_UNKNOWN;
    const struct pair_payload *payload=entry->payload_copy;
    uint32_t expected_primitive=0;
    const int tess=ps5vk_graphics_has_tessellation(key);
    if(!payload || entry->payload_bytes<sizeof(*payload) || payload->version!=2 ||
       ps5vk_agc_primitive_type(key->topology,&expected_primitive) ||
       payload->primitive_type!=expected_primitive ||
       !payload->fragment_bytes || (tess ?
           (payload->vertex_bytes || !payload->hull_bytes || !payload->domain_bytes ||
            payload->patch_control_points!=key->patch_control_points ||
            payload->tess_output_points!=ps5vk_spirv_tess_pair_output_points(key)) :
           (!payload->vertex_bytes || payload->hull_bytes || payload->domain_bytes ||
            payload->patch_control_points || payload->tess_output_points)) ||
       payload->vertex_bytes>16u*1024u*1024u || payload->fragment_bytes>16u*1024u*1024u ||
       payload->hull_bytes>16u*1024u*1024u || payload->domain_bytes>16u*1024u*1024u ||
       payload->vertex_bytes%4 || payload->fragment_bytes%4 || payload->hull_bytes%4 || payload->domain_bytes%4 ||
       sizeof(*payload)+payload->vertex_bytes+payload->fragment_bytes+
            payload->hull_bytes+payload->domain_bytes!=entry->payload_bytes)
        goto failed;
    struct pair_lease *lease=calloc(1,sizeof(*lease));
    if(!lease){failure=VK_ERROR_OUT_OF_HOST_MEMORY;goto failed;}
    lease->entry=entry;
    lease->program.primitive_type=payload->primitive_type;
    lease->program.vertex.metadata=payload->vertex;
    lease->program.vertex.machine_code=payload->vertex_bytes?(void *)(payload+1):NULL;
    lease->program.vertex.machine_code_size=(size_t)payload->vertex_bytes;
    lease->program.fragment.metadata=payload->fragment;
    lease->program.fragment.machine_code=(char *)(payload+1)+payload->vertex_bytes;
    lease->program.fragment.machine_code_size=(size_t)payload->fragment_bytes;
    {
        const int fragment_export=ps5vk_runtime_fragment_export(&payload->fragment);
        if(fragment_export<0){free(lease);goto failed;}
        lease->program.dual_source_export=
            fragment_export==PS5VK_RUNTIME_FRAGMENT_EXPORT_DUAL;
    }
    struct ps5vk_runtime_shader header;
    if(tess) {
        struct ps5vk_runtime_graphics_program *p=&lease->program;
        p->hull.metadata=payload->hull;p->domain.metadata=payload->domain;
        p->hull.machine_code=(char *)(payload+1)+payload->fragment_bytes;
        p->hull.machine_code_size=(size_t)payload->hull_bytes;
        p->domain.machine_code=(char *)p->hull.machine_code+payload->hull_bytes;
        p->domain.machine_code_size=(size_t)payload->domain_bytes;
        p->patch_control_points=payload->patch_control_points;
        p->tess_output_points=payload->tess_output_points;
        if(!ps5vk_runtime_graphics_feature_use_ok(&p->hull.metadata,&p->fragment.metadata,key->feature_mask) ||
           !ps5vk_runtime_graphics_feature_use_ok(&p->domain.metadata,&p->fragment.metadata,key->feature_mask) ||
           ps5vk_runtime_hull_build(&header,&p->hull) ||
           ps5vk_runtime_shader_build(&header,&p->domain) ||
           ps5vk_runtime_shader_build(&header,&p->fragment) ||
           ps5vk_runtime_hull_abi_build(&p->hull.metadata,&p->hull_arguments) ||
           ps5vk_runtime_draw_abi_build(&p->domain.metadata,&p->fragment.metadata,&p->arguments)) {
            free(lease);goto failed;
        }
        *out=p;return VK_SUCCESS;
    }
    /* A cached pair is accepted under the same capability gate the compiler
     * applied when it was compiled, so an application that did not enable a
     * distance feature cannot reach one through the cache. */
    if(!ps5vk_runtime_graphics_feature_use_ok(&payload->vertex,&payload->fragment,
            key->feature_mask) ||
       ps5vk_runtime_shader_build(&header,&lease->program.vertex) ||
       ps5vk_runtime_shader_build(&header,&lease->program.fragment) ||
       ps5vk_runtime_draw_abi_build(&payload->vertex,&payload->fragment,&lease->program.arguments)) {
        free(lease);goto failed;
    }
    *out=&lease->program;return VK_SUCCESS;
failed:
    ps5vk_cache_entry_release(cache,entry);return failure;
}

void ps5vk_runtime_graphics_cached_release(void *context,const void *data)
{
    if(!data)return;
    struct pair_lease *lease=(void *)data;
    if(lease->entry)ps5vk_cache_entry_release(context,lease->entry);
    else ps5vk_runtime_graphics_free(NULL,lease->uncached_program);
    free(lease);
}
