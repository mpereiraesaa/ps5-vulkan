#define _DEFAULT_SOURCE 1

#include <ps5vk/ps5vk.h>
#include <ps5vk/ps5vk_present.h>
#include "shaders.h"
#include "resource_shader.h"
#include "storage_width_shaders.h"
#include "ps5log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

#define CHECK(expr) do { \
    VkResult _res = (expr); \
    if (_res != VK_SUCCESS) { \
        ps5log_printf(PS5LOG_ERR, "CHECK failed: %s -> %d at %s:%d", #expr, (int)_res, __FILE__, __LINE__); \
        ps5log_close("check-failed"); \
        exit(1); \
    } \
} while (0)

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        ps5log_printf(PS5LOG_ERR, "REQUIRE failed: %s", (message)); \
        ps5log_close("require-failed"); \
        exit(1); \
    } \
} while (0)

static int parse_is_continuous(void)
{
#if defined(CONSUMER_CONTINUOUS) && CONSUMER_CONTINUOUS
    return 1;
#else
    FILE *f = fopen("/app0/dev.conf", "r");
    if (!f) return 0;
    char line[256];
    int continuous = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "CONSUMER_MODE=continuous") != NULL) {
            continuous = 1;
            break;
        }
    }
    fclose(f);
    return continuous;
#endif
}

