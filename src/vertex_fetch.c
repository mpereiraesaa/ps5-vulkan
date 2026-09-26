#include "vertex_fetch.h"
#include "vertex_descriptor.h"
#include "graphics_formats.h"
#include <string.h>
VkResult ps5vk_vertex_fetch_used_spans(VkDevice d,const struct ps5vk_graphics_key *key,
    const struct ps5vk_operation *op,uint32_t usage_mask,struct ps5vk_vertex_fetch_table *out)
{
    if(!d || !key || !op || !out || (op->type!=PS5VK_DRAW && op->type!=PS5VK_DRAW_INDEXED))return VK_ERROR_UNKNOWN;
    if(!key->vertex_binding_count || key->vertex_binding_count>PS5VK_MAX_VERTEX_BINDINGS ||
       !key->vertex_bindings || !key->vertex_attribute_count ||
       key->vertex_attribute_count>32 || !key->vertex_attributes)return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkVertexInputBindingDescription *bindings[PS5VK_MAX_VERTEX_BINDINGS]={0};
    struct ps5vk_vertex_fetch_table result={0};
    for(uint32_t i=0;i<key->vertex_binding_count;++i) {
        const VkVertexInputBindingDescription *b=&key->vertex_bindings[i];
        if(b->binding>=PS5VK_MAX_VERTEX_BINDINGS || bindings[b->binding])return VK_ERROR_UNKNOWN;
        if(b->inputRate!=VK_VERTEX_INPUT_RATE_VERTEX || !b->stride ||
           b->stride>0x3fff)return VK_ERROR_FEATURE_NOT_PRESENT;
        bindings[b->binding]=b;
    }
    for(uint32_t i=0;i<key->vertex_attribute_count;++i) {
        const VkVertexInputAttributeDescription *a=&key->vertex_attributes[i];
        uint32_t size=ps5vk_vertex_format_size(a->format);
        if(!size)return VK_ERROR_FEATURE_NOT_PRESENT;
        if(a->binding>=PS5VK_MAX_VERTEX_BINDINGS || !bindings[a->binding] ||
           a->location>=32 || a->offset>UINT32_MAX-size)return VK_ERROR_UNKNOWN;
        for(uint32_t j=0;j<i;++j)if(key->vertex_attributes[j].location==a->location)return VK_ERROR_UNKNOWN;
        struct ps5vk_vertex_fetch *fetch=&result.bindings[a->binding];
        if(fetch->attribute_extent<a->offset+size)fetch->attribute_extent=a->offset+size;
        fetch->stride=bindings[a->binding]->stride;
        if(result.count<=a->binding)result.count=a->binding+1;
    }
    if(usage_mask>>PS5VK_MAX_VERTEX_BINDINGS)return VK_ERROR_UNKNOWN;
    for(uint32_t i=0;i<PS5VK_MAX_VERTEX_BINDINGS;++i)
        if((usage_mask&(UINT32_C(1)<<i)) && !result.bindings[i].attribute_extent)return VK_ERROR_UNKNOWN;
    if(!(op->type==PS5VK_DRAW_INDEXED?op->index_count:op->vertex_count) || !op->instance_count) {
        memset(result.bindings,0,sizeof(result.bindings));
        *out=result;return VK_SUCCESS;
    }
    for(uint32_t i=0;i<result.count;++i) {
        struct ps5vk_vertex_fetch *fetch=&result.bindings[i];
        if(usage_mask && !(usage_mask&(UINT32_C(1)<<i))) {
            *fetch=(struct ps5vk_vertex_fetch){0};continue;
        }
        if(!fetch->attribute_extent)continue;
        const struct ps5vk_vertex_binding *bound=&op->vertices[i];
        /* robustness2 nullDescriptor: a VK_NULL_HANDLE binding keeps a null
         * address, which becomes the all-zero SRD. NUM_RECORDS zero puts
         * every fetch out of range, so each attribute reads zero. */
        if(!bound->buffer && (d->enabled_features_t09 & PS5VK_T09_FEATURE_NULL_DESCRIPTOR)) {
            fetch->address=NULL;fetch->bytes=0;continue;
        }
        if(!ps5vk_buffer_usage(d,bound->buffer,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))return VK_ERROR_UNKNOWN;
        void *address;VkDeviceSize bytes;
        VkResult rc=ps5vk_buffer_span(d,bound->buffer,bound->offset,VK_WHOLE_SIZE,&address,&bytes);
        if(rc!=VK_SUCCESS)return rc;
        if(bytes<fetch->attribute_extent)return VK_ERROR_UNKNOWN;
        uint64_t records=1+(bytes-fetch->attribute_extent)/fetch->stride;
        /* Widen before adding: a wrapped firstVertex+vertexCount must not hide an
         * out-of-range typed fetch. Descriptor bounds aren't a correctness oracle. */
        /* Indexed contents remain GPU inputs, not CPU-expanded geometry. Their
         * values plus signed baseVertex must satisfy Vulkan valid usage; the SRD
         * still bounds the actual vertex buffer. No robust-access feature claim. */
        /* robustBufferAccess2 makes an out-of-range vertex read defined: the
         * SRD's record count bounds the fetch and the rest read zero. Without
         * it, a non-indexed draw past the buffer is still refused. */
        if(op->type==PS5VK_DRAW && (uint64_t)op->first_vertex+op->vertex_count>records &&
           !(d->enabled_features_t09 & PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2))
            return VK_ERROR_UNKNOWN;
        fetch->address=address;fetch->bytes=bytes;
    }
    *out=result;return VK_SUCCESS;
}
VkResult ps5vk_vertex_fetch_spans(VkDevice d,const struct ps5vk_graphics_key *key,
    const struct ps5vk_operation *op,struct ps5vk_vertex_fetch_table *out)
{ return ps5vk_vertex_fetch_used_spans(d,key,op,0,out); }
VkResult ps5vk_vertex_fetch_span(VkDevice d,const struct ps5vk_graphics_key *key,
    const struct ps5vk_operation *op,struct ps5vk_vertex_fetch *out)
{
    if(!out)return VK_ERROR_UNKNOWN;
    struct ps5vk_vertex_fetch_table table;
    VkResult rc=ps5vk_vertex_fetch_spans(d,key,op,&table);
    if(rc!=VK_SUCCESS)return rc;
    if(table.count!=1)return VK_ERROR_FEATURE_NOT_PRESENT;
    *out=table.bindings[0];return VK_SUCCESS;
}
VkResult ps5vk_vertex_fetch_descriptor(VkDevice d,const struct ps5vk_graphics_key *key,
    const struct ps5vk_operation *op,uint32_t out[4])
{
    if(!out)return VK_ERROR_UNKNOWN;
    struct ps5vk_vertex_fetch fetch;
    VkResult rc=ps5vk_vertex_fetch_span(d,key,op,&fetch);if(rc!=VK_SUCCESS)return rc;
    if(!fetch.address)return VK_SUCCESS;
    uint32_t words[4];
    if(ps5vk_vertex_descriptor(words,(uintptr_t)fetch.address,fetch.bytes,
        fetch.stride,fetch.attribute_extent))return VK_ERROR_UNKNOWN;
    memcpy(out,words,sizeof(words));return VK_SUCCESS;
}
