/* Public-SDK witness for the graphics profile's 1 GiB single allocation.
 *
 * It reports the direct memory the title can see, then three times allocates
 * one 1 GiB VkDeviceMemory, binds a 1 GiB storage buffer, has a compute
 * dispatch write a pattern into the first, middle and last 64 KiB, and checks
 * from the host both the pattern and the guard words around each window
 * before it frees everything. The first iteration also proves the boundary
 * (1 GiB + one granule is refused) and fills the heap headroom in 32 MiB
 * allocations until the budget refuses. Every GPU wait is a bounded fence. */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "dxvk_heap_shader.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end,
                                       size_t alignment, int64_t *block_start,
                                       size_t *block_bytes);

#define GIB (UINT64_C(1) << 30)
enum { ITERATIONS = 3, REGIONS = 3, REGION_BYTES = 65536, GUARD_BYTES = 4096,
       HEADROOM_CHUNKS = 16 };
#define HEADROOM_CHUNK_BYTES (UINT64_C(32) << 20)
#define FENCE_NS UINT64_C(300000000)
static const uint32_t guard_word = UINT32_C(0xdeadbeef);
static const VkDeviceSize region_offsets[REGIONS] = {
    0, GIB / 2, GIB - REGION_BYTES};

static uint32_t seed_for(uint32_t iteration, uint32_t region)
{ return UINT32_C(0x4ea90000) | (iteration << 4) | region; }
static uint32_t expected_value(uint32_t seed, uint32_t n)
{ return (n * UINT32_C(2654435761)) ^ seed; }
static uint32_t digest_word(uint32_t digest, uint32_t word)
{ return (digest ^ word) * UINT32_C(16777619); }

static void report_direct_memory(const char *stage)
{
    const size_t capacity = sceKernelGetDirectMemorySize();
    int64_t start = -1;
    size_t bytes = 0;
    int rc = -1;
    if (capacity && capacity <= INT64_MAX)
        rc = sceKernelAvailableDirectMemorySize(0, (int64_t)capacity, 65536,
                                                &start, &bytes);
    ps5log_printf(PS5LOG_MARK,
        "DXVK_HEAP_WITNESS_DIRECT_MEMORY stage=%s capacity=%llu rc=%d "
        "block_start=%lld block_bytes=%llu", stage,
        (unsigned long long)capacity, rc, (long long)start,
        (unsigned long long)bytes);
}

static VkDeviceSize window_start(VkDeviceSize offset)
{ return offset < GUARD_BYTES ? 0 : offset - GUARD_BYTES; }
static VkDeviceSize window_end(VkDeviceSize offset)
{
    const VkDeviceSize end = offset + REGION_BYTES + GUARD_BYTES;
    return end > GIB ? GIB : end;
}

