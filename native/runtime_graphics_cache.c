#include "runtime_graphics_compiler.h"
#include "compilation_cache.h"
#include <stdlib.h>
#include <string.h>

/* Private, in-process ABI. No pointers or legacy wrapper bytes are persisted.
 * Revisit version/options whenever the pinned compiler or supported profile
 * changes. This is not an on-disk Vulkan pipeline cache format. */
struct pair_payload {
    uint32_t version, reserved;
    uint64_t vertex_bytes, fragment_bytes;
    PsbcShaderMetadata vertex, fragment;
};
struct pair_lease {
    struct ps5vk_runtime_graphics_program program; /* consumer view, first */
    struct ps5vk_cache_entry *entry;
};

static uint32_t *pair_key(const struct ps5vk_graphics_key *key,
                          struct ps5vk_cache_key *cache_key)
{
    /* Validation bounds each module at 4M words and entry names at 63 bytes.
     * Fixed 64-byte strings and explicit lengths prevent concatenation aliases. */
    size_t count=48+key->vertex.word_count+key->fragment.word_count;
    uint32_t *words=calloc(count,sizeof(*words));
    if(!words)return NULL;
    words[0]=1; /* adapter/profile version */
    words[1]=(uint32_t)key->vertex.word_count;
    words[2]=(uint32_t)key->fragment.word_count;
    words[3]=key->topology;words[4]=key->color_format;
    words[5]=key->samples;words[6]=key->color_write_mask;
    words[7]=2; /* address32_hi */
    words[8]=1; /* optimise */
    words[9]=1; /* vertex NGG */
    words[10]=1; /* omit implicit PrimitiveID (FS verified by compiler adapter) */
    words[11]=PSBC_TARGET_PS5;
    memcpy(words+16,key->vertex.entry,strlen(key->vertex.entry));
    memcpy(words+32,key->fragment.entry,strlen(key->fragment.entry));
    memcpy(words+48,key->vertex.words,key->vertex.word_count*4);
    memcpy(words+48+key->vertex.word_count,key->fragment.words,key->fragment.word_count*4);
    if(!ps5vk_cache_build_stage_key(words,count,"graphics-pair-v1",
            VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,cache_key)) {
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
    size_t bytes=sizeof(struct pair_payload)+vs+fs;
    struct pair_payload *payload=calloc(1,bytes);
    if(!payload)return NULL;
    payload->version=1;payload->vertex_bytes=vs;payload->fragment_bytes=fs;
    payload->vertex=p->vertex.metadata;payload->fragment=p->fragment.metadata;
    memcpy(payload+1,p->vertex.machine_code,vs);
    memcpy((char *)(payload+1)+vs,p->fragment.machine_code,fs);
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
    if(!payload || entry->payload_bytes<sizeof(*payload) || payload->version!=1 ||
       !payload->vertex_bytes || !payload->fragment_bytes ||
       payload->vertex_bytes>16u*1024u*1024u || payload->fragment_bytes>16u*1024u*1024u ||
       payload->vertex_bytes%4 || payload->fragment_bytes%4 ||
       sizeof(*payload)+payload->vertex_bytes+payload->fragment_bytes!=entry->payload_bytes)
        goto failed;
    struct pair_lease *lease=calloc(1,sizeof(*lease));
    if(!lease){failure=VK_ERROR_OUT_OF_HOST_MEMORY;goto failed;}
    lease->entry=entry;
    lease->program.vertex.metadata=payload->vertex;
    lease->program.vertex.machine_code=(void *)(payload+1);
    lease->program.vertex.machine_code_size=(size_t)payload->vertex_bytes;
    lease->program.fragment.metadata=payload->fragment;
    lease->program.fragment.machine_code=(char *)(payload+1)+payload->vertex_bytes;
    lease->program.fragment.machine_code_size=(size_t)payload->fragment_bytes;
    struct ps5vk_runtime_shader header;
    if(ps5vk_runtime_shader_build(&header,&lease->program.vertex) ||
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
    ps5vk_cache_entry_release(context,lease->entry);
    free(lease);
}
