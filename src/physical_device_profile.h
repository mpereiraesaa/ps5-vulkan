#ifndef PS5VK_PHYSICAL_DEVICE_PROFILE_H
#define PS5VK_PHYSICAL_DEVICE_PROFILE_H

#include "vk_descriptor.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

/* Values in this profile describe ps5vk's executable frontend contract.  They
 * are intentionally derived from allocator, descriptor and command-encoder
 * bounds; they are not estimates of the PS5's total hardware capability. */
struct ps5vk_physical_profile_info {
    const char *name;
    uint32_t vendor_id;
    uint32_t device_id;
    VkDeviceSize heap_size;
    VkDeviceSize allocation_granularity;
    VkDeviceSize buffer_image_granularity;
    VkBool32 host_coherent;
};

static inline int ps5vk_profile_power_of_two(VkDeviceSize value)
{ return value && !(value & (value - 1)); }

/* Deterministic, public, non-secret compatibility identity for
 * VkPhysicalDeviceProperties::pipelineCacheUUID. Any change to an input below
 * must invalidate previously exported cache data:
 *   - vendor/device identity of this frontend
 *   - GFX1013 target
 *   - driver version reported in the same properties block
 *   - pinned compiler identity and version (PSBC/ACO, see compilation_cache.h)
 *   - cache ABI revision and this UUID format counter
 * The mixing is FNV-1a over two streams; it is an identity, not a security
 * boundary, and it never depends on console state or private material. */
#define PS5VK_PIPELINE_CACHE_UUID_FORMAT 1u
#define PS5VK_GFX_TARGET 1013u
#define PS5VK_DRIVER_VERSION 1u
#define PS5VK_COMPILER_IDENTITY 0x50534243u /* "PSBC" */
#define PS5VK_COMPILER_IDENTITY_VERSION 1u
#define PS5VK_CACHE_ABI_IDENTITY 1u

static inline void ps5vk_profile_mix(uint64_t *h, uint32_t value)
{
    for (unsigned byte = 0; byte < 4; ++byte) {
        *h ^= (uint64_t)((value >> (8 * byte)) & 0xffu);
        *h *= UINT64_C(0x100000001b3);
    }
}

static inline void ps5vk_pipeline_cache_uuid(uint8_t out[VK_UUID_SIZE],
                                             uint32_t vendor_id, uint32_t device_id)
{
    const char tag[] = "ps5vk-gfx1013-pipeline-cache";
    uint64_t a = UINT64_C(0xcbf29ce484222325);
    uint64_t b = UINT64_C(0x9e3779b97f4a7c15);
    for (size_t i = 0; i < sizeof(tag) - 1; ++i) ps5vk_profile_mix(&a, (uint8_t)tag[i]);
    for (size_t i = 0; i < sizeof(tag) - 1; ++i) ps5vk_profile_mix(&b, (uint8_t)tag[sizeof(tag) - 2 - i]);
    ps5vk_profile_mix(&a, vendor_id);   ps5vk_profile_mix(&b, device_id);
    ps5vk_profile_mix(&a, device_id);   ps5vk_profile_mix(&b, vendor_id);
    ps5vk_profile_mix(&a, PS5VK_GFX_TARGET);             ps5vk_profile_mix(&b, PS5VK_GFX_TARGET ^ 0x5a5a5a5au);
    ps5vk_profile_mix(&a, PS5VK_DRIVER_VERSION);         ps5vk_profile_mix(&b, PS5VK_DRIVER_VERSION);
    ps5vk_profile_mix(&a, PS5VK_COMPILER_IDENTITY);      ps5vk_profile_mix(&b, PS5VK_COMPILER_IDENTITY);
    ps5vk_profile_mix(&a, PS5VK_COMPILER_IDENTITY_VERSION); ps5vk_profile_mix(&b, PS5VK_COMPILER_IDENTITY_VERSION);
    ps5vk_profile_mix(&a, PS5VK_CACHE_ABI_IDENTITY);     ps5vk_profile_mix(&b, PS5VK_CACHE_ABI_IDENTITY);
    ps5vk_profile_mix(&a, PS5VK_PIPELINE_CACHE_UUID_FORMAT); ps5vk_profile_mix(&b, PS5VK_PIPELINE_CACHE_UUID_FORMAT);
    for (unsigned i = 0; i < 8; ++i) {
        out[i] = (uint8_t)((a >> (8 * i)) & 0xffu);
        out[8 + i] = (uint8_t)((b >> (8 * i)) & 0xffu);
    }
}

static inline uint32_t ps5vk_profile_u32(VkDeviceSize value)
{ return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value; }

