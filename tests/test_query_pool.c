/*
 * Host contract tests for the Vulkan 1.0 query-pool object surface and the two
 * sparse image queries.
 *
 * Result retrieval remains absent until device-side query initialization and
 * availability exist. Sparse binding is not advertised, so both sparse queries
 * must report an empty list for valid inputs.
 */
#include "vk_internal.h"
#include "vk_query_pool.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = malloc(size); *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult cache_flush(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, cache_flush, cache_flush};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }

VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
                                 .max_allocation = 65536, .queue_flags = VK_QUEUE_COMPUTE_BIT};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU",
        .vendor_id = 0x1002u,
        .heap_size = 65536,
        .allocation_granularity = 1,
        .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

static VkPhysicalDevice physical_device(void)
{
    VkInstance instance;
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1;
    VkPhysicalDevice physical;
    assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS);
    return physical;
}

static VkDevice make_device(VkPhysicalDevice physical)
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice device;
    assert(vkCreateDevice(physical, &dci, NULL, &device) == VK_SUCCESS);
    return device;
}

static VkResult image_requirements(VkDevice device,
    const VkImageCreateInfo *info, VkMemoryRequirements *out)
{
    (void)device; (void)info;
    *out = (VkMemoryRequirements){4096, 256, 1};
    return VK_SUCCESS;
}

int main(void)
{
    VkPhysicalDevice physical = physical_device();
    VkDevice device = make_device(physical);

    /* --- creation rules --- */
    VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                  .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 8};
    VkQueryPool pool = VK_NULL_HANDLE;
    assert(vkCreateQueryPool(device, &info, NULL, &pool) == VK_SUCCESS);
    assert(pool->query_type == VK_QUERY_TYPE_OCCLUSION && pool->query_count == 8);

    VkQueryPool out = (VkQueryPool)(uintptr_t)0x1;
    VkQueryPoolCreateInfo bad_flags = info;
    bad_flags.flags = 1u;
    assert(vkCreateQueryPool(device, &bad_flags, NULL, &out) != VK_SUCCESS && out == VK_NULL_HANDLE);
    VkQueryPoolCreateInfo zero = info;
    zero.queryCount = 0;
    assert(vkCreateQueryPool(device, &zero, NULL, &out) != VK_SUCCESS);
    VkQueryPoolCreateInfo too_many = info;
    too_many.queryCount = PS5VK_MAX_QUERY_POOL_QUERIES + 1u;
    assert(vkCreateQueryPool(device, &too_many, NULL, &out) == VK_ERROR_OUT_OF_HOST_MEMORY);
    VkQueryPoolCreateInfo stats_without_type = info;
    stats_without_type.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT;
    assert(vkCreateQueryPool(device, &stats_without_type, NULL, &out) != VK_SUCCESS);
    VkQueryPoolCreateInfo timestamp = info;
    timestamp.queryType = VK_QUERY_TYPE_TIMESTAMP;
    assert(vkCreateQueryPool(device, &timestamp, NULL, &out) == VK_ERROR_FEATURE_NOT_PRESENT);
    VkQueryPoolCreateInfo stats = info;
    stats.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    stats.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT;
    assert(vkCreateQueryPool(device, &stats, NULL, &out) == VK_ERROR_FEATURE_NOT_PRESENT);

    /* the device cannot be destroyed while the pool child lives */
    unsigned errors = device->lifetime_errors;
    vkDestroyDevice(device, NULL);
    assert(device->lifetime_errors == errors + 1);

    /* --- sparse queries report an empty list --- */
    device->graphics_enabled = VK_TRUE;
    device->image_requirements = image_requirements;
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {16, 16, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    assert(vkCreateImage(device, &image_info, NULL, &image) == VK_SUCCESS);
    uint32_t sparse_count = 7;
    vkGetImageSparseMemoryRequirements(device, image, &sparse_count, NULL);
    assert(sparse_count == 0);
    uint32_t format_count = 7;
    vkGetPhysicalDeviceSparseImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_IMAGE_TILING_OPTIMAL, &format_count, NULL);
    assert(format_count == 0);

    /* --- teardown --- */
    vkDestroyImage(device, image, NULL);
    vkDestroyQueryPool(device, pool, NULL);
    VkInstance instance = device->physical->instance;
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    puts("query-pool lifetime and empty sparse queries: pass");
    return 0;
}
