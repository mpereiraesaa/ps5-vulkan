#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "t08_bda_shader.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { BUFFER_COUNT = 3, VALUES = 64, GUARD_WORDS = 8, SOURCE_OFFSET = 4,
       TOTAL_WORDS = GUARD_WORDS + SOURCE_OFFSET + VALUES + GUARD_WORDS };
enum { BUFFER_BYTES = TOTAL_WORDS * sizeof(uint32_t) };
static const uint32_t guard_word = UINT32_C(0xdeadbeef);

struct push_data {
    uint64_t left, right, output;
    uint32_t source_offset;
    uint32_t padding;
};
_Static_assert(sizeof(struct push_data) == 32, "SPIR-V push layout");

static uint32_t left_value(uint32_t n)
{ return (n * 17u) ^ UINT32_C(0x59a31c4d); }
static uint32_t right_value(uint32_t n)
{ return (n * 29u) ^ UINT32_C(0x8124e763); }
static uint32_t result_value(uint32_t n)
{ return (left_value(n) ^ UINT32_C(0xa5a55a5a)) + right_value(n) * 3u; }
static uint32_t digest_word(uint32_t digest, uint32_t word)
{ return (digest ^ word) * UINT32_C(16777619); }

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
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buffers[BUFFER_COUNT] = {VK_NULL_HANDLE};
    VkDeviceMemory memories[BUFFER_COUNT] = {VK_NULL_HANDLE};
    VkDeviceSize sizes[BUFFER_COUNT] = {0};
    VkDeviceSize offsets[BUFFER_COUNT] = {0};
    VkDeviceAddress addresses[BUFFER_COUNT] = {0};
    VkBool32 mapped[BUFFER_COUNT] = {VK_FALSE};
    VkBool32 submitted = VK_FALSE, completed = VK_FALSE;
    uint32_t mismatches = 0, guard_mismatches = 0;
    uint32_t digest = UINT32_C(2166136261);

    const char *instance_extensions[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = instance_extensions,
    };
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t group_count = 0;
    TRY(vkEnumeratePhysicalDeviceGroupsKHR(instance, &group_count, NULL));
    REQUIRE(group_count == 1, "one physical-device group");
    VkPhysicalDeviceGroupProperties group = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES,
    };
    TRY(vkEnumeratePhysicalDeviceGroupsKHR(instance, &group_count, &group));
    REQUIRE(group_count == 1 && group.physicalDeviceCount == 1 &&
            group.physicalDevices[0], "singleton physical-device group");
    VkPhysicalDevice physical = group.physicalDevices[0];

    VkPhysicalDeviceBufferDeviceAddressFeaturesKHR reported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR,
    };
    VkPhysicalDeviceFeatures2 query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &reported,
    };
    vkGetPhysicalDeviceFeatures2KHR(physical, &query);
    REQUIRE(reported.bufferDeviceAddress && !reported.bufferDeviceAddressCaptureReplay &&
            !reported.bufferDeviceAddressMultiDevice, "BDA feature report");

    const char *device_extensions[] = {
        VK_KHR_DEVICE_GROUP_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    };
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkPhysicalDeviceBufferDeviceAddressFeaturesKHR requested = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR,
        .bufferDeviceAddress = VK_TRUE,
    };
    VkDeviceGroupDeviceCreateInfo members = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO,
        .pNext = &requested, .physicalDeviceCount = 1,
        .pPhysicalDevices = &physical,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &members, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = device_extensions,
    };
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    PFN_vkGetBufferDeviceAddressKHR address_query =
        (PFN_vkGetBufferDeviceAddressKHR)vkGetDeviceProcAddr(
            device, "vkGetBufferDeviceAddressKHR");
    REQUIRE(address_query, "KHR address command available");
    ps5log_printf(PS5LOG_MARK,
        "T08_BDA_WITNESS_START values=%u guards=%u source_offset=%u query=khr",
        VALUES, GUARD_WORDS, SOURCE_OFFSET);

    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(t08_bda_spirv), .pCode = t08_bda_spirv,
    };
    TRY(vkCreateShaderModule(device, &shader_info, NULL, &module));
    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0, .size = sizeof(struct push_data),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &range,
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

    for (uint32_t n = 0; n < BUFFER_COUNT; ++n) {
        VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = BUFFER_BYTES,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        TRY(vkCreateBuffer(device, &buffer_info, NULL, &buffers[n]));
        VkBufferDeviceAddressInfo address_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = buffers[n],
        };
        REQUIRE(!address_query(device, &address_info), "unbound address is zero");
        VkMemoryRequirements requirements = {0};
        vkGetBufferMemoryRequirements(device, buffers[n], &requirements);
        REQUIRE(requirements.alignment >= 16 &&
                !(requirements.alignment & (requirements.alignment - 1)) &&
                requirements.size >= BUFFER_BYTES,
                "buffer memory requirements");
        offsets[n] = requirements.alignment;
        sizes[n] = requirements.size + offsets[n];
        VkMemoryAllocateFlagsInfo flags = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT_KHR |
                     VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR,
            .deviceMask = 1,
        };
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &flags, .allocationSize = sizes[n], .memoryTypeIndex = 0,
        };
        TRY(vkAllocateMemory(device, &allocation, NULL, &memories[n]));
        TRY(vkBindBufferMemory(device, buffers[n], memories[n], offsets[n]));
        addresses[n] = address_query(device, &address_info);
        REQUIRE(addresses[n] && !(addresses[n] & 15u),
                "nonzero aligned GPU buffer address");

        uint32_t *words = NULL;
        TRY(vkMapMemory(device, memories[n], 0, VK_WHOLE_SIZE, 0,
                        (void **)&words));
        mapped[n] = VK_TRUE;
        for (VkDeviceSize j = 0; j < sizes[n] / sizeof(uint32_t); ++j)
            words[j] = guard_word;
        if (n < 2) {
            uint32_t base = (uint32_t)(offsets[n] / sizeof(uint32_t)) +
                            GUARD_WORDS + SOURCE_OFFSET;
            for (uint32_t j = 0; j < VALUES; ++j)
                words[base + j] = n ? right_value(j) : left_value(j);
        }
        VkMappedMemoryRange flush = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[n], .offset = 0, .size = VK_WHOLE_SIZE,
        };
        TRY(vkFlushMappedMemoryRanges(device, 1, &flush));
        vkUnmapMemory(device, memories[n]);
        mapped[n] = VK_FALSE;
    }
    REQUIRE(addresses[0] != addresses[1] && addresses[0] != addresses[2] &&
            addresses[1] != addresses[2], "three distinct GPU addresses");

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0,
    };
    TRY(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    TRY(vkAllocateCommandBuffers(device, &command_info, &command));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    TRY(vkBeginCommandBuffer(command, &begin));
    VkBufferMemoryBarrier to_shader[BUFFER_COUNT] = {0};
    for (uint32_t n = 0; n < BUFFER_COUNT; ++n) {
        to_shader[n] = (VkBufferMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = n == 2 ? VK_ACCESS_SHADER_WRITE_BIT :
                                      VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffers[n], .offset = 0, .size = BUFFER_BYTES,
        };
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
        BUFFER_COUNT, to_shader, 0, NULL);
    struct push_data push = {
        .left = addresses[0] + GUARD_WORDS * sizeof(uint32_t),
        .right = addresses[1] + GUARD_WORDS * sizeof(uint32_t),
        .output = addresses[2] + GUARD_WORDS * sizeof(uint32_t),
        .source_offset = SOURCE_OFFSET,
    };
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(command, VALUES, 1, 1);
    VkBufferMemoryBarrier to_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[2], .offset = 0, .size = BUFFER_BYTES,
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

    for (uint32_t n = 0; n < BUFFER_COUNT; ++n) {
        uint32_t *words = NULL;
        TRY(vkMapMemory(device, memories[n], 0, VK_WHOLE_SIZE, 0,
                        (void **)&words));
        mapped[n] = VK_TRUE;
        VkMappedMemoryRange invalidate = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[n], .offset = 0, .size = VK_WHOLE_SIZE,
        };
        TRY(vkInvalidateMappedMemoryRanges(device, 1, &invalidate));
        uint32_t buffer_base = (uint32_t)(offsets[n] / sizeof(uint32_t));
        for (VkDeviceSize j = 0; j < sizes[n] / sizeof(uint32_t); ++j) {
            uint32_t expected = guard_word;
            uint32_t source_base = buffer_base + GUARD_WORDS + SOURCE_OFFSET;
            uint32_t output_base = buffer_base + GUARD_WORDS;
            if (n < 2 && j >= source_base && j < source_base + VALUES)
                expected = n ? right_value((uint32_t)j - source_base) :
                               left_value((uint32_t)j - source_base);
            if (n == 2 && j >= output_base && j < output_base + VALUES) {
                expected = result_value((uint32_t)j - output_base);
                digest = digest_word(digest, words[j]);
            }
            if (words[j] != expected) {
                ++mismatches;
                if (expected == guard_word) ++guard_mismatches;
            }
        }
        vkUnmapMemory(device, memories[n]);
        mapped[n] = VK_FALSE;
    }
    ps5log_printf(PS5LOG_MARK,
        "T08_BDA_WITNESS_RESULT values=%u source_offset=%u bind_offsets=%llu,%llu,%llu mismatches=%u guard_mismatches=%u digest=%08x fence=complete",
        VALUES, SOURCE_OFFSET,
        (unsigned long long)offsets[0], (unsigned long long)offsets[1],
        (unsigned long long)offsets[2], mismatches, guard_mismatches, digest);
    REQUIRE(!mismatches && !guard_mismatches, "exact GPU data and guards");

