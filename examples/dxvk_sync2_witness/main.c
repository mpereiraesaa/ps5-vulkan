/* Public-SDK witness for VK_KHR_synchronization2 on the Vulkan 1.0 profile.
 *
 * It negotiates the extension and its feature, resolves the six KHR
 * commands, and runs compute work ordered only through them:
 *   phase 1  DXVK's submission shape: one vkQueueSubmit2KHR of three command
 *            buffers (a host->compute buffer barrier2, the dispatch, a global
 *            compute->host memory barrier2) signalling a timeline value at
 *            BOTTOM_OF_PIPE, then vkWaitSemaphoresKHR on it;
 *   phase 2  a submit2 that waits on a timeline value nobody has signalled
 *            yet: nothing may become visible before the host signal;
 *   phase 3  the empty submit2 DXVK uses to advance its timeline.
 * Each phase checks the whole buffer word for word (payload and guards). The
 * queue family reports timestampValidBits = 0, so vkCmdWriteTimestamp2KHR is
 * resolved but never recorded. */
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
static const uint64_t phase1_value = 1001;
static const uint64_t gate_value = 1002;
static const uint64_t phase2_value = UINT64_C(1) << 40;
static const uint64_t phase3_value = (UINT64_C(1) << 40) + 1;
static const uint32_t seeds[2] = {UINT32_C(0x5c2d0001), UINT32_C(0x5c2d0002)};
static const uint64_t bounded_wait_ns = UINT64_C(2000000000);

static uint32_t expected_value(uint32_t seed, uint32_t n)
{ return (n * UINT32_C(2654435761)) ^ seed; }
static uint32_t digest_word(uint32_t digest, uint32_t word)
{ return (digest ^ word) * UINT32_C(16777619); }

struct buffer_view {
    VkDevice device;
    VkDeviceMemory memory;
    VkDeviceSize bytes, offset;
};
/* The VALUES words at the descriptor offset must hold `seed` data (or the
 * guard when seed is 0), every other word the guard. */
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

static uint32_t extension_spec(const VkExtensionProperties *list, uint32_t count,
                               const char *name)
{
    for (uint32_t n = 0; n < count; ++n)
        if (!strcmp(list[n].extensionName, name)) return list[n].specVersion;
    return 0;
}

static int run_witness(void)
{
    VkResult result = VK_SUCCESS;
    const char *failed = NULL;
#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { \
    failed = #call; goto cleanup; } } while (0)
#define REQUIRE(condition, reason) do { if (!(condition)) { \
    result = VK_ERROR_UNKNOWN; failed = reason; goto cleanup; } } while (0)
