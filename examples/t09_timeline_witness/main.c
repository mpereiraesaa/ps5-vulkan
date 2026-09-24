/* Public-SDK witness for VK_KHR_timeline_semaphore on the native queue.
 *
 * A compute dispatch writes a buffer; the submission waits on a timeline
 * value nobody has signalled yet and signals a later value. The witness
 * checks that vkQueueSubmit returns, that neither the counter nor the buffer
 * moves before the host signal, and that after vkWaitSemaphoresKHR on the
 * named value the host reads exactly the GPU-written data. A second phase
 * signals a value above 2^32 from the device to exercise full-width values. */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "t09_timeline_shader.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { VALUES = 64, GUARD_WORDS = 8 };
static const uint32_t guard_word = UINT32_C(0xdeadbeef);
static const uint64_t initial_value = 1000;
static const uint64_t gate_value = 1001;
static const uint64_t phase1_value = 1002;
static const uint64_t phase2_value = UINT64_C(1) << 40;
static const uint32_t seeds[2] = {UINT32_C(0x5eed1001), UINT32_C(0x7a11e002)};

static uint32_t expected_value(uint32_t seed, uint32_t n)
{ return (n * UINT32_C(2654435761)) ^ seed; }
static uint32_t digest_word(uint32_t digest, uint32_t word)
{ return (digest ^ word) * UINT32_C(16777619); }

struct buffer_view {
    VkDevice device;
    VkDeviceMemory memory;
    VkDeviceSize bytes, offset;
};
/* Count mismatches over the whole allocation: the VALUES words at the
 * descriptor offset must hold `seed` data (or the guard when seed is 0),
 * every other word the guard. */
