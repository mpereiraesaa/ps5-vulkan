/* Public-SDK witness for the Vulkan 1.0 routes DXVK 2.6.2 uses right after
 * device creation: VK_KHR_get_memory_requirements2,
 * VK_KHR_dedicated_allocation, VK_KHR_bind_memory2 and
 * VK_KHR_descriptor_update_template.
 *
 *   1. The *2 requirement queries, with VkMemoryDedicatedRequirements chained,
 *      must agree exactly with the Vulkan 1.0 queries for a buffer and an
 *      image.
 *   2. Buffer A lives in a dedicated allocation (VkMemoryDedicatedAllocateInfo)
 *      and the image in another; both bind through the *2 bind commands.
 *   3. Buffers B0 and B1 share one allocation and bind in ONE
 *      vkBindBufferMemory2KHR call, B1 at a non-zero aligned offset.
 *   4. One descriptor set is written only through
 *      vkUpdateDescriptorSetWithTemplateKHR, once per target buffer; a compute
 *      dispatch then writes a seeded pattern through it, and each buffer is
 *      checked word for word, including the guard words around the range.
 * Every submission waits on a 300 ms fence. */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "t09_timeline_shader.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { VALUES = 64, TARGETS = 3 };
static const uint32_t guard_word = UINT32_C(0xdeadbeef);
static const uint32_t seeds[TARGETS] = {UINT32_C(0x40a70001), UINT32_C(0x40a70002),
                                        UINT32_C(0x40a70003)};
static const uint64_t fence_timeout = UINT64_C(300000000);

static uint32_t expected_value(uint32_t seed, uint32_t n)
{ return (n * UINT32_C(2654435761)) ^ seed; }

static uint32_t extension_spec(const VkExtensionProperties *list, uint32_t count,
                               const char *name)
{
    for (uint32_t n = 0; n < count; ++n)
        if (!strcmp(list[n].extensionName, name)) return list[n].specVersion;
    return 0;
}

/* The template's source record: DXVK packs descriptor infos in its own
 * structures and names them by offset and stride. */