static int run_witness(void)
{
    VkResult result = VK_SUCCESS;
    const char *failed = NULL;
#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { \
    failed = #call; goto cleanup; } } while (0)
#define REQUIRE(condition, reason) do { if (!(condition)) { \
    result = VK_ERROR_UNKNOWN; failed = reason; goto cleanup; } } while (0)
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[REGIONS] = {VK_NULL_HANDLE};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceMemory headroom[HEADROOM_CHUNKS] = {VK_NULL_HANDLE};
    uint32_t headroom_count = 0;
    VkBool32 pending = VK_FALSE;

    report_direct_memory("boot");
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t physical_count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &physical_count, &physical));
    REQUIRE(physical_count == 1 && physical, "one physical device");
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    ps5log_printf(PS5LOG_MARK,
        "DXVK_HEAP_WITNESS_START heap=%llu heaps=%u types=%u storage_range=%u "
        "allocations=%u ssbo_align=%llu atom=%llu",
        (unsigned long long)memory_properties.memoryHeaps[0].size,
        memory_properties.memoryHeapCount, memory_properties.memoryTypeCount,
        properties.limits.maxStorageBufferRange,
        properties.limits.maxMemoryAllocationCount,
        (unsigned long long)properties.limits.minStorageBufferOffsetAlignment,
        (unsigned long long)properties.limits.nonCoherentAtomSize);
    REQUIRE(memory_properties.memoryHeapCount == 1 &&
            memory_properties.memoryHeaps[0].size > GIB &&
            REGION_BYTES % properties.limits.minStorageBufferOffsetAlignment == 0 &&
            REGION_BYTES % properties.limits.nonCoherentAtomSize == 0 &&
            GUARD_BYTES % properties.limits.nonCoherentAtomSize == 0,
            "heap and alignment preconditions");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    report_direct_memory("device");

    VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(dxvk_heap_spirv), .pCode = dxvk_heap_spirv};
    TRY(vkCreateShaderModule(device, &shader_info, NULL, &module));
    VkDescriptorSetLayoutBinding binding = {.binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    TRY(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, REGIONS};
    VkDescriptorPoolCreateInfo descriptor_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = REGIONS, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    TRY(vkCreateDescriptorPool(device, &descriptor_pool_info, NULL, &descriptor_pool));
    const VkDescriptorSetLayout layouts[REGIONS] = {set_layout, set_layout, set_layout};
    VkDescriptorSetAllocateInfo set_allocation = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = REGIONS,
        .pSetLayouts = layouts};
    TRY(vkAllocateDescriptorSets(device, &set_allocation, sets));
    VkPushConstantRange push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range};
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
        .layout = layout};
    TRY(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    TRY(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    TRY(vkAllocateCommandBuffers(device, &command_info, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));

    for (uint32_t iteration = 0; iteration < ITERATIONS; ++iteration) {
        VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = GIB, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        TRY(vkCreateBuffer(device, &buffer_info, NULL, &buffer));
        VkMemoryRequirements requirements = {0};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        REQUIRE(requirements.size == GIB && (requirements.memoryTypeBits & 1u),
                "1 GiB buffer requirements");
        VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = GIB, .memoryTypeIndex = 0};
        if (iteration == 0) {
            /* Boundary control: one granule above the single-allocation
             * limit is refused before any backing is requested. */
            VkDeviceMemory over = VK_NULL_HANDLE;
            VkMemoryAllocateInfo too_big = allocation;
            too_big.allocationSize = GIB + properties.limits.bufferImageGranularity;
            const VkResult refused = vkAllocateMemory(device, &too_big, NULL, &over);
            if (over) vkFreeMemory(device, over, NULL);
            ps5log_printf(PS5LOG_MARK,
                "DXVK_HEAP_WITNESS_BOUNDARY bytes=%llu result=%d",
                (unsigned long long)too_big.allocationSize, (int)refused);
            REQUIRE(refused == VK_ERROR_OUT_OF_DEVICE_MEMORY && !over,
                    "allocation above 1 GiB refused");
        }
        TRY(vkAllocateMemory(device, &allocation, NULL, &memory));
        TRY(vkBindBufferMemory(device, buffer, memory, 0));

        uint8_t *bytes = NULL;
        TRY(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&bytes));
        VkMappedMemoryRange ranges[REGIONS];
        for (uint32_t r = 0; r < REGIONS; ++r) {
            const VkDeviceSize start = window_start(region_offsets[r]);
            const VkDeviceSize end = window_end(region_offsets[r]);
            uint32_t *words = (uint32_t *)(bytes + start);
            for (VkDeviceSize j = 0; j < (end - start) / sizeof(uint32_t); ++j)
                words[j] = guard_word;
            ranges[r] = (VkMappedMemoryRange){.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = memory, .offset = start, .size = end - start};
        }
        result = vkFlushMappedMemoryRanges(device, REGIONS, ranges);
        if (result != VK_SUCCESS) { vkUnmapMemory(device, memory); TRY(result); }

        for (uint32_t r = 0; r < REGIONS; ++r) {
            VkDescriptorBufferInfo descriptor = {buffer, region_offsets[r], REGION_BYTES};
            VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = sets[r], .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &descriptor};
            vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
        }
        TRY(vkResetCommandPool(device, pool, 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        TRY(vkBeginCommandBuffer(command, &begin));
        VkMemoryBarrier to_shader = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &to_shader, 0, NULL, 0, NULL);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        for (uint32_t r = 0; r < REGIONS; ++r) {
            const uint32_t seed = seed_for(iteration, r);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout,
                                    0, 1, &sets[r], 0, NULL);
            vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(seed), &seed);
            vkCmdDispatch(command, REGION_BYTES / sizeof(uint32_t) / 64, 1, 1);
        }
        VkMemoryBarrier to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0, NULL);
        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS) { vkUnmapMemory(device, memory); TRY(result); }
        TRY(vkResetFences(device, 1, &fence));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command};
        result = vkQueueSubmit(queue, 1, &submit, fence);
        if (result != VK_SUCCESS) { vkUnmapMemory(device, memory); TRY(result); }
        pending = VK_TRUE;
        result = vkWaitForFences(device, 1, &fence, VK_TRUE, FENCE_NS);
        ps5log_printf(PS5LOG_MARK, "DXVK_HEAP_WITNESS_FENCE iteration=%u result=%d",
                      iteration, (int)result);
        if (result != VK_SUCCESS) {
            vkUnmapMemory(device, memory);
            failed = "bounded fence";
            if (result == VK_TIMEOUT) result = VK_ERROR_DEVICE_LOST;
            goto cleanup;
        }
        pending = VK_FALSE;
        result = vkInvalidateMappedMemoryRanges(device, REGIONS, ranges);
        if (result != VK_SUCCESS) { vkUnmapMemory(device, memory); TRY(result); }
        uint32_t total_mismatches = 0;
        for (uint32_t r = 0; r < REGIONS; ++r) {
            const uint32_t seed = seed_for(iteration, r);
            const VkDeviceSize offset = region_offsets[r];
            const VkDeviceSize start = window_start(offset);
            const VkDeviceSize end = window_end(offset);
            const uint32_t *words = (const uint32_t *)(bytes + start);
            uint32_t mismatches = 0, guard_mismatches = 0;
            uint32_t digest = UINT32_C(2166136261);
            for (VkDeviceSize j = 0; j < (end - start) / sizeof(uint32_t); ++j) {
                const VkDeviceSize at = start + j * sizeof(uint32_t);
                const int payload = at >= offset && at < offset + REGION_BYTES;
                const uint32_t expected = payload ?
                    expected_value(seed, (uint32_t)((at - offset) / sizeof(uint32_t))) :
                    guard_word;
                if (payload) digest = digest_word(digest, words[j]);
                if (words[j] != expected) {
                    if (payload) ++mismatches; else ++guard_mismatches;
                }
            }
            total_mismatches += mismatches + guard_mismatches;
            ps5log_printf(PS5LOG_MARK,
                "DXVK_HEAP_WITNESS_RESULT iteration=%u region=%u offset=%llu "
                "mismatches=%u guard_mismatches=%u digest=%08x",
                iteration, r, (unsigned long long)offset, mismatches,
                guard_mismatches, digest);
        }
        vkUnmapMemory(device, memory);
        REQUIRE(!total_mismatches, "1 GiB buffer windows");

        if (iteration == 0) {
            /* Headroom: fill the rest of the budget while 1 GiB is held. */
            report_direct_memory("held");
            VkResult refusal = VK_SUCCESS;
            VkMemoryAllocateInfo chunk = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = HEADROOM_CHUNK_BYTES, .memoryTypeIndex = 0};
            while (headroom_count < HEADROOM_CHUNKS) {
                refusal = vkAllocateMemory(device, &chunk, NULL, &headroom[headroom_count]);
                if (refusal != VK_SUCCESS) { headroom[headroom_count] = VK_NULL_HANDLE; break; }
                ++headroom_count;
            }
            report_direct_memory("full");
            ps5log_printf(PS5LOG_MARK,
                "DXVK_HEAP_WITNESS_HEADROOM chunk=%llu chunks=%u bytes=%llu refusal=%d",
                (unsigned long long)HEADROOM_CHUNK_BYTES, headroom_count,
                (unsigned long long)(headroom_count * HEADROOM_CHUNK_BYTES), (int)refusal);
            for (uint32_t n = 0; n < headroom_count; ++n) {
                vkFreeMemory(device, headroom[n], NULL);
                headroom[n] = VK_NULL_HANDLE;
            }
            REQUIRE(refusal == VK_ERROR_OUT_OF_DEVICE_MEMORY,
                    "headroom ends at the budget");
        }
        vkDestroyBuffer(device, buffer, NULL);
        buffer = VK_NULL_HANDLE;
        vkFreeMemory(device, memory, NULL);
        memory = VK_NULL_HANDLE;
        report_direct_memory("released");
    }