cleanup:
    if (submitted && !completed && fence && device &&
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(300000000)) == VK_SUCCESS)
        completed = VK_TRUE;
    if (submitted && !completed) {
        ps5log_printf(PS5LOG_ERR,
            "T08_BDA_WITNESS_FAILURE call=%s result=%d retirement=pending",
            failed ? failed : "fence", (int)result);
        return 1;
    }
    if (fence) vkDestroyFence(device, fence, NULL);
    if (command) vkFreeCommandBuffers(device, pool, 1, &command);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    for (uint32_t n = 0; n < BUFFER_COUNT; ++n) {
        if (mapped[n]) vkUnmapMemory(device, memories[n]);
        if (buffers[n]) {
            VkBuffer stale = buffers[n];
            vkDestroyBuffer(device, buffers[n], NULL);
            if (result == VK_SUCCESS) {
                VkBufferDeviceAddressInfo address_info = {
                    .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                    .buffer = stale,
                };
                if (((PFN_vkGetBufferDeviceAddressKHR)vkGetDeviceProcAddr(
                        device, "vkGetBufferDeviceAddressKHR"))(
                            device, &address_info)) {
                    result = VK_ERROR_UNKNOWN;
                    failed = "destroyed address is zero";
                }
            }
        }
        if (memories[n]) vkFreeMemory(device, memories[n], NULL);
    }
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    if (layout) vkDestroyPipelineLayout(device, layout, NULL);
    if (module) vkDestroyShaderModule(device, module, NULL);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS)
        ps5log_printf(PS5LOG_MARK,
            "T08_BDA_WITNESS_RETIRED resources=clean gpu_addresses=distinct");
    else
        ps5log_printf(PS5LOG_ERR,
            "T08_BDA_WITNESS_FAILURE call=%s result=%d retirement=attempted",
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
    ps5log_close(failed ? "t08-bda-witness-failed" : "t08-bda-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}