struct template_record {
    uint32_t tag;
    VkDescriptorBufferInfo buffer;
};

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
    VkDescriptorUpdateTemplate update_template = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buffers[TARGETS] = {VK_NULL_HANDLE};
    VkDeviceMemory dedicated = VK_NULL_HANDLE, shared = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkDeviceMemory target_memory[TARGETS] = {VK_NULL_HANDLE};
    VkDeviceSize target_offset[TARGETS] = {0};
    unsigned completed = 0;

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
    VkExtensionProperties extensions[48];
    REQUIRE(extension_count <= 48, "bounded extension list");
    TRY(vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, extensions));
    const char *device_extensions[4] = {
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME};
    uint32_t specs[4];
    for (unsigned n = 0; n < 4; ++n)
        specs[n] = extension_spec(extensions, extension_count, device_extensions[n]);
    REQUIRE(specs[0] && specs[1] && specs[2] && specs[3], "four routes enumerated");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 4, .ppEnabledExtensionNames = device_extensions};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    PFN_vkGetBufferMemoryRequirements2KHR buffer_requirements2 =
        PROC(vkGetBufferMemoryRequirements2KHR);
    PFN_vkGetImageMemoryRequirements2KHR image_requirements2 =
        PROC(vkGetImageMemoryRequirements2KHR);
    PFN_vkBindBufferMemory2KHR bind_buffer2 = PROC(vkBindBufferMemory2KHR);
    PFN_vkBindImageMemory2KHR bind_image2 = PROC(vkBindImageMemory2KHR);
    PFN_vkCreateDescriptorUpdateTemplateKHR create_template =
        PROC(vkCreateDescriptorUpdateTemplateKHR);
    PFN_vkUpdateDescriptorSetWithTemplateKHR update_with_template =
        PROC(vkUpdateDescriptorSetWithTemplateKHR);
    PFN_vkDestroyDescriptorUpdateTemplateKHR destroy_template =
        PROC(vkDestroyDescriptorUpdateTemplateKHR);
    REQUIRE(buffer_requirements2 && image_requirements2 && bind_buffer2 && bind_image2 &&
            create_template && update_with_template && destroy_template,
            "seven KHR commands resolved");
    ps5log_printf(PS5LOG_MARK,
        "DXVK_ROUTES_WITNESS_START memreq2=%u dedicated=%u bind2=%u template=%u commands=7 "
        "values=%u targets=%u", specs[0], specs[1], specs[2], specs[3], VALUES, TARGETS);

    VkPhysicalDeviceProperties core;
    vkGetPhysicalDeviceProperties(physical, &core);
    VkDeviceSize guard = core.limits.minStorageBufferOffsetAlignment;
    if (guard < 16 * sizeof(uint32_t)) guard = 16 * sizeof(uint32_t);
    const VkDeviceSize range = VALUES * sizeof(uint32_t);
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = guard + range + guard, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    for (unsigned n = 0; n < TARGETS; ++n)
        TRY(vkCreateBuffer(device, &buffer_info, NULL, &buffers[n]));

    /* 1. The *2 queries agree with the Vulkan 1.0 ones. */
    VkMemoryRequirements legacy;
    vkGetBufferMemoryRequirements(device, buffers[0], &legacy);
    VkMemoryDedicatedRequirements dedicated_req = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated_req};
    VkBufferMemoryRequirementsInfo2 buffer_query = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = buffers[0]};
    buffer_requirements2(device, &buffer_query, &requirements);
    const int buffer_agrees =
        !memcmp(&legacy, &requirements.memoryRequirements, sizeof(legacy));
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {16, 16, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    TRY(vkCreateImage(device, &image_info, NULL, &image));
    VkMemoryRequirements image_legacy;
    vkGetImageMemoryRequirements(device, image, &image_legacy);
    VkMemoryDedicatedRequirements image_dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 image_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &image_dedicated};
    VkImageMemoryRequirementsInfo2 image_query = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = image};
    image_requirements2(device, &image_query, &image_requirements);
    const int image_agrees =
        !memcmp(&image_legacy, &image_requirements.memoryRequirements, sizeof(image_legacy));
    ps5log_printf(PS5LOG_MARK,
        "DXVK_ROUTES_WITNESS_REQUIREMENTS buffer_size=%llu buffer_alignment=%llu "
        "buffer_agrees=%d buffer_prefers=%u buffer_requires=%u image_size=%llu "
        "image_alignment=%llu image_agrees=%d image_prefers=%u image_requires=%u",
        (unsigned long long)legacy.size, (unsigned long long)legacy.alignment, buffer_agrees,
        dedicated_req.prefersDedicatedAllocation, dedicated_req.requiresDedicatedAllocation,
        (unsigned long long)image_legacy.size, (unsigned long long)image_legacy.alignment,
        image_agrees, image_dedicated.prefersDedicatedAllocation,
        image_dedicated.requiresDedicatedAllocation);
    REQUIRE(buffer_agrees && image_agrees, "*2 requirements agree with Vulkan 1.0");

    /* 2. Dedicated allocations for buffer A and the image, bound with *2. */
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = buffers[0]};
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info, .allocationSize = legacy.size, .memoryTypeIndex = 0};
    TRY(vkAllocateMemory(device, &allocation, NULL, &dedicated));
    VkBindBufferMemoryInfo bind_a = {.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
        .buffer = buffers[0], .memory = dedicated, .memoryOffset = 0};
    TRY(bind_buffer2(device, 1, &bind_a));
    target_memory[0] = dedicated;
    VkMemoryDedicatedAllocateInfo image_dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = image};
    VkMemoryAllocateInfo image_allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &image_dedicated_info, .allocationSize = image_legacy.size,
        .memoryTypeIndex = 0};
    TRY(vkAllocateMemory(device, &image_allocation, NULL, &image_memory));
    VkBindImageMemoryInfo bind_image = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .image = image, .memory = image_memory, .memoryOffset = 0};
    TRY(bind_image2(device, 1, &bind_image));

    /* 3. B0 and B1 in one shared allocation, bound in ONE call. */
    const VkDeviceSize stride = (legacy.size + legacy.alignment - 1) / legacy.alignment *
        legacy.alignment;
    VkMemoryAllocateInfo shared_allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 2 * stride, .memoryTypeIndex = 0};
    TRY(vkAllocateMemory(device, &shared_allocation, NULL, &shared));
    VkBindBufferMemoryInfo bind_shared[2] = {
        {.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO, .buffer = buffers[1],
         .memory = shared, .memoryOffset = 0},
        {.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO, .buffer = buffers[2],
         .memory = shared, .memoryOffset = stride}};
    TRY(bind_buffer2(device, 2, bind_shared));
    target_memory[1] = shared; target_offset[1] = 0;
    target_memory[2] = shared; target_offset[2] = stride;
    ps5log_printf(PS5LOG_MARK,
        "DXVK_ROUTES_WITNESS_BIND dedicated_buffer=1 dedicated_image=1 shared_binds=2 "
        "shared_offset=%llu", (unsigned long long)stride);

    /* Every target starts as guard words. */
    for (unsigned n = 0; n < TARGETS; ++n) {
        /* Map the whole allocation: a flush names a range inside the
         * mapping, and the shared allocation holds two buffers. */
        uint8_t *base = NULL;
        TRY(vkMapMemory(device, target_memory[n], 0, VK_WHOLE_SIZE, 0, (void **)&base));
        uint8_t *bytes = base + target_offset[n];
        for (VkDeviceSize j = 0; j < buffer_info.size / sizeof(uint32_t); ++j)
            memcpy(bytes + j * sizeof(uint32_t), &guard_word, sizeof(guard_word));
        VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = target_memory[n], .offset = 0, .size = VK_WHOLE_SIZE};
        result = vkFlushMappedMemoryRanges(device, 1, &flush);
        vkUnmapMemory(device, target_memory[n]);
        TRY(result);
    }

    /* 4. The descriptor set is written only through the template. */
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
    VkDescriptorUpdateTemplateEntry entry = {.dstBinding = 0, .dstArrayElement = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .offset = offsetof(struct template_record, buffer),
        .stride = sizeof(struct template_record)};
    VkDescriptorUpdateTemplateCreateInfo template_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        .descriptorUpdateEntryCount = 1, .pDescriptorUpdateEntries = &entry,
        .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
        .descriptorSetLayout = set_layout};
    TRY(create_template(device, &template_info, NULL, &update_template));
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
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    TRY(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    TRY(vkAllocateCommandBuffers(device, &command_info, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    VkMemoryBarrier to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

    for (unsigned n = 0; n < TARGETS; ++n) {
        struct template_record record = {UINT32_C(0x7e3a1a7e), {buffers[n], guard, range}};
        update_with_template(device, set, update_template, &record);
        TRY(vkResetCommandBuffer(command, 0));
        TRY(vkBeginCommandBuffer(command, &begin));
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set,
                                0, NULL);
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t),
                           &seeds[n]);
        vkCmdDispatch(command, 1, 1, 1);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0, NULL);
        TRY(vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command};
        TRY(vkQueueSubmit(queue, 1, &submit, fence));
        TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout));
        TRY(vkResetFences(device, 1, &fence));
        ++completed;
        /* Check every target: this one holds its seed, the later ones are
         * still pure guard. */
        for (unsigned t = 0; t < TARGETS; ++t) {
            uint8_t *base = NULL;
            TRY(vkMapMemory(device, target_memory[t], 0, VK_WHOLE_SIZE, 0, (void **)&base));
            const uint8_t *bytes = base + target_offset[t];
            VkMappedMemoryRange invalidate = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = target_memory[t], .offset = 0, .size = VK_WHOLE_SIZE};
            result = vkInvalidateMappedMemoryRanges(device, 1, &invalidate);
            uint32_t mismatches = 0, guard_mismatches = 0;
            const VkDeviceSize first = guard / sizeof(uint32_t);
            for (VkDeviceSize j = 0; j < buffer_info.size / sizeof(uint32_t); ++j) {
                uint32_t word;
                memcpy(&word, bytes + j * sizeof(uint32_t), sizeof(word));
                const int payload = j >= first && j < first + VALUES;
                const uint32_t want = payload && t <= n ?
                    expected_value(seeds[t], (uint32_t)(j - first)) : guard_word;
                if (word != want) { ++mismatches; if (!payload) ++guard_mismatches; }
            }
            vkUnmapMemory(device, target_memory[t]);
            TRY(result);
            if (t == n || n == TARGETS - 1)
                ps5log_printf(PS5LOG_MARK,
                    "DXVK_ROUTES_WITNESS_RESULT dispatch=%u target=%u mismatches=%u "
                    "guard_mismatches=%u fence=complete", n, t, mismatches, guard_mismatches);
            REQUIRE(!mismatches, "template-updated dispatch wrote exactly its target");
        }
    }