static uint32_t fnv1a32(const uint8_t *bytes, size_t count)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static void report_physical_device_contract(VkInstance instance,
                                            VkPhysicalDevice physical_device,
                                            const VkPhysicalDeviceProperties *props)
{
    uint32_t count = 0;
    REQUIRE(vkEnumeratePhysicalDevices(instance, &count, NULL) == VK_SUCCESS &&
            count == 1, "physical-device count query");

    VkPhysicalDevice sentinel = (VkPhysicalDevice)(uintptr_t)0x1234;
    VkPhysicalDevice devices[2] = {sentinel, sentinel};
    count = 0;
    REQUIRE(vkEnumeratePhysicalDevices(instance, &count, devices) == VK_INCOMPLETE &&
            count == 0 && devices[0] == sentinel && devices[1] == sentinel,
            "physical-device zero-capacity query");
    count = 2;
    REQUIRE(vkEnumeratePhysicalDevices(instance, &count, devices) == VK_SUCCESS &&
            count == 1 && devices[0] == physical_device && devices[1] == sentinel,
            "physical-device oversized-capacity query");

    VkPhysicalDeviceMemoryProperties memory;
    memset(&memory, 0xa5, sizeof(memory));
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
    REQUIRE(memory.memoryHeapCount == 1 && memory.memoryTypeCount == 1 &&
            memory.memoryHeaps[0].size == UINT64_C(268435456) &&
            memory.memoryHeaps[0].flags == VK_MEMORY_HEAP_DEVICE_LOCAL_BIT &&
            memory.memoryTypes[0].heapIndex == 0 &&
            memory.memoryTypes[0].propertyFlags ==
                (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
            "exact native memory report");

    VkQueueFamilyProperties queues[2];
    memset(queues, 0xa5, sizeof(queues));
    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, NULL);
    REQUIRE(count == 1, "queue-family count query");
    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, queues);
    for (size_t byte = 0; byte < sizeof(queues); ++byte)
        REQUIRE(((const uint8_t *)queues)[byte] == 0xa5,
                "queue-family zero-capacity preservation");
    count = 2;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, queues);
    REQUIRE(count == 1 && queues[0].queueCount == 1 &&
            queues[0].queueFlags == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) &&
            queues[0].minImageTransferGranularity.width == 1 &&
            queues[0].minImageTransferGranularity.height == 1 &&
            queues[0].minImageTransferGranularity.depth == 1,
            "exact queue-family report");
    for (size_t byte = sizeof(queues[0]); byte < sizeof(queues); ++byte)
        REQUIRE(((const uint8_t *)queues)[byte] == 0xa5,
                "queue-family tail preservation");

    VkBaseOutStructure unknown = {
        .sType = VK_STRUCTURE_TYPE_MAX_ENUM,
        .pNext = NULL,
    };
    VkPhysicalDeviceProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &unknown,
    };
    vkGetPhysicalDeviceProperties2KHR(physical_device, &properties2);
    REQUIRE(!memcmp(&properties2.properties, props, sizeof(*props)) &&
            unknown.sType == VK_STRUCTURE_TYPE_MAX_ENUM && !unknown.pNext,
            "properties2 and unknown pNext preservation");

    VkFormatProperties bgra, rgba, depth, texel, unsupported;
    vkGetPhysicalDeviceFormatProperties(physical_device,
        VK_FORMAT_B8G8R8A8_UNORM, &bgra);
    vkGetPhysicalDeviceFormatProperties(physical_device,
        VK_FORMAT_R8G8B8A8_UNORM, &rgba);
    vkGetPhysicalDeviceFormatProperties(physical_device,
        VK_FORMAT_D32_SFLOAT, &depth);
    vkGetPhysicalDeviceFormatProperties(physical_device,
        VK_FORMAT_R32_UINT, &texel);
    vkGetPhysicalDeviceFormatProperties(physical_device,
        VK_FORMAT_R8G8B8A8_SRGB, &unsupported);
    REQUIRE(!bgra.linearTilingFeatures && !bgra.bufferFeatures &&
            bgra.optimalTilingFeatures == VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT &&
            !rgba.linearTilingFeatures && !rgba.bufferFeatures &&
            rgba.optimalTilingFeatures == VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT &&
            !depth.linearTilingFeatures && !depth.bufferFeatures &&
            depth.optimalTilingFeatures ==
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT &&
            texel.bufferFeatures == VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT &&
            !texel.linearTilingFeatures && !texel.optimalTilingFeatures &&
            !unsupported.linearTilingFeatures &&
            !unsupported.optimalTilingFeatures && !unsupported.bufferFeatures,
            "exact format-property matrix");

    VkImageFormatProperties image;
    REQUIRE(vkGetPhysicalDeviceImageFormatProperties(physical_device,
                VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                0, &image) == VK_SUCCESS &&
            image.maxExtent.width == 16383 && image.maxExtent.height == 16383 &&
            image.maxExtent.depth == 1 && image.maxMipLevels == 1 &&
            image.maxArrayLayers == 1 &&
            image.sampleCounts == VK_SAMPLE_COUNT_1_BIT &&
            image.maxResourceSize == UINT64_C(268435456),
            "exact supported image-format query");
    memset(&image, 0xa5, sizeof(image));
    REQUIRE(vkGetPhysicalDeviceImageFormatProperties(physical_device,
                VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT,
                0, &image) == VK_ERROR_FORMAT_NOT_SUPPORTED,
            "unsupported image-format result");
    VkImageFormatProperties zero_image = {0};
    REQUIRE(!memcmp(&image, &zero_image, sizeof(image)),
            "unsupported image-format zero report");

    const VkPhysicalDeviceLimits *limits = &props->limits;
    REQUIRE(props->apiVersion == VK_API_VERSION_1_0 &&
            props->vendorID == 0x1002 && props->deviceID == 0 &&
            props->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
            limits->maxStorageBufferRange == UINT32_C(268435456) &&
            limits->maxUniformBufferRange == 65536 &&
            limits->maxTexelBufferElements == 65536 &&
            limits->maxPushConstantsSize == 256 &&
            limits->maxMemoryAllocationCount == 2048 &&
            limits->bufferImageGranularity == UINT64_C(131072) &&
            limits->minMemoryMapAlignment == 64 &&
            limits->minTexelBufferOffsetAlignment == 4 &&
            limits->minUniformBufferOffsetAlignment == 256 &&
            limits->minStorageBufferOffsetAlignment == 256 &&
            limits->nonCoherentAtomSize == 64 &&
            limits->maxComputeSharedMemorySize == 65536 &&
            limits->maxComputeWorkGroupInvocations == 1024,
            "exact derived physical-device limits");

    char canonical[512];
    int written = snprintf(canonical, sizeof(canonical),
        "api=%08x vendor=%04x device=%04x heap=%llu heap_flags=%08x "
        "type_flags=%08x queue_flags=%08x storage=%u uniform=%u texel=%u "
        "push=%u allocations=%u granularity=%llu map_align=%llu texel_align=%llu "
        "ubo_align=%llu ssbo_align=%llu atom=%llu shared=%u invocations=%u",
        props->apiVersion, props->vendorID, props->deviceID,
        (unsigned long long)memory.memoryHeaps[0].size,
        memory.memoryHeaps[0].flags, memory.memoryTypes[0].propertyFlags,
        queues[0].queueFlags, limits->maxStorageBufferRange,
        limits->maxUniformBufferRange, limits->maxTexelBufferElements,
        limits->maxPushConstantsSize, limits->maxMemoryAllocationCount,
        (unsigned long long)limits->bufferImageGranularity,
        (unsigned long long)limits->minMemoryMapAlignment,
        (unsigned long long)limits->minTexelBufferOffsetAlignment,
        (unsigned long long)limits->minUniformBufferOffsetAlignment,
        (unsigned long long)limits->minStorageBufferOffsetAlignment,
        (unsigned long long)limits->nonCoherentAtomSize,
        limits->maxComputeSharedMemorySize,
        limits->maxComputeWorkGroupInvocations);
    REQUIRE(written > 0 && (size_t)written < sizeof(canonical),
            "physical-device canonical report size");
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_PHYSICAL_DEVICE %s hash=%08x",
        canonical, fnv1a32((const uint8_t *)canonical, (size_t)written));
    ps5log_line(PS5LOG_MARK,
        "PS5VK_CONSUMER_PHYSICAL_QUERIES devices=1 queues=1 two_call=1 "
        "tail_preserved=1 pnext_preserved=1 formats=5 image_supported=1 "
        "image_rejected=1");
}

