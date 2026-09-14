#include "vertex_fetch.h"
#include "vertex_descriptor.h"
#include "graphics_formats.h"
#include <string.h>
VkResult ps5vk_vertex_fetch_span(VkDevice d,const struct ps5vk_graphics_key *key,
    const struct ps5vk_operation *op,struct ps5vk_vertex_fetch *out)
{
    if(!d || !key || !op || !out || (op->type!=PS5VK_DRAW && op->type!=PS5VK_DRAW_INDEXED))return VK_ERROR_UNKNOWN;
    if(key->vertex_binding_count!=1 || !key->vertex_bindings || !key->vertex_attribute_count ||
       key->vertex_attribute_count>32 || !key->vertex_attributes)return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkVertexInputBindingDescription *b=key->vertex_bindings;
    if(b->binding || b->inputRate!=VK_VERTEX_INPUT_RATE_VERTEX || !b->stride ||
       b->stride>0x3fff)return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t extent=0;
    for(uint32_t i=0;i<key->vertex_attribute_count;++i) {
        const VkVertexInputAttributeDescription *a=&key->vertex_attributes[i];
        uint32_t size=ps5vk_vertex_format_size(a->format);
        if(!size)return VK_ERROR_FEATURE_NOT_PRESENT;
        if(a->binding || a->location>=32 || a->offset>UINT32_MAX-size)return VK_ERROR_UNKNOWN;
        for(uint32_t j=0;j<i;++j)if(key->vertex_attributes[j].location==a->location)return VK_ERROR_UNKNOWN;
        if(extent<a->offset+size)extent=a->offset+size;
    }
    if(!(op->type==PS5VK_DRAW_INDEXED?op->index_count:op->vertex_count) || !op->instance_count) {
        *out=(struct ps5vk_vertex_fetch){0};return VK_SUCCESS;
    }
    const struct ps5vk_vertex_binding *bound=&op->vertices[0];
    if(!ps5vk_buffer_usage(d,bound->buffer,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))return VK_ERROR_UNKNOWN;
    void *address;VkDeviceSize bytes;
    VkResult rc=ps5vk_buffer_span(d,bound->buffer,bound->offset,VK_WHOLE_SIZE,&address,&bytes);
    if(rc!=VK_SUCCESS)return rc;
    if(bytes<extent)return VK_ERROR_UNKNOWN;
    uint64_t records=1+(bytes-extent)/b->stride;
    /* Widen before adding: a wrapped firstVertex+vertexCount must not hide an
     * out-of-range typed fetch. Descriptor bounds aren't a correctness oracle. */
    /* Indexed contents remain GPU inputs, not CPU-expanded geometry. Their
     * values plus signed baseVertex must satisfy Vulkan valid usage; the SRD
     * still bounds the actual vertex buffer. No robust-access feature claim. */
    if(op->type==PS5VK_DRAW && (uint64_t)op->first_vertex+op->vertex_count>records)return VK_ERROR_UNKNOWN;
    *out=(struct ps5vk_vertex_fetch){address,bytes,b->stride,extent};return VK_SUCCESS;
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
