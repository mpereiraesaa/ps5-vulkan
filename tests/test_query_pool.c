/*
 * Host contract tests for the bounded Vulkan 1.0 query-pool surface. This
 * proves ordered reset, precise-control feature gating, result width and
 * availability publication; it does not emulate occlusion counters.
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
                                 .max_allocation = 65536, .queue_flags = VK_QUEUE_COMPUTE_BIT,
                                 .supported_features = PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE};
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
    VkPhysicalDeviceFeatures features = {.occlusionQueryPrecise = VK_TRUE};
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                              .pEnabledFeatures = &features};
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
    for (uint32_t j = 0; j < pool->query_count; ++j)
        assert(pool->states[j] == PS5VK_QUERY_UNINITIALIZED);

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

    /* Reset is an ordered command: recording cannot change host-visible query
     * state before the frontend operation executes. */
    uint64_t results[4] = {0xaaaaaaaaaaaaaaaaull, 0xbbbbbbbbbbbbbbbbull,
                           0xccccccccccccccccull, 0xddddddddddddddddull};
    assert(vkGetQueryPoolResults(device, pool, 2, 2, sizeof(results), results,
        16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
        == VK_ERROR_UNKNOWN);
    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    VkCommandPool command_pool;
    assert(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool)
        == VK_SUCCESS);
    VkCommandBufferAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command;
    assert(vkAllocateCommandBuffers(device, &allocate_info, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    vkCmdResetQueryPool(command, pool, 2, 2);
    assert(command->state == PS5VK_RECORDING && command->operation_count == 1);
    assert(pool->states[2] == PS5VK_QUERY_UNINITIALIZED);
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    assert(vkQueueSubmit(&device->queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(!device->submission && command->state == PS5VK_EXECUTABLE);
    assert(pool->states[1] == PS5VK_QUERY_UNINITIALIZED &&
        pool->states[2] == PS5VK_QUERY_UNAVAILABLE &&
        pool->states[3] == PS5VK_QUERY_UNAVAILABLE &&
        pool->states[4] == PS5VK_QUERY_UNINITIALIZED);
    assert(vkGetQueryPoolResults(device, pool, 2, 2, sizeof(results), results,
        16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
        == VK_NOT_READY);
    assert(results[0] == 0xaaaaaaaaaaaaaaaaull && results[1] == 0 &&
        results[2] == 0xccccccccccccccccull && results[3] == 0);
    uint32_t one[2] = {0x12345678u, 0x87654321u};
    assert(vkGetQueryPoolResults(device, pool, 2, 1, sizeof(one), one, 0,
        VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_NOT_READY);
    assert(one[0] == 0x12345678u && one[1] == 0);
    assert(vkGetQueryPoolResults(device, pool, 2, 1, sizeof(one), one, 0,
        VK_QUERY_RESULT_WAIT_BIT) == VK_NOT_READY);
    assert(vkGetQueryPoolResults(device, pool, 2, 1, sizeof(one), one, 0,
        VK_QUERY_RESULT_PARTIAL_BIT) == VK_NOT_READY);
    assert(one[0] == 0 && one[1] == 0);

    /* A completed backend may publish only a real measured result. The public
     * result path then writes the low 32 bits or the full 64 bits and sets the
     * matching availability word. */
    assert(ps5vk_query_publish(device, pool, 2, UINT64_C(0x12345678abcdef01)) == VK_SUCCESS);
    uint64_t published64[2] = {0, 0};
    assert(vkGetQueryPoolResults(device, pool, 2, 1, sizeof(published64),
        published64, 0, VK_QUERY_RESULT_64_BIT |
        VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS);
    assert(published64[0] == UINT64_C(0x12345678abcdef01) && published64[1] == 1);
    uint32_t published32[2] = {0, 0};
    assert(vkGetQueryPoolResults(device, pool, 2, 1, sizeof(published32),
        published32, 0, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS);
    assert(published32[0] == UINT32_C(0xabcdef01) && published32[1] == 1);
    assert(ps5vk_query_publish(device, pool, 0, 1) != VK_SUCCESS);

    VkBufferCreateInfo copy_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 64,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer query_copy_buffer;
    assert(vkCreateBuffer(device, &copy_buffer_info, NULL, &query_copy_buffer) == VK_SUCCESS);
    VkMemoryRequirements copy_requirements;
    vkGetBufferMemoryRequirements(device, query_copy_buffer, &copy_requirements);
    VkMemoryAllocateInfo copy_memory_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = copy_requirements.size,
        .memoryTypeIndex = 0,
    };
    VkDeviceMemory query_copy_memory;
    assert(vkAllocateMemory(device, &copy_memory_info, NULL, &query_copy_memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, query_copy_buffer, query_copy_memory, 0) == VK_SUCCESS);
    void *query_copy_map;
    VkDeviceSize query_copy_bytes;
    assert(ps5vk_buffer_span(device, query_copy_buffer, 0, VK_WHOLE_SIZE,
        &query_copy_map, &query_copy_bytes) == VK_SUCCESS && query_copy_bytes >= 64);
    memset(query_copy_map, 0x5a, 64);
    assert(vkResetCommandBuffer(command, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    vkCmdCopyQueryPoolResults(command, pool, 2, 1, query_copy_buffer, 0, 16,
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
        VK_QUERY_RESULT_WAIT_BIT);
    vkCmdCopyQueryPoolResults(command, pool, 3, 1, query_copy_buffer, 32, 0,
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
        VK_QUERY_RESULT_PARTIAL_BIT);
    assert(command->state == PS5VK_RECORDING && command->operation_count == 2 &&
        command->operations[0].type == PS5VK_QUERY_COPY);
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    submit = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    assert(vkQueueSubmit(&device->queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(((uint64_t *)query_copy_map)[0] == UINT64_C(0x12345678abcdef01) &&
        ((uint64_t *)query_copy_map)[1] == 1);
    assert(((uint64_t *)query_copy_map)[4] == 0 &&
        ((uint64_t *)query_copy_map)[5] == 0);
    assert(vkResetCommandBuffer(command, 0) == VK_SUCCESS);

    /* Pinned CTS copy-reset stride coverage uses zero stride only with one
     * query per copy, advancing dstOffset by the packed result record. */
    assert(ps5vk_query_publish(device, pool, 3, UINT64_C(0x0123456789abcdef))
        == VK_SUCCESS);
    memset(query_copy_map, 0x5a, 32);
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    vkCmdCopyQueryPoolResults(command, pool, 2, 1, query_copy_buffer, 0, 0,
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
        VK_QUERY_RESULT_WAIT_BIT);
    vkCmdCopyQueryPoolResults(command, pool, 3, 1, query_copy_buffer, 16, 0,
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
        VK_QUERY_RESULT_WAIT_BIT);
    assert(command->state == PS5VK_RECORDING && command->operation_count == 2 &&
        command->operations[0].query_stride == 0 &&
        command->operations[1].query_stride == 0);
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    submit = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    assert(vkQueueSubmit(&device->queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    uint64_t *packed_copy = (uint64_t *)query_copy_map;
    assert(packed_copy[0] == UINT64_C(0x12345678abcdef01) && packed_copy[1] == 1 &&
        packed_copy[2] == UINT64_C(0x0123456789abcdef) && packed_copy[3] == 1);
    assert(vkResetCommandBuffer(command, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    vkCmdCopyQueryPoolResults(command, pool, 2, 2, query_copy_buffer, 0, 0,
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    assert(command->state == PS5VK_INVALID && !command->operation_count);
    assert(vkResetCommandBuffer(command, 0) == VK_SUCCESS);

    /* No fake results: operations requiring a real ZPASS counter or supported
     * timestamp domain invalidate recording without consuming an op slot. */
#define CHECK_QUERY_FAIL_CLOSED(statement) do { \
    assert(vkResetCommandBuffer(command, 0) == VK_SUCCESS); \
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS); \
    statement; \
    assert(command->state == PS5VK_INVALID && !command->operation_count); \
} while (0)
    CHECK_QUERY_FAIL_CLOSED(vkCmdBeginQuery(command, pool, 2, 0));
    CHECK_QUERY_FAIL_CLOSED(vkCmdEndQuery(command, pool, 2));
    CHECK_QUERY_FAIL_CLOSED(vkCmdWriteTimestamp(command,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 2));
    CHECK_QUERY_FAIL_CLOSED(vkCmdCopyQueryPoolResults(command, pool, 2, 1,
        VK_NULL_HANDLE, 0, 0, 0));
    CHECK_QUERY_FAIL_CLOSED(vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE));
    CHECK_QUERY_FAIL_CLOSED(vkCmdNextSubpass(command,
        VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS));
    CHECK_QUERY_FAIL_CLOSED(vkCmdExecuteCommands(command, 0, NULL));
    CHECK_QUERY_FAIL_CLOSED(vkCmdExecuteCommands(command, 1, &command));
#undef CHECK_QUERY_FAIL_CLOSED

    /* The recorder accepts a structurally paired, non-precise occlusion scope
     * inside an inline pass. Submission still requires native counter support. */
    assert(vkResetCommandBuffer(command, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    vkCmdResetQueryPool(command, pool, 2, 1);
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    submit = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    assert(vkQueueSubmit(&device->queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(pool->states[2] == PS5VK_QUERY_UNAVAILABLE);
    assert(vkResetCommandBuffer(command, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    command->render_pass = (VkRenderPass)(uintptr_t)1;
    command->render_pass_contents = VK_SUBPASS_CONTENTS_INLINE;
    vkCmdBeginQuery(command, pool, 2, 0);
    assert(command->state == PS5VK_RECORDING && command->operation_count == 1 &&
        command->active_occlusion_query_pool == pool &&
        command->operations[0].type == PS5VK_QUERY_BEGIN);
    vkCmdEndQuery(command, pool, 2);
    assert(command->state == PS5VK_RECORDING && command->operation_count == 2 &&
        !command->active_occlusion_query_pool &&
        command->operations[1].type == PS5VK_QUERY_END);
    command->render_pass = VK_NULL_HANDLE;
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    assert(vkResetCommandBuffer(command, 0) == VK_SUCCESS);

    /* The precise control bit is accepted only when the logical device enabled
     * occlusionQueryPrecise. It remains an ordered begin/end pair, and the
     * operation validator enforces the same feature contract at submit time. */
    VkPhysicalDeviceFeatures reported_features;
    vkGetPhysicalDeviceFeatures(physical, &reported_features);
    assert(reported_features.occlusionQueryPrecise == VK_TRUE);
    assert(device->enabled_features & PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE);
    device->enabled_features &= ~PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE;
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    vkCmdResetQueryPool(command, pool, 2, 1);
    command->render_pass = (VkRenderPass)(uintptr_t)1;
    command->render_pass_contents = VK_SUBPASS_CONTENTS_INLINE;
    vkCmdBeginQuery(command, pool, 2, VK_QUERY_CONTROL_PRECISE_BIT);
    assert(command->state == PS5VK_INVALID && command->operation_count == 1 &&
        command->operations[0].type == PS5VK_QUERY_RESET &&
        !command->active_occlusion_query_pool);
    assert(vkResetCommandBuffer(command, 0) == VK_SUCCESS);
    device->enabled_features |= PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE;
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    vkCmdResetQueryPool(command, pool, 2, 1);
    command->render_pass = (VkRenderPass)(uintptr_t)1;
    command->render_pass_contents = VK_SUBPASS_CONTENTS_INLINE;
    vkCmdBeginQuery(command, pool, 2, VK_QUERY_CONTROL_PRECISE_BIT);
    assert(command->state == PS5VK_RECORDING && command->operation_count == 2 &&
        command->operations[1].type == PS5VK_QUERY_BEGIN &&
        command->operations[1].query_flags == VK_QUERY_CONTROL_PRECISE_BIT);
    assert(ps5vk_query_operation_validate(device, &command->operations[1]) == VK_SUCCESS);
    device->enabled_features &= ~PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE;
    assert(ps5vk_query_operation_validate(device, &command->operations[1]) ==
        VK_ERROR_FEATURE_NOT_PRESENT);
    device->enabled_features |= PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE;
    vkCmdEndQuery(command, pool, 2);
    assert(command->state == PS5VK_RECORDING && command->operation_count == 3 &&
        command->operations[2].type == PS5VK_QUERY_END);
    command->render_pass = VK_NULL_HANDLE;
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    assert(vkResetCommandBuffer(command, 0) == VK_SUCCESS);

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
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyBuffer(device, query_copy_buffer, NULL);
    vkFreeMemory(device, query_copy_memory, NULL);
    vkDestroyQueryPool(device, pool, NULL);
    VkInstance instance = device->physical->instance;
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    puts("query-pool lifetime and empty sparse queries: pass");
    return 0;
}