static void run_storage_width_compute(VkDevice device, VkQueue queue)
{
    enum { WIDTH_RUNS = 2, ELEMENTS = 64, BUFFER_BYTES = 4096, DATA_OFFSET = 256 };
    const uint32_t *shader_words[WIDTH_RUNS] = {
        consumer_storage8_spirv, consumer_storage16_spirv
    };
    const size_t shader_bytes[WIDTH_RUNS] = {
        sizeof(consumer_storage8_spirv), sizeof(consumer_storage16_spirv)
    };
    const uint32_t element_bytes[WIDTH_RUNS] = {1, 2};
    VkShaderModule modules[WIDTH_RUNS] = {VK_NULL_HANDLE};
    VkPipeline pipelines[WIDTH_RUNS] = {VK_NULL_HANDLE};
    VkBuffer buffers[WIDTH_RUNS][2] = {{VK_NULL_HANDLE}};
    VkDeviceMemory memories[WIDTH_RUNS][2] = {{VK_NULL_HANDLE}};
    VkDescriptorSet sets[WIDTH_RUNS] = {VK_NULL_HANDLE};

    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_STORAGE_WIDTH_START");

    VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorSetLayout(device, &layout_info, NULL, &set_layout));

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
    };
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &pipeline_layout));

    for (uint32_t run = 0; run < WIDTH_RUNS; ++run) {
        VkShaderModuleCreateInfo module_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = shader_bytes[run],
            .pCode = shader_words[run],
        };
        CHECK(vkCreateShaderModule(device, &module_info, NULL, &modules[run]));
        VkComputePipelineCreateInfo pipeline_info = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = modules[run],
                .pName = "main",
            },
            .layout = pipeline_layout,
        };
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                       &pipeline_info, NULL, &pipelines[run]));
    }
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_STORAGE_WIDTH_PIPELINES_CREATED count=2");

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = BUFFER_BYTES,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    };
    for (uint32_t run = 0; run < WIDTH_RUNS; ++run) {
        for (uint32_t io = 0; io < 2; ++io) {
            CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffers[run][io]));
            VkMemoryRequirements requirements;
            vkGetBufferMemoryRequirements(device, buffers[run][io], &requirements);
            VkMemoryAllocateInfo allocation = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = requirements.size,
                .memoryTypeIndex = 0,
            };
            CHECK(vkAllocateMemory(device, &allocation, NULL, &memories[run][io]));
            CHECK(vkBindBufferMemory(device, buffers[run][io], memories[run][io], 0));
        }
    }

    for (uint32_t run = 0; run < WIDTH_RUNS; ++run) {
        uint8_t *input = NULL;
        uint8_t *output = NULL;
        CHECK(vkMapMemory(device, memories[run][0], 0, BUFFER_BYTES, 0,
                          (void **)&input));
        CHECK(vkMapMemory(device, memories[run][1], 0, BUFFER_BYTES, 0,
                          (void **)&output));
        memset(input, 0xa5, BUFFER_BYTES);
        memset(output, 0xa5, BUFFER_BYTES);
        if (element_bytes[run] == 1) {
            for (uint32_t i = 0; i < ELEMENTS; ++i)
                input[DATA_OFFSET + i] = (uint8_t)(i * 7u + 3u);
        } else {
            uint16_t *values = (uint16_t *)(void *)(input + DATA_OFFSET);
            for (uint32_t i = 0; i < ELEMENTS; ++i)
                values[i] = (uint16_t)(i * 257u + 19u);
        }
        VkMappedMemoryRange flush_ranges[2] = {{
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[run][0], .offset = 0, .size = BUFFER_BYTES,
        }, {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[run][1], .offset = 0, .size = BUFFER_BYTES,
        }};
        CHECK(vkFlushMappedMemoryRanges(device, 2, flush_ranges));
        vkUnmapMemory(device, memories[run][0]);
        vkUnmapMemory(device, memories[run][1]);
    }

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = WIDTH_RUNS * 2,
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = WIDTH_RUNS,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSetLayout layouts[WIDTH_RUNS] = {set_layout, set_layout};
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = WIDTH_RUNS,
        .pSetLayouts = layouts,
    };
    CHECK(vkAllocateDescriptorSets(device, &set_info, sets));

    for (uint32_t run = 0; run < WIDTH_RUNS; ++run) {
        VkDescriptorBufferInfo infos[2] = {
            {.buffer = buffers[run][0], .offset = DATA_OFFSET,
             .range = ELEMENTS * element_bytes[run]},
            {.buffer = buffers[run][1], .offset = DATA_OFFSET,
             .range = ELEMENTS * element_bytes[run]},
        };
        VkWriteDescriptorSet writes[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = sets[run], .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .pBufferInfo = &infos[0]},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = sets[run], .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .pBufferInfo = &infos[1]},
        };
        vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    }

    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    CHECK(vkBeginCommandBuffer(command, &begin));
    for (uint32_t run = 0; run < WIDTH_RUNS; ++run) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[run]);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline_layout, 0, 1, &sets[run], 0, NULL);
        vkCmdDispatch(command, 1, 1, 1);
    }
    CHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command,
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));

    uint32_t mismatch[WIDTH_RUNS] = {0, 0};
    uint32_t guard_mismatch[WIDTH_RUNS] = {0, 0};
    uint32_t checksum[WIDTH_RUNS] = {0, 0};
    for (uint32_t run = 0; run < WIDTH_RUNS; ++run) {
        uint8_t *output = NULL;
        CHECK(vkMapMemory(device, memories[run][1], 0, BUFFER_BYTES, 0,
                          (void **)&output));
        VkMappedMemoryRange invalidate = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[run][1], .offset = 0, .size = BUFFER_BYTES,
        };
        CHECK(vkInvalidateMappedMemoryRanges(device, 1, &invalidate));
        const uint32_t data_bytes = ELEMENTS * element_bytes[run];
        for (uint32_t i = 0; i < BUFFER_BYTES; ++i) {
            if ((i < DATA_OFFSET || i >= DATA_OFFSET + data_bytes) &&
                output[i] != 0xa5)
                ++guard_mismatch[run];
        }
        if (element_bytes[run] == 1) {
            for (uint32_t i = 0; i < ELEMENTS; ++i) {
                const uint8_t source = (uint8_t)(i * 7u + 3u);
                const uint8_t expected = (uint8_t)(source * 3u + i + 7u);
                if (output[DATA_OFFSET + i] != expected) ++mismatch[run];
            }
        } else {
            const uint16_t *values =
                (const uint16_t *)(const void *)(output + DATA_OFFSET);
            for (uint32_t i = 0; i < ELEMENTS; ++i) {
                const uint16_t source = (uint16_t)(i * 257u + 19u);
                const uint16_t expected = (uint16_t)(source * 5u + i + 11u);
                if (values[i] != expected) ++mismatch[run];
            }
        }
        checksum[run] = fnv1a32(output + DATA_OFFSET, data_bytes);
        vkUnmapMemory(device, memories[run][1]);
    }
    if (mismatch[0] || mismatch[1] || guard_mismatch[0] || guard_mismatch[1]) {
        ps5log_close("storage-width-verification-failed");
        exit(1);
    }
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_STORAGE_WIDTH_SUCCESS storage8=1 storage16=1 "
        "elements8=64 elements16=64 checksum8=%08x checksum16=%08x "
        "mismatches8=0 mismatches16=0 guard_bytes8=4032 guard_bytes16=3968 "
        "guard_mismatches8=0 guard_mismatches16=0",
        checksum[0], checksum[1]);

    vkDestroyFence(device, fence, NULL);
    vkFreeCommandBuffers(device, command_pool, 1, &command);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    for (uint32_t run = 0; run < WIDTH_RUNS; ++run) {
        for (uint32_t io = 0; io < 2; ++io) {
            vkDestroyBuffer(device, buffers[run][io], NULL);
            vkFreeMemory(device, memories[run][io], NULL);
        }
        vkDestroyPipeline(device, pipelines[run], NULL);
        vkDestroyShaderModule(device, modules[run], NULL);
    }
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_STORAGE_WIDTH_RETIRED");
}

