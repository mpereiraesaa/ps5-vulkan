#include "index_fetch.h"
VkResult ps5vk_index_fetch_prepare(VkDevice d,const struct ps5vk_operation *op,
    struct ps5vk_index_fetch *out)
{
    if(!d || !op || !out || op->type!=PS5VK_DRAW_INDEXED)return VK_ERROR_UNKNOWN;
    unsigned size;
    switch(op->indices.type) {
    case VK_INDEX_TYPE_UINT16:size=2;break;
    case VK_INDEX_TYPE_UINT32:size=4;break;
    default:return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if(!op->index_count || !op->instance_count)return VK_SUCCESS;
    if(!ps5vk_buffer_usage(d,op->indices.buffer,VK_BUFFER_USAGE_INDEX_BUFFER_BIT))return VK_ERROR_UNKNOWN;
    void *base;VkDeviceSize bytes;
    VkResult rc=ps5vk_buffer_span(d,op->indices.buffer,op->indices.offset,VK_WHOLE_SIZE,&base,&bytes);
    if(rc!=VK_SUCCESS)return rc;
    uint64_t address=(uintptr_t)base;
    const uint64_t limit=UINT64_C(1)<<48;
    if(!address || address>=limit || address%size || bytes>limit-address ||
        (uint64_t)op->first_index+op->index_count>bytes/size)return VK_ERROR_UNKNOWN;
    const uint64_t advance=(uint64_t)op->first_index*size;
    uint64_t remaining=(bytes-advance)/size;
    /* Packet MAX_SIZE is 32 bits; capping it cannot trim a legal uint32 draw. */
    if(remaining>UINT32_MAX)remaining=UINT32_MAX;
    *out=(struct ps5vk_index_fetch){address+advance,(uint32_t)remaining,size};
    return VK_SUCCESS;
}
