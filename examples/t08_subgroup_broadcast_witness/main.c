#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "t08_subgroup_broadcast_shader.h"

#include <stdint.h>
#include <time.h>
#include <unistd.h>

enum { SUBGROUPS = 4, OUTPUTS = 128, OUTPUT_OFFSET = 16,
       WORDS = OUTPUT_OFFSET + OUTPUTS + 8 };
static const uint32_t sentinel = UINT32_C(0xdeadbeef);
static const uint32_t source_lanes[SUBGROUPS] = {7, 19, 31, 1};

static uint32_t digest_word(uint32_t digest, uint32_t word)
{ return (digest ^ word) * UINT32_C(16777619); }

static int witness(void)
{
    VkResult result = VK_SUCCESS;
    const char *failed = NULL;
#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { \
    failed = #call; goto cleanup; } } while (0)
#define REQUIRE(ok, reason) do { if (!(ok)) { \
    result = VK_ERROR_UNKNOWN; failed = reason; goto cleanup; } } while (0)
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBool32 mapped = VK_FALSE, submitted = VK_FALSE, completed = VK_FALSE;
    uint32_t mismatches = 0, guards = 0;
    uint32_t digest = UINT32_C(2166136261);

    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    };
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t physical_count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &physical_count, &physical));
    REQUIRE(physical_count == 1 && physical, "one physical device");
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    REQUIRE(properties.apiVersion == VK_API_VERSION_1_0,
            "diagnostic API remains 1.0");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
    };
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    ps5log_printf(PS5LOG_MARK,
        "T08_SUBGROUP_START subgroups=4 outputs=128 ids=7,19,31,1 api=1.0");

    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(t08_subgroup_broadcast_spirv),
        .pCode = t08_subgroup_broadcast_spirv,
    };
    TRY(vkCreateShaderModule(device, &shader_info, NULL, &module));
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding,
    };
    TRY(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout,
    };
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module, .pName = "main"},
        .layout = layout,
    };
    TRY(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                 &pipeline_info, NULL, &pipeline));

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = WORDS * sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    TRY(vkCreateBuffer(device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    REQUIRE(requirements.size >= WORDS * sizeof(uint32_t),
            "buffer allocation size");
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = 0,
    };
    TRY(vkAllocateMemory(device, &allocation, NULL, &memory));
    TRY(vkBindBufferMemory(device, buffer, memory, 0));
    uint32_t *words = NULL;
    TRY(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&words));
    mapped = VK_TRUE;
    for (VkDeviceSize i = 0; i < requirements.size / sizeof(uint32_t); ++i)
        words[i] = sentinel;
    for (uint32_t i = 0; i < SUBGROUPS; ++i)
        words[i] = source_lanes[i];
    VkMappedMemoryRange mapped_range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    TRY(vkFlushMappedMemoryRanges(device, 1, &mapped_range));
    vkUnmapMemory(device, memory);
    mapped = VK_FALSE;

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size,
    };
    TRY(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo descriptor_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    };
    TRY(vkAllocateDescriptorSets(device, &descriptor_info, &descriptor));
    VkDescriptorBufferInfo buffer_descriptor = {
        .buffer = buffer, .offset = 0,
        .range = WORDS * sizeof(uint32_t),
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_descriptor,
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0,
    };
    TRY(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    TRY(vkAllocateCommandBuffers(device, &command_info, &command));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    TRY(vkBeginCommandBuffer(command, &begin));
    VkBufferMemoryBarrier to_shader = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, &to_shader, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            layout, 0, 1, &descriptor, 0, NULL);
    vkCmdDispatch(command, 2, 1, 1);
    VkBufferMemoryBarrier to_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &to_host, 0, NULL);
    TRY(vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command,
    };
    TRY(vkQueueSubmit(queue, 1, &submit, fence));
    submitted = VK_TRUE;
    TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(500000000)));
    completed = VK_TRUE;
    TRY(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&words));
    mapped = VK_TRUE;
    TRY(vkInvalidateMappedMemoryRanges(device, 1, &mapped_range));
    for (uint32_t i = 0; i < SUBGROUPS; ++i)
        guards += words[i] != source_lanes[i];
    for (uint32_t i = SUBGROUPS; i < OUTPUT_OFFSET; ++i)
        guards += words[i] != sentinel;
    for (uint32_t i = 0; i < OUTPUTS; ++i) {
        uint32_t subgroup = i / 32;
        uint32_t expected = (subgroup / 2) * 1000u +
                            (subgroup % 2) * 100u + source_lanes[subgroup];
        uint32_t actual = words[OUTPUT_OFFSET + i];
        mismatches += actual != expected;
        digest = digest_word(digest, actual);
    }
    for (uint32_t i = OUTPUT_OFFSET + OUTPUTS; i < WORDS; ++i)
        guards += words[i] != sentinel;
    ps5log_printf(PS5LOG_MARK,
        "T08_SUBGROUP_RESULT outputs=%u mismatches=%u guards=%u digest=%08x fence=complete",
        OUTPUTS, mismatches, guards, digest);
    REQUIRE(!mismatches && !guards, "exact GPU outputs and guards");

cleanup:
    if (submitted && !completed && fence && device &&
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(300000000)) == VK_SUCCESS)
        completed = VK_TRUE;
    if (submitted && !completed) {
        ps5log_printf(PS5LOG_ERR,
            "T08_SUBGROUP_FAILURE call=%s result=%d retirement=pending",
            failed ? failed : "fence", (int)result);
        return 1;
    }
    if (mapped) vkUnmapMemory(device, memory);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (command) vkFreeCommandBuffers(device, command_pool, 1, &command);
    if (command_pool) vkDestroyCommandPool(device, command_pool, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (buffer) vkDestroyBuffer(device, buffer, NULL);
    if (memory) vkFreeMemory(device, memory, NULL);
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    if (layout) vkDestroyPipelineLayout(device, layout, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    if (module) vkDestroyShaderModule(device, module, NULL);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS)
        ps5log_printf(PS5LOG_MARK, "T08_SUBGROUP_RETIRED resources=clean");
    else
        ps5log_printf(PS5LOG_ERR,
            "T08_SUBGROUP_FAILURE call=%s result=%d retirement=attempted",
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
    int failed = witness();
    ps5log_close(failed ? "t08-subgroup-failed" : "t08-subgroup-end");
    for (;;) sleep(1);
}