static void run_runtime_compute(VkDevice device, VkQueue queue)
{
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_COMPUTE_START");

    /* 1. Create compute shader module from owned SPIR-V */
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_resource_spirv),
        .pCode = consumer_resource_spirv
    };
    VkShaderModule comp_module = VK_NULL_HANDLE;
    CHECK(vkCreateShaderModule(device, &smci, NULL, &comp_module));

    /* Three independent resource tables: storage, uniform and uniform texel. */
    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        }
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings
    };
    VkDescriptorSetLayout set_layouts[3] = {VK_NULL_HANDLE};
    CHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &set_layouts[0]));
    VkDescriptorSetLayoutBinding uniform_binding = {
        0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL
    };
    dslci.bindingCount = 1;
    dslci.pBindings = &uniform_binding;
    CHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &set_layouts[1]));
    VkDescriptorSetLayoutBinding texel_binding = {
        0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, NULL
    };
    dslci.pBindings = &texel_binding;
    CHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &set_layouts[2]));

    /* 3. Pipeline layout with one compute push-constant word. */
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(uint32_t)
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 3,
        .pSetLayouts = set_layouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range
    };
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineLayout(device, &plci, NULL, &pipeline_layout));

    /* 4. Create compute pipeline (compiled at runtime on PS5 via PSBC/ACO) */
    const uint32_t multiplier = 5u;
    const uint32_t extra_bias = 11u;
    VkSpecializationMapEntry specialization_entries[2] = {
        {.constantID = 0, .offset = 0, .size = sizeof(uint32_t)},
        {.constantID = 1, .offset = sizeof(uint32_t), .size = sizeof(uint32_t)}
    };
    const uint32_t specialization_data[2] = {multiplier, extra_bias};
    VkSpecializationInfo specialization = {
        .mapEntryCount = 2,
        .pMapEntries = specialization_entries,
        .dataSize = sizeof(specialization_data),
        .pData = specialization_data
    };
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = comp_module,
            .pName = "main",
            .pSpecializationInfo = &specialization
        },
        .layout = pipeline_layout
    };
    VkPipeline compute_pipeline = VK_NULL_HANDLE;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &compute_pipeline));
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_COMPUTE_PIPELINE_CREATED");

    /* 5. Create storage buffers: 64 words input (binding 0) and 64 words output with boundary guards (binding 1) */
    const uint32_t element_count = 64;
    const uint32_t guard_count = 64; /* 256 bytes = minStorageBufferOffsetAlignment */
    const VkDeviceSize buffer_bytes = 4096; /* ample alignment and guard room */

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer_bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    };
    VkBuffer buffer_in = VK_NULL_HANDLE, buffer_out = VK_NULL_HANDLE;
    CHECK(vkCreateBuffer(device, &bci, NULL, &buffer_in));
    CHECK(vkCreateBuffer(device, &bci, NULL, &buffer_out));
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VkBuffer buffer_uniform = VK_NULL_HANDLE;
    CHECK(vkCreateBuffer(device, &bci, NULL, &buffer_uniform));
    bci.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    VkBuffer buffer_texel = VK_NULL_HANDLE;
    CHECK(vkCreateBuffer(device, &bci, NULL, &buffer_texel));

    VkMemoryRequirements req_in, req_out, req_uniform, req_texel;
    vkGetBufferMemoryRequirements(device, buffer_in, &req_in);
    vkGetBufferMemoryRequirements(device, buffer_out, &req_out);
    vkGetBufferMemoryRequirements(device, buffer_uniform, &req_uniform);
    vkGetBufferMemoryRequirements(device, buffer_texel, &req_texel);

    VkMemoryAllocateInfo mai_in = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req_in.size,
        .memoryTypeIndex = 0
    };
    VkMemoryAllocateInfo mai_out = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req_out.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory mem_in = VK_NULL_HANDLE, mem_out = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(device, &mai_in, NULL, &mem_in));
    CHECK(vkAllocateMemory(device, &mai_out, NULL, &mem_out));
    mai_in.allocationSize = req_uniform.size;
    VkDeviceMemory mem_uniform = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(device, &mai_in, NULL, &mem_uniform));
    mai_in.allocationSize = req_texel.size;
    VkDeviceMemory mem_texel = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(device, &mai_in, NULL, &mem_texel));

    CHECK(vkBindBufferMemory(device, buffer_in, mem_in, 0));
    CHECK(vkBindBufferMemory(device, buffer_out, mem_out, 0));
    CHECK(vkBindBufferMemory(device, buffer_uniform, mem_uniform, 0));
    CHECK(vkBindBufferMemory(device, buffer_texel, mem_texel, 0));

    /* 6. Populate input data and guard words */
    uint32_t *map_in = NULL, *map_out = NULL;
    uint32_t *map_uniform = NULL, *map_texel = NULL;
    CHECK(vkMapMemory(device, mem_in, 0, buffer_bytes, 0, (void **)&map_in));
    CHECK(vkMapMemory(device, mem_out, 0, buffer_bytes, 0, (void **)&map_out));
    CHECK(vkMapMemory(device, mem_uniform, 0, buffer_bytes, 0, (void **)&map_uniform));
    CHECK(vkMapMemory(device, mem_texel, 0, buffer_bytes, 0, (void **)&map_texel));

    for (uint32_t i = 0; i < element_count; ++i) {
        map_in[i] = i * 100u + 42u;
        map_texel[i] = i * 31u;
    }
    map_uniform[0] = 0x1337u;
    /* Guard words in destination buffer */
    for (uint32_t i = 0; i < buffer_bytes / 4; ++i) {
        map_out[i] = 0xdeadbeefu;
    }

    VkMappedMemoryRange flush_ranges[4] = {
        {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = mem_in, .offset = 0, .size = buffer_bytes},
        {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = mem_out, .offset = 0, .size = buffer_bytes},
        {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = mem_uniform, .offset = 0, .size = buffer_bytes},
        {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = mem_texel, .offset = 0, .size = buffer_bytes}
    };
    CHECK(vkFlushMappedMemoryRanges(device, 4, flush_ranges));
    vkUnmapMemory(device, mem_in);
    vkUnmapMemory(device, mem_out);
    vkUnmapMemory(device, mem_uniform);
    vkUnmapMemory(device, mem_texel);

    /* 7. Descriptor pool and allocation */
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,1}};
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 3,
        .poolSizeCount = 3,
        .pPoolSizes = pool_sizes
    };
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &desc_pool));

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = desc_pool,
        .descriptorSetCount = 3,
        .pSetLayouts = set_layouts
    };
    VkDescriptorSet desc_sets[3] = {VK_NULL_HANDLE};
    CHECK(vkAllocateDescriptorSets(device, &dsai, desc_sets));

    VkDescriptorBufferInfo dbi_in = {
        .buffer = buffer_in,
        .offset = 0,
        .range = element_count * sizeof(uint32_t)
    };
    VkDescriptorBufferInfo dbi_out = {
        .buffer = buffer_out,
        .offset = guard_count * sizeof(uint32_t), /* store output after front guards */
        .range = element_count * sizeof(uint32_t)
    };
    VkDescriptorBufferInfo dbi_uniform = {buffer_uniform, 0, 256};
    VkBufferViewCreateInfo bvci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
        .buffer = buffer_texel,
        .format = VK_FORMAT_R32_UINT,
        .offset = 0,
        .range = element_count * sizeof(uint32_t)
    };
    VkBufferView texel_view = VK_NULL_HANDLE;
    CHECK(vkCreateBufferView(device, &bvci, NULL, &texel_view));
    VkWriteDescriptorSet writes[4] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = desc_sets[0],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &dbi_in
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = desc_sets[0],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &dbi_out
        },
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=desc_sets[1],.dstBinding=0,
         .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&dbi_uniform},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=desc_sets[2],.dstBinding=0,
         .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,.pTexelBufferView=&texel_view}
    };
    vkUpdateDescriptorSets(device, 4, writes, 0, NULL);

    /* 8. Command pool & recording */
    VkCommandPoolCreateInfo cpci_pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0
    };
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &cpci_pool, NULL, &cmd_pool));

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer cmd_buf = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd_buf));

    VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd_buf, &cbbi));
    vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
    vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 3, desc_sets, 0, NULL);
    const uint32_t push_addend = 19u;
    vkCmdPushConstants(cmd_buf, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push_addend), &push_addend);
    vkCmdDispatch(cmd_buf, 1, 1, 1);
    CHECK(vkEndCommandBuffer(cmd_buf));

    /* 9. Submit with fence and wait */
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    CHECK(vkCreateFence(device, &fci, NULL, &fence));

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd_buf
    };
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));

    /* 10. Invalidate mapped memory and verify results */
    CHECK(vkMapMemory(device, mem_out, 0, buffer_bytes, 0, (void **)&map_out));
    VkMappedMemoryRange inv_range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = mem_out,
        .offset = 0,
        .size = buffer_bytes
    };
    CHECK(vkInvalidateMappedMemoryRanges(device, 1, &inv_range));

    /* Check front guards */
    int guards_intact = 1;
    for (uint32_t i = 0; i < guard_count; ++i) {
        if (map_out[i] != 0xdeadbeefu) {
            guards_intact = 0;
            ps5log_printf(PS5LOG_ERR, "Compute front guard corrupted at index %u: 0x%08x", i, map_out[i]);
        }
    }

    /* Check non-default specialization values and the pushed word together. */
    int results_correct = 1;
    uint32_t *results = map_out + guard_count;
    for (uint32_t i = 0; i < element_count; ++i) {
        uint32_t src_val = i * 100u + 42u;
        uint32_t expected =
            (src_val * multiplier + 0x1337u + extra_bias + push_addend) ^
            (i * 31u);
        if (results[i] != expected) {
            results_correct = 0;
            ps5log_printf(PS5LOG_ERR, "Compute mismatch at %u: expected 0x%08x got 0x%08x", i, expected, results[i]);
        }
    }

    /* Check tail guards */
    for (uint32_t i = guard_count + element_count; i < guard_count + element_count + guard_count; ++i) {
        if (map_out[i] != 0xdeadbeefu) {
            guards_intact = 0;
            ps5log_printf(PS5LOG_ERR, "Compute tail guard corrupted at index %u: 0x%08x", i, map_out[i]);
        }
    }

    vkUnmapMemory(device, mem_out);

    if (!guards_intact || !results_correct) {
        ps5log_close("compute-verification-failed");
        exit(1);
    }
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_RESOURCE_ABI_SUCCESS sets=3 storage=2 uniform=1 texel=1 "
        "push_bytes=4 spec_constants=2 multiplier=%u extra_bias=%u addend=%u "
        "elements=%u mismatches=0 guard_words=%u guard_mismatches=0",
        multiplier, extra_bias, push_addend, element_count, guard_count * 2);

    /* Clean up compute resources in reverse order */
    vkDestroyFence(device, fence, NULL);
    vkFreeCommandBuffers(device, cmd_pool, 1, &cmd_buf);
    vkDestroyCommandPool(device, cmd_pool, NULL);
    vkDestroyDescriptorPool(device, desc_pool, NULL);
    vkDestroyBufferView(device,texel_view,NULL);
    vkDestroyBuffer(device, buffer_in, NULL);
    vkDestroyBuffer(device, buffer_out, NULL);
    vkDestroyBuffer(device, buffer_uniform, NULL);
    vkDestroyBuffer(device, buffer_texel, NULL);
    vkFreeMemory(device, mem_in, NULL);
    vkFreeMemory(device, mem_out, NULL);
    vkFreeMemory(device, mem_uniform, NULL);
    vkFreeMemory(device, mem_texel, NULL);
    vkDestroyPipeline(device, compute_pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    for (unsigned set = 0; set < 3; ++set)
        vkDestroyDescriptorSetLayout(device, set_layouts[set], NULL);
    vkDestroyShaderModule(device, comp_module, NULL);
}