cleanup:
    if (device) {
        vkDeviceWaitIdle(device);
        if (fence) vkDestroyFence(device, fence, NULL);
        if (pool) vkDestroyCommandPool(device, pool, NULL);
        if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
        if (layout) vkDestroyPipelineLayout(device, layout, NULL);
        if (update_template) {
            PFN_vkDestroyDescriptorUpdateTemplateKHR destroy_update =
                PROC(vkDestroyDescriptorUpdateTemplateKHR);
            if (destroy_update) destroy_update(device, update_template, NULL);
        }
        if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
        if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
        if (module) vkDestroyShaderModule(device, module, NULL);
        for (unsigned n = 0; n < TARGETS; ++n)
            if (buffers[n]) vkDestroyBuffer(device, buffers[n], NULL);
        if (image) vkDestroyImage(device, image, NULL);
        if (dedicated) vkFreeMemory(device, dedicated, NULL);
        if (shared) vkFreeMemory(device, shared, NULL);
        if (image_memory) vkFreeMemory(device, image_memory, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS)
        ps5log_printf(PS5LOG_MARK, "DXVK_ROUTES_WITNESS_RETIRED resources=clean dispatches=%u",
                      completed);
    else
        ps5log_printf(PS5LOG_ERR, "DXVK_ROUTES_WITNESS_FAILURE call=%s result=%d completed=%u",
                      failed ? failed : "unknown", (int)result, completed);
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
    ps5log_close(failed ? "dxvk-routes-witness-failed" : "dxvk-routes-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}