static VkResult check_buffer(const struct buffer_view *b, uint32_t seed,
    uint32_t *mismatches, uint32_t *guard_mismatches, uint32_t *digest)
{
    uint32_t *words = NULL;
    VkResult result = vkMapMemory(b->device, b->memory, 0, VK_WHOLE_SIZE, 0,
                                  (void **)&words);
    if (result != VK_SUCCESS) return result;
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = b->memory, .offset = 0, .size = VK_WHOLE_SIZE};
    result = vkInvalidateMappedMemoryRanges(b->device, 1, &range);
    *mismatches = *guard_mismatches = 0;
    *digest = UINT32_C(2166136261);
    const VkDeviceSize first = b->offset / sizeof(uint32_t);
    for (VkDeviceSize j = 0; result == VK_SUCCESS && j < b->bytes / sizeof(uint32_t); ++j) {
        const int payload = j >= first && j < first + VALUES;
        const uint32_t expected = payload && seed ?
            expected_value(seed, (uint32_t)(j - first)) : guard_word;
        if (payload && seed) *digest = digest_word(*digest, words[j]);
        if (words[j] != expected) {
            ++*mismatches;
            if (!payload) ++*guard_mismatches;
        }
    }
    vkUnmapMemory(b->device, b->memory);
    return result;
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
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSemaphore timeline = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBool32 submitted = VK_FALSE;
    PFN_vkGetSemaphoreCounterValueKHR get_value = NULL;
    PFN_vkWaitSemaphoresKHR wait = NULL;
    PFN_vkSignalSemaphoreKHR signal = NULL;
    struct buffer_view view = {0};

    const char *instance_extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &instance_extension};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t physical_count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &physical_count, &physical));
    REQUIRE(physical_count == 1 && physical, "one physical device");

    uint32_t extension_count = 0;
    TRY(vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, NULL));
    VkExtensionProperties extensions[32];
    REQUIRE(extension_count <= 32, "bounded extension list");
    TRY(vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, extensions));
    uint32_t spec = 0;
    for (uint32_t n = 0; n < extension_count; ++n)
        if (!strcmp(extensions[n].extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))
            spec = extensions[n].specVersion;
    REQUIRE(spec, "timeline extension enumerated");
    VkPhysicalDeviceTimelineSemaphoreFeatures reported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = &reported};
    vkGetPhysicalDeviceFeatures2KHR(physical, &features);
    REQUIRE(reported.timelineSemaphore, "timelineSemaphore reported");
    VkPhysicalDeviceTimelineSemaphoreProperties limits = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &limits};
    vkGetPhysicalDeviceProperties2KHR(physical, &properties);
    REQUIRE(limits.maxTimelineSemaphoreValueDifference >= UINT64_C(2147483647),
            "maxTimelineSemaphoreValueDifference floor");

    const char *device_extension = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceTimelineSemaphoreFeatures requested = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .timelineSemaphore = VK_TRUE};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &requested, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &device_extension};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    get_value = (PFN_vkGetSemaphoreCounterValueKHR)vkGetDeviceProcAddr(
        device, "vkGetSemaphoreCounterValueKHR");
    wait = (PFN_vkWaitSemaphoresKHR)vkGetDeviceProcAddr(device, "vkWaitSemaphoresKHR");
    signal = (PFN_vkSignalSemaphoreKHR)vkGetDeviceProcAddr(device, "vkSignalSemaphoreKHR");
    REQUIRE(get_value && wait && signal, "KHR timeline commands available");
    ps5log_printf(PS5LOG_MARK,
        "T09_TIMELINE_WITNESS_START spec=%u max_difference=%llu initial=%llu values=%u",
        spec, (unsigned long long)limits.maxTimelineSemaphoreValueDifference,
        (unsigned long long)initial_value, VALUES);

    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = initial_value};
    VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                            .pNext = &type};
    TRY(vkCreateSemaphore(device, &semaphore_info, NULL, &timeline));

    /* Storage: the descriptor range sits between leading and trailing guards. */
    VkPhysicalDeviceProperties core;
    vkGetPhysicalDeviceProperties(physical, &core);
    VkDeviceSize offset = core.limits.minStorageBufferOffsetAlignment;
    if (offset < GUARD_WORDS * sizeof(uint32_t)) offset = GUARD_WORDS * sizeof(uint32_t);
    const VkDeviceSize range = VALUES * sizeof(uint32_t);
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = offset + range + GUARD_WORDS * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements = {0};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = 0};
    TRY(vkAllocateMemory(device, &allocation, NULL, &memory));
    TRY(vkBindBufferMemory(device, buffer, memory, 0));
    view = (struct buffer_view){device, memory, requirements.size, offset};
    {
        uint32_t *words = NULL;
        TRY(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&words));
        for (VkDeviceSize j = 0; j < requirements.size / sizeof(uint32_t); ++j)
            words[j] = guard_word;
        VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memory, .offset = 0, .size = VK_WHOLE_SIZE};
        result = vkFlushMappedMemoryRanges(device, 1, &flush);
        vkUnmapMemory(device, memory);
        TRY(result);
    }

    VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(t09_timeline_spirv), .pCode = t09_timeline_spirv};
    TRY(vkCreateShaderModule(device, &shader_info, NULL, &module));
    VkDescriptorSetLayoutBinding binding = {.binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    TRY(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo descriptor_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    TRY(vkCreateDescriptorPool(device, &descriptor_pool_info, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo set_allocation = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    TRY(vkAllocateDescriptorSets(device, &set_allocation, &set));
    VkDescriptorBufferInfo descriptor = {buffer, offset, range};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &descriptor};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
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
        .commandBufferCount = 2};
    TRY(vkAllocateCommandBuffers(device, &command_info, commands));
    for (uint32_t n = 0; n < 2; ++n) {
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        TRY(vkBeginCommandBuffer(commands[n], &begin));
        VkBufferMemoryBarrier to_shader = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_HOST_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer, .offset = offset, .size = range};
        vkCmdPipelineBarrier(commands[n], VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, &to_shader, 0, NULL);
        vkCmdBindPipeline(commands[n], VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(commands[n], VK_PIPELINE_BIND_POINT_COMPUTE, layout,
                                0, 1, &set, 0, NULL);
        vkCmdPushConstants(commands[n], layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(uint32_t), &seeds[n]);
        vkCmdDispatch(commands[n], 1, 1, 1);
        VkBufferMemoryBarrier to_host = to_shader;
        to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commands[n], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &to_host, 0, NULL);
        TRY(vkEndCommandBuffer(commands[n]));
    }

    /* Phase 1: wait on a value nobody has signalled yet. */
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkTimelineSemaphoreSubmitInfo values = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &gate_value,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &phase1_value};
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &values,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &timeline, .pWaitDstStageMask = &stage,
        .commandBufferCount = 1, .pCommandBuffers = &commands[0],
        .signalSemaphoreCount = 1, .pSignalSemaphores = &timeline};
    TRY(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    submitted = VK_TRUE;
    uint64_t counter = 0;
    TRY(get_value(device, timeline, &counter));
    VkSemaphoreWaitInfo wait_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1, .pSemaphores = &timeline, .pValues = &phase1_value};
    const VkResult early = wait(device, &wait_info, 0);
    const VkResult early_timed = wait(device, &wait_info, UINT64_C(20000000));
    uint32_t mismatches = 0, guard_mismatches = 0, digest = 0;
    TRY(check_buffer(&view, 0, &mismatches, &guard_mismatches, &digest));
    ps5log_printf(PS5LOG_MARK,
        "T09_TIMELINE_WITNESS_DEFERRED submit=returned counter=%llu wait_zero=%s wait_20ms=%s untouched=%s",
        (unsigned long long)counter, early == VK_TIMEOUT ? "timeout" : "other",
        early_timed == VK_TIMEOUT ? "timeout" : "other",
        mismatches ? "no" : "yes");
    REQUIRE(counter == initial_value && early == VK_TIMEOUT && early_timed == VK_TIMEOUT &&
            !mismatches, "nothing visible before the host signal");

    VkSemaphoreSignalInfo host = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = timeline, .value = gate_value};
    TRY(signal(device, &host));
    TRY(wait(device, &wait_info, UINT64_C(2000000000)));
    TRY(get_value(device, timeline, &counter));
    TRY(check_buffer(&view, seeds[0], &mismatches, &guard_mismatches, &digest));
    ps5log_printf(PS5LOG_MARK,
        "T09_TIMELINE_WITNESS_RESULT phase=1 wait=success counter=%llu values=%u mismatches=%u guard_mismatches=%u digest=%08x",
        (unsigned long long)counter, VALUES, mismatches, guard_mismatches, digest);
    REQUIRE(counter == phase1_value && !mismatches && !guard_mismatches,
            "phase 1 GPU data after the named value");

    /* Phase 2: a device signal above 2^32, waited with a finite timeout. */
    values = (VkTimelineSemaphoreSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &phase2_value};
    submit = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &values,
        .commandBufferCount = 1, .pCommandBuffers = &commands[1],
        .signalSemaphoreCount = 1, .pSignalSemaphores = &timeline};
    TRY(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    wait_info.pValues = &phase2_value;
    TRY(wait(device, &wait_info, UINT64_C(2000000000)));
    TRY(get_value(device, timeline, &counter));
    TRY(check_buffer(&view, seeds[1], &mismatches, &guard_mismatches, &digest));
    ps5log_printf(PS5LOG_MARK,
        "T09_TIMELINE_WITNESS_RESULT phase=2 wait=success counter=%llu values=%u mismatches=%u guard_mismatches=%u digest=%08x",
        (unsigned long long)counter, VALUES, mismatches, guard_mismatches, digest);
    REQUIRE(counter == phase2_value && !mismatches && !guard_mismatches,
            "phase 2 GPU data after the named value");
    TRY(vkDeviceWaitIdle(device));
    submitted = VK_FALSE;

cleanup:
    if (submitted && device && vkDeviceWaitIdle(device) != VK_SUCCESS) {
        ps5log_printf(PS5LOG_ERR,
            "T09_TIMELINE_WITNESS_FAILURE call=%s result=%d retirement=pending",
            failed ? failed : "idle", (int)result);
        return 1;
    }
    if (commands[0] || commands[1]) vkFreeCommandBuffers(device, pool, 2, commands);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    if (layout) vkDestroyPipelineLayout(device, layout, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    if (module) vkDestroyShaderModule(device, module, NULL);
    if (buffer) vkDestroyBuffer(device, buffer, NULL);
    if (memory) vkFreeMemory(device, memory, NULL);
    if (timeline) vkDestroySemaphore(device, timeline, NULL);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS)
        ps5log_printf(PS5LOG_MARK, "T09_TIMELINE_WITNESS_RETIRED resources=clean");
    else
        ps5log_printf(PS5LOG_ERR,
            "T09_TIMELINE_WITNESS_FAILURE call=%s result=%d retirement=attempted",
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
    ps5log_close(failed ? "t09-timeline-witness-failed" : "t09-timeline-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}
