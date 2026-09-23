#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "t08_memory_model_shaders.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { GUARD_WORDS = 8, VALUE_WORDS = 128, TOTAL_WORDS = VALUE_WORDS + 2 * GUARD_WORDS };
enum { BUFFER_BYTES = TOTAL_WORDS * sizeof(uint32_t) };
static const uint32_t guard_value = UINT32_C(0xdeadbeef);

#if defined(T08_DEVICE_SCOPE)
static const char scope_name[] = "device";
#else
static const char scope_name[] = "queue-family";
#endif

#define CHECK(call) do { \
    VkResult result_ = (call); \
    if (result_ != VK_SUCCESS) { \
        ps5log_printf(PS5LOG_ERR, "T08_MEMORY_MODEL_CHECK call=%s result=%d", \
                      #call, (int)result_); \
        ps5log_close("check-failed"); \
        exit(1); \
    } \
} while (0)

#define REQUIRE(test, reason) do { \
    if (!(test)) { \
        ps5log_printf(PS5LOG_ERR, "T08_MEMORY_MODEL_REQUIRE reason=%s", reason); \
        ps5log_close("require-failed"); \
        exit(1); \
    } \
} while (0)

static uint32_t produced(uint32_t index)
{ return (index * 17u) ^ UINT32_C(0x59a31c4d); }

static uint32_t consumed(uint32_t index)
{ return produced(index) ^ UINT32_C(0x9e3779b9); }

static uint32_t digest_word(uint32_t digest, uint32_t word)
{ return (digest ^ word) * UINT32_C(16777619); }

/* One dispatch pairs producer and consumer workgroups. A consumer that sees
 * the atomic flag judges the preceding payload; a consumer that runs first
 * records a skip. There is no API barrier between the actors. */
static void run_litmus(VkDevice device, VkQueue queue)
{
    enum { PAIRS = 1024, WORDS = PAIRS + 2 * GUARD_WORDS,
           BYTES = WORDS * sizeof(uint32_t) };
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(t08_litmus_spirv), .pCode = t08_litmus_spirv,
    };
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &module));
    VkDescriptorSetLayoutBinding bindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bindings,
    };
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module, .pName = "main"},
        .layout = layout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                   &pipeline_info, NULL, &pipeline));

    VkBuffer buffers[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory memories[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    for (unsigned n = 0; n < 3; ++n) {
        VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = BYTES, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        };
        CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffers[n]));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffers[n], &requirements);
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = 0,
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &memories[n]));
        CHECK(vkBindBufferMemory(device, buffers[n], memories[n], 0));
        uint32_t *mapped = NULL;
        CHECK(vkMapMemory(device, memories[n], 0, VK_WHOLE_SIZE, 0,
                          (void **)&mapped));
        for (unsigned j = 0; j < WORDS; ++j)
            mapped[j] = j < GUARD_WORDS || j >= GUARD_WORDS + PAIRS ?
                        guard_value : (n == 2 ? UINT32_MAX : 0u);
        VkMappedMemoryRange flush = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[n], .offset = 0, .size = VK_WHOLE_SIZE,
        };
        CHECK(vkFlushMappedMemoryRanges(device, 1, &flush));
        vkUnmapMemory(device, memories[n]);
    }
    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3,
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size,
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool));
    VkDescriptorSetAllocateInfo set_allocation = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    CHECK(vkAllocateDescriptorSets(device, &set_allocation, &set));
    VkDescriptorBufferInfo descriptor_buffers[3];
    VkWriteDescriptorSet writes[3];
    memset(writes, 0, sizeof(writes));
    for (unsigned n = 0; n < 3; ++n) {
        descriptor_buffers[n] = (VkDescriptorBufferInfo){
            .buffer = buffers[n], .offset = 0, .range = BYTES};
        writes[n] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set, .dstBinding = n, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &descriptor_buffers[n],
        };
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);

    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    VkCommandBufferAllocateInfo command_allocation = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &command_allocation, &command));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    CHECK(vkBeginCommandBuffer(command, &begin));
    VkBufferMemoryBarrier host_barriers[3];
    for (unsigned n = 0; n < 3; ++n) {
        host_barriers[n] = (VkBufferMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffers[n], .offset = 0, .size = BYTES,
        };
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 3, host_barriers, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(command, 64, 1, 1);
    VkBufferMemoryBarrier to_host[3];
    for (unsigned n = 0; n < 3; ++n) {
        to_host[n] = (VkBufferMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffers[n], .offset = 0, .size = BYTES,
        };
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 3, to_host, 0, NULL);
    CHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command,
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(500000000)));

    uint32_t *mapped[3] = {NULL, NULL, NULL};
    unsigned observed = 0, skipped = 0, failures = 0, guard_mismatches = 0;
    for (unsigned n = 0; n < 3; ++n) {
        CHECK(vkMapMemory(device, memories[n], 0, VK_WHOLE_SIZE, 0,
                          (void **)&mapped[n]));
        VkMappedMemoryRange invalidate = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[n], .offset = 0, .size = VK_WHOLE_SIZE,
        };
        CHECK(vkInvalidateMappedMemoryRanges(device, 1, &invalidate));
        for (unsigned j = 0; j < GUARD_WORDS; ++j)
            guard_mismatches += mapped[n][j] != guard_value;
        for (unsigned j = GUARD_WORDS + PAIRS; j < WORDS; ++j)
            guard_mismatches += mapped[n][j] != guard_value;
    }
    for (unsigned j = 0; j < PAIRS; ++j) {
        uint32_t expected = produced(j);
        failures += mapped[0][GUARD_WORDS + j] != expected;
        failures += mapped[1][GUARD_WORDS + j] != 1u;
        uint32_t value = mapped[2][GUARD_WORDS + j];
        observed += value == 1u;
        skipped += value == 0u;
        failures += value != 0u && value != 1u;
    }
    ps5log_printf(PS5LOG_MARK,
        "T08_MEMORY_MODEL_LITMUS scope=%s pairs=%u observed=%u skipped=%u failures=%u guard_mismatches=%u fence=complete",
        scope_name, PAIRS, observed, skipped, failures, guard_mismatches);
    REQUIRE(observed >= 32 && observed + skipped == PAIRS &&
            !failures && !guard_mismatches, "same-dispatch message passing");
    for (unsigned n = 0; n < 3; ++n) vkUnmapMemory(device, memories[n]);
    vkDestroyFence(device, fence, NULL);
    vkFreeCommandBuffers(device, command_pool, 1, &command);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyDescriptorPool(device, pool, NULL);
    for (unsigned n = 0; n < 3; ++n) {
        vkDestroyBuffer(device, buffers[n], NULL);
        vkFreeMemory(device, memories[n], NULL);
    }
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, module, NULL);
}