cleanup:
    if (pending) {
        ps5log_printf(PS5LOG_ERR,
            "DXVK_HEAP_WITNESS_FAILURE call=%s result=%d retirement=pending",
            failed ? failed : "fence", (int)result);
        return 1;
    }
    for (uint32_t n = 0; n < HEADROOM_CHUNKS; ++n)
        if (headroom[n]) vkFreeMemory(device, headroom[n], NULL);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (command) vkFreeCommandBuffers(device, pool, 1, &command);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    if (layout) vkDestroyPipelineLayout(device, layout, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    if (module) vkDestroyShaderModule(device, module, NULL);
    if (buffer) vkDestroyBuffer(device, buffer, NULL);
    if (memory) vkFreeMemory(device, memory, NULL);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    report_direct_memory("closed");
    if (result == VK_SUCCESS)
        ps5log_printf(PS5LOG_MARK, "DXVK_HEAP_WITNESS_RETIRED resources=clean");
    else
        ps5log_printf(PS5LOG_ERR,
            "DXVK_HEAP_WITNESS_FAILURE call=%s result=%d retirement=attempted",
            failed ? failed : "unknown", (int)result);
    return result == VK_SUCCESS ? 0 : 1;
#undef TRY
#undef REQUIRE
}

int main(void)
{
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t boot = (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
    ps5log_config config;
    const char *loaded = NULL, *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&config);
    if (ps5log_load_config(paths, 1, &config, &loaded)) _exit(1);
    config.udp = 0;
    if (ps5log_init(&config, "PPSA99994", "ps5vk", boot)) _exit(1);
    int failed = run_witness();
    ps5log_close(failed ? "dxvk-heap-witness-failed" : "dxvk-heap-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}