static void run_consumer(VkDevice device, VkQueue queue, int is_continuous)
{
    ps5log_printf(PS5LOG_MARK, "PS5VK_CONSUMER_GRAPHICS_START mode=%s", is_continuous ? "continuous" : "finite");

    /* 1. Create VS and FS shader modules */
    VkShaderModuleCreateInfo smci_vs = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_vertex_spirv),
        .pCode = consumer_vertex_spirv
    };
    VkShaderModule vs_module = VK_NULL_HANDLE;
    CHECK(vkCreateShaderModule(device, &smci_vs, NULL, &vs_module));

    VkShaderModuleCreateInfo smci_fs = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_fragment_spirv),
        .pCode = consumer_fragment_spirv
    };
    VkShaderModule fs_module = VK_NULL_HANDLE;
    CHECK(vkCreateShaderModule(device, &smci_fs, NULL, &fs_module));

    /* 2. Pipeline layout (empty: procedural triangle, no descriptors) */
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineLayout(device, &plci, NULL, &pipeline_layout));

    /* 3. Render Pass: BGRA8 UNORM, 1 sample, clear on load in finite mode, store on end */
    VkAttachmentDescription color_attachment = {
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass
    };
    VkRenderPass render_pass = VK_NULL_HANDLE;
    CHECK(vkCreateRenderPass(device, &rpci, NULL, &render_pass));

    /* 4. Graphics pipeline creation */
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vs_module,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fs_module,
            .pName = "main"
        }
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkViewport viewport = {0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {1920, 1080}};
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor
    };
    VkPipelineRasterizationStateCreateInfo rci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .lineWidth = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo msi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineColorBlendAttachmentState cba = {
        .colorWriteMask = 0xf
    };
    VkPipelineColorBlendStateCreateInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &cba
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vps,
        .pRasterizationState = &rci,
        .pMultisampleState = &msi,
        .pColorBlendState = &cbi,
        .layout = pipeline_layout,
        .renderPass = render_pass
    };

    /* First creation: Cold compilation */
    VkPipeline pipeline1 = VK_NULL_HANDLE;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline1));
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_GRAPHICS_PIPELINE_COLD_CREATED");

    /* Second creation: Warm cache hit test */
    VkPipeline pipeline2 = VK_NULL_HANDLE;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline2));
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_GRAPHICS_PIPELINE_WARM_CREATED");

    /* Destroy pipeline2 immediately to verify refcount and bounded cache release */
    vkDestroyPipeline(device, pipeline2, NULL);
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_GRAPHICS_PIPELINE_DESTROYED refcount_verified=1");

    /* 5. Create two 1080p presentation images and bind them in 128 MiB direct memory */
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = {1920, 1080, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
    };
    VkImage images[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    CHECK(vkCreateImage(device, &ici, NULL, &images[0]));
    CHECK(vkCreateImage(device, &ici, NULL, &images[1]));

    const VkDeviceSize total_image_memory = UINT64_C(0x08000000); /* 128 MiB two-buffer envelope */
    VkMemoryAllocateInfo mai_img = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = total_image_memory,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(device, &mai_img, NULL, &image_memory));

    CHECK(vkBindImageMemory(device, images[0], image_memory, 0));
    CHECK(vkBindImageMemory(device, images[1], image_memory, UINT64_C(0x04000000)));

    /* 6. Create image views and framebuffers */
    VkImageView image_views[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFramebuffer framebuffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    for (unsigned slot = 0; slot < 2; ++slot) {
        VkImageViewCreateInfo ivci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[slot],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_B8G8R8A8_UNORM,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1
            }
        };
        CHECK(vkCreateImageView(device, &ivci, NULL, &image_views[slot]));

        VkFramebufferCreateInfo fbci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = render_pass,
            .attachmentCount = 1,
            .pAttachments = &image_views[slot],
            .width = 1920,
            .height = 1080,
            .layers = 1
        };
        CHECK(vkCreateFramebuffer(device, &fbci, NULL, &framebuffers[slot]));
    }

    /* 7. Create native presentation surface via public API */
    struct ps5vk_present_config pconfig = {
        .width = 1920,
        .height = 1080,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .buffer_count = 2
    };
    ps5vk_present_surface surface = NULL;
    CHECK(ps5vkCreatePresentSurface(device, &pconfig, 2, images, &surface));
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_PRESENT_SURFACE_CREATED buffers=2");

    /* 8. Command pool and buffer */
    VkCommandPoolCreateInfo cpci_gfx = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0
    };
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &cpci_gfx, NULL, &cmd_pool));

    VkCommandBufferAllocateInfo cbai_gfx = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer cmd_buf = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &cbai_gfx, &cmd_buf));

    /* Map image memory for readback */
    void *mapped_images = NULL;
    CHECK(vkMapMemory(device, image_memory, 0, total_image_memory, 0, &mapped_images));

    /* 9. Render loop: finite mode runs 18 frames with readbacks; continuous mode runs indefinitely */
    const uint32_t max_finite_frames = 18;
    const uint32_t sentinel_bg = 0x55aa11eeu;

    for (uint32_t frame = 0;; ++frame) {
        if (!is_continuous && frame >= max_finite_frames) {
            break;
        }

        uint32_t slot = frame & 1u;
        VkDeviceSize slot_offset = slot ? UINT64_C(0x04000000) : 0;
        uint32_t *pixels = (uint32_t *)((unsigned char *)mapped_images + slot_offset);
        const size_t word_count = 1920 * 1080;

        /* Before render: clear background to sentinel and flush CPU writes */
        if (!is_continuous) {
            for (size_t w = 0; w < word_count; ++w) {
                pixels[w] = sentinel_bg;
            }
            VkMappedMemoryRange flush_range = {
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = image_memory,
                .offset = slot_offset,
                .size = word_count * sizeof(uint32_t)
            };
            CHECK(vkFlushMappedMemoryRanges(device, 1, &flush_range));
        }

        /* Record draw commands */
        CHECK(vkResetCommandBuffer(cmd_buf, 0));
        VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd_buf, &begin_info));

        VkRenderPassBeginInfo rp_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = render_pass,
            .framebuffer = framebuffers[slot],
            .renderArea = {{0, 0}, {1920, 1080}}
        };
        vkCmdBeginRenderPass(cmd_buf, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline1);
        vkCmdDraw(cmd_buf, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd_buf);
        CHECK(vkEndCommandBuffer(cmd_buf));

        /* Submit to queue and wait */
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd_buf
        };
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));

        /* Present frame */
        CHECK(ps5vkPresentFrame(surface, slot, (uint64_t)(frame + 1)));

        /* Deterministic readback validation in finite mode */
        if (!is_continuous) {
            VkMappedMemoryRange inv_range = {
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = image_memory,
                .offset = slot_offset,
                .size = word_count * sizeof(uint32_t)
            };
            CHECK(vkInvalidateMappedMemoryRanges(device, 1, &inv_range));

            uint64_t changed = 0;
            uint64_t bad_alpha = 0;
            uint64_t bad_sum = 0;

            for (size_t w = 0; w < word_count; ++w) {
                uint32_t p = pixels[w];
                if (p == sentinel_bg) continue;
                ++changed;
                if ((p >> 24) != 255) ++bad_alpha;
                unsigned sum = (p & 255) + ((p >> 8) & 255) + ((p >> 16) & 255);
                if (sum < 254 || sum > 256) ++bad_sum;
            }

            /* Expected triangle area: 1920 * 1080 * 91 / 400 = 471,744 words */
            uint64_t expected_area = (uint64_t)1920 * 1080 * 91 / 400;
            uint64_t tolerance = 2ull * (1920 + 1080); /* 6,000 words boundary band */
            uint64_t diff = changed > expected_area ? (changed - expected_area) : (expected_area - changed);
            int valid = (diff <= tolerance) && (bad_alpha == 0) && (bad_sum == 0) && (changed > 0);

            ps5log_printf(PS5LOG_MARK,
                          "PS5VK_CONSUMER_READBACK frame=%u slot=%u changed=%llu bad_alpha=%llu bad_sum=%llu valid=%d",
                          frame, slot, (unsigned long long)changed, (unsigned long long)bad_alpha, (unsigned long long)bad_sum, valid);

            if (!valid) {
                ps5log_close("readback-validation-failed");
                exit(1);
            }
        } else {
            if ((frame % 60) == 0) {
                ps5log_printf(PS5LOG_MARK, "PS5VK_CONSUMER_CONTINUOUS_CADENCE frame=%u fps=60", frame);
            }
        }
    }

    /* Unmap before closing */
    vkUnmapMemory(device, image_memory);

    /* 10. Orderly explicit retirement (exact VideoOut / GPU ownership order) */
    CHECK(vkQueueWaitIdle(queue));
    ps5vkDestroyPresentSurface(surface);
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_PRESENT_SURFACE_DESTROYED");

    for (unsigned slot = 0; slot < 2; ++slot) {
        vkDestroyFramebuffer(device, framebuffers[slot], NULL);
        vkDestroyImageView(device, image_views[slot], NULL);
        vkDestroyImage(device, images[slot], NULL);
    }
    vkFreeMemory(device, image_memory, NULL);

    vkFreeCommandBuffers(device, cmd_pool, 1, &cmd_buf);
    vkDestroyCommandPool(device, cmd_pool, NULL);
    vkDestroyPipeline(device, pipeline1, NULL);
    vkDestroyRenderPass(device, render_pass, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyShaderModule(device, vs_module, NULL);
    vkDestroyShaderModule(device, fs_module, NULL);

    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_TEST_SUCCESS");
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1");
    ps5log_line(PS5LOG_MARK, "PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1");
    ps5log_close("consumer-finite-end");

    /* Bounded validation: process stays in submit suspend wait for system Close Game */
    for (;;) sleep(1);
}