static void run_witness(void)
{
    const char *instance_extensions[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = instance_extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical, "one physical device");

    VkPhysicalDeviceVulkanMemoryModelFeaturesKHR reported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES_KHR,
    };
    VkPhysicalDeviceFeatures2 query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &reported,
    };
    vkGetPhysicalDeviceFeatures2KHR(physical, &query);
    REQUIRE(reported.vulkanMemoryModel, "memory model report");
#if defined(T08_DEVICE_SCOPE)
    REQUIRE(reported.vulkanMemoryModelDeviceScope, "device scope report");
#endif
    REQUIRE(!reported.vulkanMemoryModelAvailabilityVisibilityChains,
            "unsupported availability-visibility chains stay false");

    const char *device_extensions[] = {
        VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME,
        VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME,
    };
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkPhysicalDeviceVulkanMemoryModelFeaturesKHR requested = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES_KHR,
        .vulkanMemoryModelDeviceScope = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &requested,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &features,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = device_extensions,
    };
    VkDevice rejected = VK_NULL_HANDLE;
    VkResult negative = vkCreateDevice(physical, &device_info, NULL, &rejected);
    REQUIRE(negative == VK_ERROR_FEATURE_NOT_PRESENT && !rejected,
            "device scope requires base model");
    requested.vulkanMemoryModel = VK_TRUE;
#if !defined(T08_DEVICE_SCOPE)
    requested.vulkanMemoryModelDeviceScope = VK_FALSE;
