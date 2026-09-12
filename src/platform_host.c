#include "vk_descriptor.h"
#include <stdlib.h>
#include <string.h>

static VkResult host_alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx;
    *address = malloc(size);
    *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void host_free_memory(void *ctx, void *backing)
{
    (void)ctx;
    free(backing);
}

static VkResult host_cache_noop(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{
    (void)ctx; (void)backing; (void)offset; (void)size;
    return VK_SUCCESS;
}

static VkResult host_open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){
        NULL, host_alloc_memory, host_free_memory, host_cache_noop, host_cache_noop
    };
    return VK_SUCCESS;
}

static void host_close_backend(struct ps5vk_memory_backend *backend)
{
    (void)backend;
}

__attribute__((weak)) VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    if (!p) return VK_ERROR_INITIALIZATION_FAILED;
    memset(p, 0, sizeof(*p));
    p->open = host_open_backend;
    p->close = host_close_backend;
    p->max_allocation = 64 * 1024 * 1024;
    p->queue_flags = VK_QUEUE_COMPUTE_BIT;
    p->properties.apiVersion = VK_API_VERSION_1_0;
    p->properties.driverVersion = 1;
    p->properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    strcpy(p->properties.deviceName, "ps5vk host platform");
    p->properties.limits.nonCoherentAtomSize = 64;
    p->properties.limits.minStorageBufferOffsetAlignment = 256;
    p->properties.limits.minUniformBufferOffsetAlignment = 256;
    p->properties.limits.maxUniformBufferRange = 64 * 1024;
    p->properties.limits.maxTexelBufferElements = 64 * 1024;
    p->properties.limits.maxBoundDescriptorSets = PS5VK_MAX_SETS;
    p->properties.limits.maxPerStageDescriptorStorageBuffers = PS5VK_MAX_DESCRIPTORS;
    p->properties.limits.maxDescriptorSetStorageBuffers = PS5VK_MAX_DESCRIPTORS;
    p->properties.limits.maxPerStageDescriptorUniformBuffers = PS5VK_MAX_DESCRIPTORS;
    p->properties.limits.maxDescriptorSetUniformBuffers = PS5VK_MAX_DESCRIPTORS;
    p->properties.limits.maxPerStageDescriptorSampledImages = 1;
    p->properties.limits.maxDescriptorSetSampledImages = 1;
    p->properties.limits.maxPerStageResources = PS5VK_MAX_DESCRIPTORS;
    p->properties.limits.maxComputeWorkGroupInvocations = 1024;
    for (int i = 0; i < 3; i++) {
        p->properties.limits.maxComputeWorkGroupCount[i] = 65535;
        p->properties.limits.maxComputeWorkGroupSize[i] = 1024;
    }
    p->memory_properties.memoryTypeCount = 1;
    p->memory_properties.memoryHeapCount = 1;
    p->memory_properties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    p->memory_properties.memoryHeaps[0].size = 64 * 1024 * 1024;
    return VK_SUCCESS;
}