#define PROC(name) ((PFN_##name)vkGetDeviceProcAddr(device, #name))
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
    VkCommandBuffer commands[4] = {VK_NULL_HANDLE};
    VkSemaphore timeline = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBool32 submitted = VK_FALSE;
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
    const uint32_t sync2_spec = extension_spec(extensions, extension_count,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    const uint32_t timeline_spec = extension_spec(extensions, extension_count,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    REQUIRE(sync2_spec && timeline_spec, "synchronization2 and timeline enumerated");
    VkPhysicalDeviceSynchronization2Features reported_sync2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    VkPhysicalDeviceTimelineSemaphoreFeatures reported_timeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .pNext = &reported_sync2};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = &reported_timeline};
    vkGetPhysicalDeviceFeatures2KHR(physical, &features);
    REQUIRE(reported_sync2.synchronization2 && reported_timeline.timelineSemaphore,
            "synchronization2 and timelineSemaphore reported");
    uint32_t family_count = 1;
    VkQueueFamilyProperties family = {0};
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, &family);
    REQUIRE(family_count == 1, "one queue family");

    const char *device_extensions[2] = {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
                                        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME};
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceSynchronization2Features requested_sync2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
        .synchronization2 = VK_TRUE};
    VkPhysicalDeviceTimelineSemaphoreFeatures requested_timeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .pNext = &requested_sync2, .timelineSemaphore = VK_TRUE};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &requested_timeline, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = device_extensions};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    PFN_vkCmdPipelineBarrier2KHR barrier2 = PROC(vkCmdPipelineBarrier2KHR);
    PFN_vkQueueSubmit2KHR submit2 = PROC(vkQueueSubmit2KHR);
    PFN_vkCmdSetEvent2KHR set_event2 = PROC(vkCmdSetEvent2KHR);
    PFN_vkCmdResetEvent2KHR reset_event2 = PROC(vkCmdResetEvent2KHR);
    PFN_vkCmdWaitEvents2KHR wait_events2 = PROC(vkCmdWaitEvents2KHR);
    PFN_vkCmdWriteTimestamp2KHR timestamp2 = PROC(vkCmdWriteTimestamp2KHR);
    PFN_vkGetSemaphoreCounterValueKHR get_value = PROC(vkGetSemaphoreCounterValueKHR);
    PFN_vkWaitSemaphoresKHR wait = PROC(vkWaitSemaphoresKHR);
    PFN_vkSignalSemaphoreKHR signal = PROC(vkSignalSemaphoreKHR);
    REQUIRE(barrier2 && submit2 && set_event2 && reset_event2 && wait_events2 && timestamp2,
            "six synchronization2 commands resolved");
    REQUIRE(!vkGetDeviceProcAddr(device, "vkCmdPipelineBarrier2") &&
            !vkGetDeviceProcAddr(device, "vkQueueSubmit2"),
            "no Vulkan 1.3 core names on the 1.0 device");
    REQUIRE(get_value && wait && signal, "KHR timeline commands available");
    ps5log_printf(PS5LOG_MARK,
        "DXVK_SYNC2_WITNESS_START sync2_spec=%u timeline_spec=%u commands=6 core_names=absent "
        "timestamp_valid_bits=%u initial=%llu values=%u",
        sync2_spec, timeline_spec, family.timestampValidBits,
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
        .commandBufferCount = 4};
    TRY(vkAllocateCommandBuffers(device, &command_info, commands));

    /* The two dependencies, in synchronization2 form only. */
    VkBufferMemoryBarrier2 to_shader = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer, .offset = offset, .size = range};
    VkDependencyInfo acquire = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &to_shader};
    VkMemoryBarrier2 to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo publish = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &to_host};
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    /* Phase 1 buffers: [acquire] [dispatch] [publish]; phase 2: all in one. */
    for (uint32_t n = 0; n < 4; ++n) TRY(vkBeginCommandBuffer(commands[n], &begin));
    barrier2(commands[0], &acquire);
    for (uint32_t n = 1; n < 4; n += 2) {
        if (n == 3) barrier2(commands[n], &acquire);
        vkCmdBindPipeline(commands[n], VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(commands[n], VK_PIPELINE_BIND_POINT_COMPUTE, layout,
                                0, 1, &set, 0, NULL);
        vkCmdPushConstants(commands[n], layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(uint32_t), &seeds[n / 2]);
        vkCmdDispatch(commands[n], 1, 1, 1);
        if (n == 3) barrier2(commands[n], &publish);
    }
    barrier2(commands[2], &publish);
    for (uint32_t n = 0; n < 4; ++n) TRY(vkEndCommandBuffer(commands[n]));

    /* Phase 1: DXVK's three-buffer submission signalling the timeline. */
    VkCommandBufferSubmitInfo cbs[3];
    for (uint32_t n = 0; n < 3; ++n)
        cbs[n] = (VkCommandBufferSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = commands[n], .deviceMask = 0};
    VkSemaphoreSubmitInfo signal_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = timeline, .value = phase1_value,
        .stageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT};
    VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 3, .pCommandBufferInfos = cbs,
        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal_info};
    TRY(submit2(queue, 1, &submit, VK_NULL_HANDLE));
    submitted = VK_TRUE;
    VkSemaphoreWaitInfo wait_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1, .pSemaphores = &timeline, .pValues = &phase1_value};
    TRY(wait(device, &wait_info, bounded_wait_ns));
    uint64_t counter = 0;
    uint32_t mismatches = 0, guard_mismatches = 0, digest = 0;
    TRY(get_value(device, timeline, &counter));
    TRY(check_buffer(&view, seeds[0], &mismatches, &guard_mismatches, &digest));
    ps5log_printf(PS5LOG_MARK,
        "DXVK_SYNC2_WITNESS_RESULT phase=1 buffers=3 wait=success counter=%llu values=%u "
        "mismatches=%u guard_mismatches=%u digest=%08x",
        (unsigned long long)counter, VALUES, mismatches, guard_mismatches, digest);
    REQUIRE(counter == phase1_value && !mismatches && !guard_mismatches,
            "phase 1 GPU data after the named value");

    /* Phase 2: a submit2 waiting at TOP_OF_PIPE on a value not yet signalled. */
    VkSemaphoreSubmitInfo wait_gate = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = timeline, .value = gate_value,
        .stageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT};
    signal_info.value = phase2_value;
    submit = (VkSubmitInfo2){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &wait_gate,
        .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbs[0],
        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal_info};
    cbs[0].commandBuffer = commands[3];
    TRY(submit2(queue, 1, &submit, VK_NULL_HANDLE));
    wait_info.pValues = &phase2_value;
    const VkResult early = wait(device, &wait_info, 0);
    const VkResult early_timed = wait(device, &wait_info, UINT64_C(20000000));
    TRY(get_value(device, timeline, &counter));
    TRY(check_buffer(&view, seeds[0], &mismatches, &guard_mismatches, &digest));
    ps5log_printf(PS5LOG_MARK,
        "DXVK_SYNC2_WITNESS_DEFERRED submit=returned counter=%llu wait_zero=%s "
        "wait_20ms=%s untouched=%s",
        (unsigned long long)counter, early == VK_TIMEOUT ? "timeout" : "other",
        early_timed == VK_TIMEOUT ? "timeout" : "other", mismatches ? "no" : "yes");
    REQUIRE(counter == phase1_value && early == VK_TIMEOUT && early_timed == VK_TIMEOUT &&
            !mismatches, "nothing visible before the host signal");
    VkSemaphoreSignalInfo host = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = timeline, .value = gate_value};
    TRY(signal(device, &host));
    TRY(wait(device, &wait_info, bounded_wait_ns));
    TRY(get_value(device, timeline, &counter));
    TRY(check_buffer(&view, seeds[1], &mismatches, &guard_mismatches, &digest));
    ps5log_printf(PS5LOG_MARK,
        "DXVK_SYNC2_WITNESS_RESULT phase=2 buffers=1 wait=success counter=%llu values=%u "
        "mismatches=%u guard_mismatches=%u digest=%08x",
        (unsigned long long)counter, VALUES, mismatches, guard_mismatches, digest);
    REQUIRE(counter == phase2_value && !mismatches && !guard_mismatches,
            "phase 2 GPU data after the named value");

    /* Phase 3: the empty submit2 that only advances the timeline. */
    signal_info.value = phase3_value;
    submit = (VkSubmitInfo2){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal_info};
    TRY(submit2(queue, 1, &submit, VK_NULL_HANDLE));
    wait_info.pValues = &phase3_value;
    TRY(wait(device, &wait_info, bounded_wait_ns));
    TRY(get_value(device, timeline, &counter));
    TRY(check_buffer(&view, seeds[1], &mismatches, &guard_mismatches, &digest));
    ps5log_printf(PS5LOG_MARK,
        "DXVK_SYNC2_WITNESS_RESULT phase=3 buffers=0 wait=success counter=%llu values=%u "
        "mismatches=%u guard_mismatches=%u digest=%08x",
        (unsigned long long)counter, VALUES, mismatches, guard_mismatches, digest);
    REQUIRE(counter == phase3_value && !mismatches && !guard_mismatches,
            "phase 3 timeline value with the phase 2 data intact");
    TRY(vkDeviceWaitIdle(device));
    submitted = VK_FALSE;

cleanup:
    if (submitted && device && vkDeviceWaitIdle(device) != VK_SUCCESS) {
        ps5log_printf(PS5LOG_ERR,
            "DXVK_SYNC2_WITNESS_FAILURE call=%s result=%d retirement=pending",
            failed ? failed : "idle", (int)result);
        return 1;
    }
    if (pool) {
        vkFreeCommandBuffers(device, pool, 4, commands);
        vkDestroyCommandPool(device, pool, NULL);
    }
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
        ps5log_printf(PS5LOG_MARK, "DXVK_SYNC2_WITNESS_RETIRED resources=clean");
    else
        ps5log_printf(PS5LOG_ERR,
            "DXVK_SYNC2_WITNESS_FAILURE call=%s result=%d retirement=attempted",
            failed ? failed : "unknown", (int)result);
    return result == VK_SUCCESS ? 0 : 1;
#undef PROC
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
    ps5log_close(failed ? "dxvk-sync2-witness-failed" : "dxvk-sync2-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}