#endif
    VkDevice device = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    ps5log_printf(PS5LOG_MARK,
        "T08_MEMORY_MODEL_WITNESS_START scope=%s values=%u guards=%u negative_gate=pass",
        scope_name, VALUE_WORDS, 2u * GUARD_WORDS);

    const uint32_t *shader_words[2] = {t08_producer_spirv, t08_consumer_spirv};
    const size_t shader_bytes[2] = {sizeof(t08_producer_spirv), sizeof(t08_consumer_spirv)};
    VkShaderModule modules[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    for (unsigned n = 0; n < 2; ++n) {
        VkShaderModuleCreateInfo shader_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = shader_bytes[n], .pCode = shader_words[n],
        };
        CHECK(vkCreateShaderModule(device, &shader_info, NULL, &modules[n]));
    }
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings,
    };
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkPipeline pipelines[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    for (unsigned n = 0; n < 2; ++n) {
        VkComputePipelineCreateInfo pipeline_info = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = modules[n], .pName = "main"},
            .layout = layout,
        };
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                       &pipeline_info, NULL, &pipelines[n]));
    }

    VkBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory memories[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    for (unsigned n = 0; n < 2; ++n) {
        VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = BUFFER_BYTES, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        };
        CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffers[n]));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffers[n], &requirements);
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = 0,
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &memories[n]));
        CHECK(vkBindBufferMemory(device, buffers[n], memories[n], 0));
        uint32_t *mapped = NULL;
        CHECK(vkMapMemory(device, memories[n], 0, VK_WHOLE_SIZE, 0,
                          (void **)&mapped));
        for (unsigned j = 0; j < TOTAL_WORDS; ++j) mapped[j] = guard_value;
        VkMappedMemoryRange flush = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[n], .offset = 0, .size = VK_WHOLE_SIZE,
        };
        CHECK(vkFlushMappedMemoryRanges(device, 1, &flush));
        vkUnmapMemory(device, memories[n]);
    }

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2,
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size,
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool));
    VkDescriptorSetAllocateInfo set_allocation = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    CHECK(vkAllocateDescriptorSets(device, &set_allocation, &set));
    VkDescriptorBufferInfo descriptor_buffers[2] = {
        {.buffer = buffers[0], .offset = 0, .range = BUFFER_BYTES},
        {.buffer = buffers[1], .offset = 0, .range = BUFFER_BYTES},
    };
    VkWriteDescriptorSet writes[2] = {{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &descriptor_buffers[0],
    }, {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &descriptor_buffers[1],
    }};
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    VkCommandBufferAllocateInfo command_allocation = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &command_allocation, &command));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    CHECK(vkBeginCommandBuffer(command, &begin));
    VkBufferMemoryBarrier host_barriers[2] = {{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[0], .offset = 0, .size = BUFFER_BYTES,
    }, {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[1], .offset = 0, .size = BUFFER_BYTES,
    }};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 2, host_barriers, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[0]);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(command, VALUE_WORDS / 32, 1, 1);
    VkBufferMemoryBarrier between = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[0], .offset = 0, .size = BUFFER_BYTES,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, &between, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[1]);
    vkCmdDispatch(command, VALUE_WORDS / 32, 1, 1);
    VkBufferMemoryBarrier to_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[1], .offset = 0, .size = BUFFER_BYTES,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &to_host, 0, NULL);
    CHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command,
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(500000000)));

    uint32_t *mapped[2] = {NULL, NULL};
    for (unsigned n = 0; n < 2; ++n) {
        CHECK(vkMapMemory(device, memories[n], 0, VK_WHOLE_SIZE, 0,
                          (void **)&mapped[n]));
        VkMappedMemoryRange invalidate = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[n], .offset = 0, .size = VK_WHOLE_SIZE,
        };
        CHECK(vkInvalidateMappedMemoryRanges(device, 1, &invalidate));
    }
    unsigned mismatches = 0, guard_mismatches = 0;
    uint32_t digest = UINT32_C(2166136261);
    for (unsigned n = 0; n < 2; ++n) {
        for (unsigned j = 0; j < GUARD_WORDS; ++j)
            guard_mismatches += mapped[n][j] != guard_value;
        for (unsigned j = GUARD_WORDS + VALUE_WORDS; j < TOTAL_WORDS; ++j)
            guard_mismatches += mapped[n][j] != guard_value;
    }
    for (unsigned j = 0; j < VALUE_WORDS; ++j) {
        mismatches += mapped[0][GUARD_WORDS + j] != produced(j);
        mismatches += mapped[1][GUARD_WORDS + j] != consumed(j);
        digest = digest_word(digest, mapped[1][GUARD_WORDS + j]);
    }
    ps5log_printf(PS5LOG_MARK,
        "T08_MEMORY_MODEL_WITNESS_RESULT scope=%s values=%u mismatches=%u guard_mismatches=%u digest=%08x fence=complete",
        scope_name, VALUE_WORDS, mismatches, guard_mismatches, digest);
    REQUIRE(!mismatches && !guard_mismatches, "exact data and guards");
    for (unsigned n = 0; n < 2; ++n) vkUnmapMemory(device, memories[n]);

    vkDestroyFence(device, fence, NULL);
    vkFreeCommandBuffers(device, command_pool, 1, &command);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyDescriptorPool(device, pool, NULL);
    for (unsigned n = 0; n < 2; ++n) {
        vkDestroyBuffer(device, buffers[n], NULL);
        vkFreeMemory(device, memories[n], NULL);
        vkDestroyPipeline(device, pipelines[n], NULL);
        vkDestroyShaderModule(device, modules[n], NULL);
    }
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    run_litmus(device, queue);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    ps5log_printf(PS5LOG_MARK,
        "T08_MEMORY_MODEL_WITNESS_RETIRED scope=%s resources=clean", scope_name);
    ps5log_close("t08-memory-model-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
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
    run_witness();
    return 0;
}
