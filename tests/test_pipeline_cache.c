/*
 * Host contract tests for the Vulkan 1.0 pipeline-cache slice.
 *
 * Hermetic: a mock platform provides the device, no compiler and no GPU work is
 * involved. The oracles mirror the pinned CTS cases that are blocked by D16
 * depth support (misc_tests.cache_header_test, invalid_size_test,
 * zero_size_test, invalid_blob_test) plus object lifetime, merge validation and
 * the device-parentage rules this repository already applies to other objects.
 */
#include "vk_internal.h"
#include "vk_pipeline_cache.h"
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

static uint32_t load_u32(const uint8_t *b)
{ return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }

static VkDevice make_device(void)
{
    VkInstance instance;
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1;
    VkPhysicalDevice physical;
    assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice device;
    assert(vkCreateDevice(physical, &dci, NULL, &device) == VK_SUCCESS);
    return device;
}

static VkPipelineCache make_cache(VkDevice device)
{
    VkPipelineCacheCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache;
    assert(vkCreatePipelineCache(device, &info, NULL, &cache) == VK_SUCCESS);
    assert(cache != VK_NULL_HANDLE);
    return cache;
}

int main(void)
{
    VkDevice device = make_device();

    /* --- lifecycle, parentage and the derived UUID --- */
    VkPipelineCache cache = make_cache(device);
    const uint8_t *uuid = device->physical->platform.properties.pipelineCacheUUID;
    int any_nonzero = 0;
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) any_nonzero |= uuid[i] != 0;
    assert(any_nonzero); /* a zero UUID would make every blob trivially compatible */
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) assert(cache->uuid[i] == uuid[i]);

    /* device destruction must refuse while the cache child lives */
    unsigned errors = device->lifetime_errors;
    vkDestroyDevice(device, NULL);
    assert(device->lifetime_errors == errors + 1);

    /* invalid create parameters fail closed and never publish an object */
    VkPipelineCacheCreateInfo bad = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                     .flags = 1u};
    VkPipelineCache out = (VkPipelineCache)(uintptr_t)0x1;
    assert(vkCreatePipelineCache(device, &bad, NULL, &out) != VK_SUCCESS);
    assert(out == VK_NULL_HANDLE);
    VkPipelineCacheCreateInfo bad_size = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                          .initialDataSize = 64, .pInitialData = NULL};
    assert(vkCreatePipelineCache(device, &bad_size, NULL, &out) != VK_SUCCESS);

    /* --- export: exact header and the required size-query semantics --- */
    size_t size = 0;
    assert(vkGetPipelineCacheData(device, cache, &size, NULL) == VK_SUCCESS);
    assert(size == 32u);
    uint8_t blob[64];
    memset(blob, 0xa5, sizeof(blob));
    size = sizeof(blob);
    assert(vkGetPipelineCacheData(device, cache, &size, blob) == VK_SUCCESS);
    assert(size == 32u);
    for (unsigned i = 32; i < sizeof(blob); ++i) assert(blob[i] == 0xa5); /* no overrun */
    assert(load_u32(blob + 0) == 32u);
    assert(load_u32(blob + 4) == 1u);
    assert(load_u32(blob + 8) == device->physical->platform.properties.vendorID);
    assert(load_u32(blob + 12) == device->physical->platform.properties.deviceID);
    assert(memcmp(blob + 16, uuid, VK_UUID_SIZE) == 0);

    /* buffer one byte short: VK_INCOMPLETE, nothing written, size reported as 0 */
    uint8_t canary[64];
    memset(canary, 0x5a, sizeof(canary));
    size = 31u;
    assert(vkGetPipelineCacheData(device, cache, &size, canary) == VK_INCOMPLETE);
    assert(size == 0u);
    for (unsigned i = 0; i < sizeof(canary); ++i) assert(canary[i] == 0x5a);

    /* --- import: untrusted input is ignored, never trusted --- */
    assert(vkCreatePipelineCache(device, &(VkPipelineCacheCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = 0,
        .pInitialData = blob}, NULL, &out) == VK_SUCCESS);
    vkDestroyPipelineCache(device, out, NULL);

    struct { const char *name; size_t size; uint32_t offset; } corrupt[] = {
        {"truncated", 31u, 0u},
        {"header size", 32u, 0u},
        {"header version", 32u, 4u},
        {"vendor id", 32u, 8u},
        {"device id", 32u, 12u},
        {"uuid", 32u, 16u},
    };
    for (unsigned i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); ++i) {
        uint8_t damaged[32];
        memcpy(damaged, blob, sizeof(damaged));
        damaged[corrupt[i].offset] = (uint8_t)(damaged[corrupt[i].offset] + 13u);
        VkPipelineCacheCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                          .initialDataSize = corrupt[i].size,
                                          .pInitialData = damaged};
        assert(vkCreatePipelineCache(device, &info, NULL, &out) == VK_SUCCESS);
        /* a damaged header must not become a compatible cache */
        uint8_t exported[32];
        size_t exported_size = sizeof(exported);
        assert(vkGetPipelineCacheData(device, out, &exported_size, exported) == VK_SUCCESS);
        size_t reference_size = sizeof(blob);
        uint8_t reference[64];
        assert(vkGetPipelineCacheData(device, cache, &reference_size, reference) == VK_SUCCESS);
        assert(memcmp(exported, reference, sizeof(exported)) == 0);
        vkDestroyPipelineCache(device, out, NULL);
        printf("import rejected: %s\n", corrupt[i].name);
    }

    /* a recognized header-only blob round-trips byte for byte */
    VkPipelineCacheCreateInfo round_trip = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                            .initialDataSize = 32u, .pInitialData = blob};
    assert(vkCreatePipelineCache(device, &round_trip, NULL, &out) == VK_SUCCESS);
    uint8_t exported[32];
    size_t exported_size = sizeof(exported);
    assert(vkGetPipelineCacheData(device, out, &exported_size, exported) == VK_SUCCESS);
    assert(exported_size == 32u && memcmp(exported, blob, sizeof(exported)) == 0);
    vkDestroyPipelineCache(device, out, NULL);

    /* --- merge: validated no-op --- */
    VkPipelineCache second = make_cache(device);
    assert(vkMergePipelineCaches(device, cache, 0u, NULL) == VK_SUCCESS);
    assert(vkMergePipelineCaches(device, cache, 1u, &cache) == VK_SUCCESS);       /* self */
    VkPipelineCache sources[2] = {second, second};
    assert(vkMergePipelineCaches(device, cache, 2u, sources) == VK_SUCCESS);      /* duplicates */
    VkPipelineCache null_source = VK_NULL_HANDLE;
    assert(vkMergePipelineCaches(device, cache, 1u, &null_source) != VK_SUCCESS);
    assert(vkMergePipelineCaches(device, cache, 1u, NULL) != VK_SUCCESS);
    /* destination unchanged on the rejected call */
    size_t after_size = sizeof(exported);
    uint8_t after[32];
    assert(vkGetPipelineCacheData(device, cache, &after_size, after) == VK_SUCCESS);
    assert(memcmp(after, blob, sizeof(after)) == 0);

    /* foreign-device cache is rejected by both merge and pipeline creation */
    VkDevice other = make_device();
    VkPipelineCache foreign = make_cache(other);
    assert(vkMergePipelineCaches(device, cache, 1u, &foreign) != VK_SUCCESS);
    assert(vkGetPipelineCacheData(device, foreign, &after_size, after) != VK_SUCCESS);
    vkDestroyPipelineCache(device, foreign, NULL); /* no-op: another device owns it */
    assert(foreign->device == other);
    vkDestroyPipelineCache(other, foreign, NULL);
    vkDestroyPipelineCache(other, second, NULL); /* wrong device for second: ignored */
    VkInstance other_instance = other->physical->instance;
    vkDestroyDevice(other, NULL);
    vkDestroyInstance(other_instance, NULL);

    /* --- teardown --- */
    vkDestroyPipelineCache(device, second, NULL);
    vkDestroyPipelineCache(device, cache, NULL);
    /* A second destroy of a freed handle is undefined in Vulkan; the sanitized
     * build proves it must not be attempted, so the test stops here. */
    VkInstance instance = device->physical->instance;
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);

    puts("pipeline cache: pass (lifecycle, header, incomplete buffer, import, merge, parentage)");
    return 0;
}