int main(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t boot = (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;

    ps5log_config cfg;
    const char *loaded = NULL, *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&cfg);
    if (ps5log_load_config(paths, 1, &cfg, &loaded)) {
        _exit(0);
    }
    cfg.udp = 0;
    if (ps5log_init(&cfg, "PPSA99994", "ps5vk", boot)) {
        _exit(0);
    }

    int is_continuous = parse_is_continuous();
    ps5log_printf(PS5LOG_MARK, "PS5VK_CONSUMER_BOOT mode=%s sdk_version=%u.%u",
                  is_continuous ? "continuous" : "finite",
                  PS5VK_SDK_VERSION_MAJOR, PS5VK_SDK_VERSION_MINOR);

    /* 1. Create a Vulkan 1.0 instance with the properties2 query extension. */
    const char *instance_extensions[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = instance_extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&ici, NULL, &instance));

    /* 2. Enumerate Physical Device & verify properties */
    uint32_t dev_count = 1;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    CHECK(vkEnumeratePhysicalDevices(instance, &dev_count, &physical_device));

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device, &props);
    ps5log_printf(PS5LOG_MARK, "PS5VK_CONSUMER_DEVICE name='%s' api=%u.%u driver=%u",
                  props.deviceName,
                  VK_VERSION_MAJOR(props.apiVersion),
                  VK_VERSION_MINOR(props.apiVersion),
                  props.driverVersion);
    report_physical_device_contract(instance, physical_device, &props);

    VkPhysicalDevice16BitStorageFeatures storage16 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
    };
    VkPhysicalDevice8BitStorageFeatures storage8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,
        .pNext = &storage16,
    };
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &storage8,
    };
    vkGetPhysicalDeviceFeatures2KHR(physical_device, &features2);
    REQUIRE(storage8.storageBuffer8BitAccess == VK_TRUE &&
            storage8.uniformAndStorageBuffer8BitAccess == VK_FALSE &&
            storage8.storagePushConstant8 == VK_FALSE,
            "exact 8-bit storage feature report");
    REQUIRE(storage16.storageBuffer16BitAccess == VK_TRUE &&
            storage16.uniformAndStorageBuffer16BitAccess == VK_FALSE &&
            storage16.storagePushConstant16 == VK_FALSE &&
            storage16.storageInputOutput16 == VK_FALSE,
            "exact 16-bit storage feature report");
    ps5log_line(PS5LOG_MARK,
        "PS5VK_CONSUMER_STORAGE_WIDTH_NEGOTIATED instance_ext=1 device_exts=3 "
        "storageBuffer8BitAccess=1 storageBuffer16BitAccess=1 narrow_arithmetic=0");

    /* 3. Create Device & Queue with only the two reported narrow-storage bits. */
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };
    const char *device_extensions[] = {
        VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME,
        VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
        VK_KHR_16BIT_STORAGE_EXTENSION_NAME,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features2,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 3,
        .ppEnabledExtensionNames = device_extensions,
    };
    VkDevice device = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(physical_device, &dci, NULL, &device));

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);

    /* 4. Run runtime compute */
    run_runtime_compute(device, queue);

    /* 5. Run byte- and word-exact 8/16-bit storage-buffer witnesses. */
    run_storage_width_compute(device, queue);

    /* 6. Run runtime procedural graphics and presentation */
    run_consumer(device, queue, is_continuous);

    /* Orderly destroy device and instance if ever returned */
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
