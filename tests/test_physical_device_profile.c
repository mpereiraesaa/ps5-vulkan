#include "physical_device_profile.h"
#include "graphics_limits.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    const struct ps5vk_physical_profile_info compute = {
        .name = "ps5vk gfx1013 test profile",
        .vendor_id = 0x1002,
        .heap_size = 64u * 1024u * 1024u,
        .allocation_granularity = 65536,
        .buffer_image_granularity = 65536,
    };
    ps5vk_physical_profile_init(&properties, &memory, &compute);

    assert(properties.apiVersion == VK_API_VERSION_1_0);
    assert(properties.vendorID == 0x1002 && properties.deviceID == 0);
    assert(properties.limits.maxStorageBufferRange == compute.heap_size);
    assert(properties.limits.maxMemoryAllocationCount == 1024);
    assert(properties.limits.bufferImageGranularity == 65536);
    assert(properties.limits.minTexelBufferOffsetAlignment == 4);
    assert(properties.limits.maxComputeSharedMemorySize == 65536);
    assert(properties.limits.maxComputeWorkGroupInvocations == 1024);
    assert(properties.limits.discreteQueuePriorities ==
           PS5VK_REQUIRED_QUEUE_PRIORITIES);
    assert(memory.memoryHeapCount == 1 && memory.memoryTypeCount == 1);
    assert(memory.memoryHeaps[0].flags == VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
    assert(memory.memoryTypes[0].propertyFlags ==
           (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));
    assert(ps5vk_physical_profile_valid(&properties, &memory, compute.heap_size,
        VK_QUEUE_COMPUTE_BIT, 0, 0));

    VkPhysicalDeviceProperties graphics_properties;
    VkPhysicalDeviceMemoryProperties graphics_memory;
    const struct ps5vk_physical_profile_info graphics = {
        .name = "ps5vk gfx1013 graphics test profile",
        .vendor_id = 0x1002,
        .heap_size = 256u * 1024u * 1024u,
        .allocation_granularity = 131072,
        .buffer_image_granularity = 131072,
    };
    ps5vk_physical_profile_init(&graphics_properties, &graphics_memory, &graphics);
    ps5vk_graphics_limits(&graphics_properties.limits);
    assert(graphics_properties.limits.maxMemoryAllocationCount == 2048);
    assert(graphics_properties.limits.maxImageDimension2D == 16383);
    assert(graphics_properties.limits.storageImageSampleCounts ==
           VK_SAMPLE_COUNT_1_BIT);
    assert(properties.limits.storageImageSampleCounts == 0);
    assert(ps5vk_physical_profile_valid(&graphics_properties, &graphics_memory,
        graphics.heap_size, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 1, 1));

    VkPhysicalDeviceProperties bad = properties;
    bad.limits.minTexelBufferOffsetAlignment = 3;
    assert(!ps5vk_physical_profile_valid(&bad, &memory, compute.heap_size,
        VK_QUEUE_COMPUTE_BIT, 0, 0));
    bad = properties;
    bad.limits.maxStorageBufferRange = (uint32_t)compute.heap_size + 1;
    assert(!ps5vk_physical_profile_valid(&bad, &memory, compute.heap_size,
        VK_QUEUE_COMPUTE_BIT, 0, 0));
    VkPhysicalDeviceMemoryProperties bad_memory = memory;
    bad_memory.memoryHeaps[0].flags = 0;
    assert(!ps5vk_physical_profile_valid(&properties, &bad_memory,
        compute.heap_size, VK_QUEUE_COMPUTE_BIT, 0, 0));
    bad_memory = memory;
    bad_memory.memoryHeaps[0].flags |= VK_MEMORY_HEAP_MULTI_INSTANCE_BIT;
    assert(!ps5vk_physical_profile_valid(&properties, &bad_memory,
        compute.heap_size, VK_QUEUE_COMPUTE_BIT, 0, 0));
    bad_memory = memory;
    bad_memory.memoryTypes[0].propertyFlags |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    assert(!ps5vk_physical_profile_valid(&properties, &bad_memory,
        compute.heap_size, VK_QUEUE_COMPUTE_BIT, 0, 0));
    bad = properties;
    bad.limits.bufferImageGranularity = 3;
    assert(!ps5vk_physical_profile_valid(&bad, &memory,
        compute.heap_size, VK_QUEUE_COMPUTE_BIT, 0, 0));
    bad = properties;
    bad.limits.maxComputeWorkGroupSize[2] = 0;
    assert(!ps5vk_physical_profile_valid(&bad, &memory,
        compute.heap_size, VK_QUEUE_COMPUTE_BIT, 0, 0));
    bad = properties;
    bad.limits.discreteQueuePriorities = PS5VK_REQUIRED_QUEUE_PRIORITIES - 1;
    assert(!ps5vk_physical_profile_valid(&bad, &memory,
        compute.heap_size, VK_QUEUE_COMPUTE_BIT, 0, 0));
    assert(!ps5vk_physical_profile_valid(&properties, &memory,
        compute.heap_size, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0, 0));
    assert(!ps5vk_physical_profile_valid(&properties, &memory,
        compute.heap_size, VK_QUEUE_COMPUTE_BIT, 1, 0));

    puts("Physical-device profile: pass (derived bounds and fail-closed coherence)");
    return 0;
}