static inline void ps5vk_physical_profile_init(
    VkPhysicalDeviceProperties *properties,
    VkPhysicalDeviceMemoryProperties *memory,
    const struct ps5vk_physical_profile_info *info)
{
    memset(properties, 0, sizeof(*properties));
    memset(memory, 0, sizeof(*memory));

    properties->apiVersion = VK_API_VERSION_1_0;
    properties->driverVersion = 1;
    properties->vendorID = info->vendor_id;
    properties->deviceID = info->device_id;
    ps5vk_pipeline_cache_uuid(properties->pipelineCacheUUID, info->vendor_id, info->device_id);
    properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    if (info->name) {
        strncpy(properties->deviceName, info->name,
                VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
        properties->deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1] = '\0';
    }

    VkPhysicalDeviceLimits *limits = &properties->limits;
    limits->maxStorageBufferRange = ps5vk_profile_u32(info->heap_size);
    limits->maxUniformBufferRange = 64u * 1024u;
    limits->maxTexelBufferElements = 64u * 1024u;
    limits->maxPushConstantsSize = PS5VK_MAX_PUSH_CONSTANT_BYTES;
    limits->maxMemoryAllocationCount = info->allocation_granularity
        ? ps5vk_profile_u32(info->heap_size / info->allocation_granularity) : 0;
    limits->bufferImageGranularity = info->buffer_image_granularity;
    limits->minMemoryMapAlignment = 64;
    limits->minTexelBufferOffsetAlignment = 4;
    limits->minStorageBufferOffsetAlignment = 256;
    limits->minUniformBufferOffsetAlignment = 256;
    limits->nonCoherentAtomSize = 64;

    limits->maxBoundDescriptorSets = PS5VK_MAX_SETS;
    limits->maxPerStageDescriptorStorageBuffers = PS5VK_MAX_DESCRIPTORS;
    limits->maxDescriptorSetStorageBuffers = PS5VK_MAX_DESCRIPTORS;
    limits->maxPerStageDescriptorUniformBuffers = PS5VK_MAX_DESCRIPTORS;
    limits->maxDescriptorSetUniformBuffers = PS5VK_MAX_DESCRIPTORS;
    /* Uniform texel buffers consume the sampled-image accounting class. */
    limits->maxPerStageDescriptorSampledImages = 1;
    limits->maxDescriptorSetSampledImages = 1;
    limits->maxPerStageResources = PS5VK_MAX_DESCRIPTORS;

    /* dispatch_encode.c enforces each dimension, the product and group count.
     * PSBC configures 64 KiB LDS per GFX1013 workgroup. */
    limits->maxComputeSharedMemorySize = 64u * 1024u;
    limits->maxComputeWorkGroupInvocations = 1024;
    for (unsigned i = 0; i < 3; ++i) {
        limits->maxComputeWorkGroupCount[i] = 65535;
        limits->maxComputeWorkGroupSize[i] = 1024;
    }

    memory->memoryHeapCount = 1;
    memory->memoryTypeCount = 1;
    memory->memoryHeaps[0].size = info->heap_size;
    memory->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    memory->memoryTypes[0].heapIndex = 0;
    memory->memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        (info->host_coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0);
}

static inline int ps5vk_physical_profile_valid(
    const VkPhysicalDeviceProperties *properties,
    const VkPhysicalDeviceMemoryProperties *memory,
    VkDeviceSize max_allocation,
    VkQueueFlags queue_flags,
    int has_format_properties,
    int has_image_properties)
{
    const VkPhysicalDeviceLimits *limits = &properties->limits;
    if (properties->apiVersion != VK_API_VERSION_1_0 ||
        !properties->deviceName[0] ||
        properties->deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1] != '\0' ||
        !max_allocation || !queue_flags ||
        (queue_flags & ~(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                         VK_QUEUE_TRANSFER_BIT)) ||
        !limits->maxStorageBufferRange ||
        limits->maxStorageBufferRange > max_allocation ||
        !limits->maxUniformBufferRange || !limits->maxTexelBufferElements ||
        !limits->maxPushConstantsSize || !limits->maxMemoryAllocationCount ||
        !ps5vk_profile_power_of_two(limits->bufferImageGranularity) ||
        !ps5vk_profile_power_of_two(limits->minMemoryMapAlignment) ||
        !ps5vk_profile_power_of_two(limits->minTexelBufferOffsetAlignment) ||
        !ps5vk_profile_power_of_two(limits->minUniformBufferOffsetAlignment) ||
        !ps5vk_profile_power_of_two(limits->minStorageBufferOffsetAlignment) ||
        !ps5vk_profile_power_of_two(limits->nonCoherentAtomSize) ||
        limits->maxBoundDescriptorSets != PS5VK_MAX_SETS ||
        limits->maxPerStageResources != PS5VK_MAX_DESCRIPTORS ||
        !limits->maxComputeSharedMemorySize ||
        !limits->maxComputeWorkGroupInvocations)
        return 0;
    for (unsigned i = 0; i < 3; ++i)
        if (!limits->maxComputeWorkGroupCount[i] ||
            !limits->maxComputeWorkGroupSize[i]) return 0;

    if (memory->memoryTypeCount != 1 || memory->memoryHeapCount != 1 ||
        memory->memoryTypes[0].heapIndex != 0 ||
        !(memory->memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
        !(memory->memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        (memory->memoryTypes[0].propertyFlags &
            ~(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ||
        !(memory->memoryHeaps[0].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ||
        (memory->memoryHeaps[0].flags & ~VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ||
        memory->memoryHeaps[0].size < max_allocation)
        return 0;

    if (!!has_format_properties != !!has_image_properties) return 0;
    if (queue_flags & VK_QUEUE_GRAPHICS_BIT) {
        if (!has_format_properties || !limits->maxImageDimension2D ||
            !limits->maxFramebufferWidth || !limits->maxFramebufferHeight)
            return 0;
    } else if (has_format_properties || has_image_properties) {
        return 0;
    }
    return 1;
}

#endif
