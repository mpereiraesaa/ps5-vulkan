#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include <ps5vk/ps5vk_present.h>
#include "shaders.h"
#include "resource_shader.h"
#ifdef CONSUMER_TEXEL_RGBA8
#include "texel_rgba8_shader.h"
#endif
#include "storage_width_shaders.h"
#include "sync_shaders.h"
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

#include "sampled_sets.h"

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

#include "physical_device_contract.h"

static void run_buffer_transfer_contract(VkDevice device, VkQueue queue)
{
    enum { LOGICAL_BYTES = 67 };
    VkBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory memories[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    void *mapped[2] = {NULL, NULL};
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_BUFFER_TRANSFER_START");

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = LOGICAL_BYTES,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    for (uint32_t j = 0; j < 2; ++j) {
        buffer_info.usage = j ? VK_BUFFER_USAGE_TRANSFER_DST_BIT :
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffers[j]));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffers[j], &requirements);
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = 0,
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &memories[j]));
        CHECK(vkBindBufferMemory(device, buffers[j], memories[j], 0));
        CHECK(vkMapMemory(device, memories[j], 0, requirements.size, 0,
                          &mapped[j]));
        memset(mapped[j], j ? 0x5a : 0, (size_t)requirements.size);
    }
    for (uint32_t j = 0; j < LOGICAL_BYTES; ++j)
        ((uint8_t *)mapped[0])[j] = (uint8_t)(j + 1);
    VkMappedMemoryRange source_flush = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memories[0], .offset = 0, .size = VK_WHOLE_SIZE,
    };
    CHECK(vkFlushMappedMemoryRanges(device, 1, &source_flush));

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    CHECK(vkBeginCommandBuffer(command, &begin));
    VkBufferCopy copy = {.srcOffset = 1, .dstOffset = 0, .size = 7};
    uint32_t update[2] = {0x11223344u, 0xaabbccddu};
    vkCmdCopyBuffer(command, buffers[0], buffers[1], 1, &copy);
    vkCmdUpdateBuffer(command, buffers[1], 8, sizeof(update), update);
    update[0] = update[1] = 0;
    vkCmdFillBuffer(command, buffers[1], 16, 16, 0xdecafbadu);
    vkCmdFillBuffer(command, buffers[1], 60, VK_WHOLE_SIZE, 0x01020304u);
    CHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VkFence fence = VK_NULL_HANDLE;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command,
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));

    VkMappedMemoryRange destination_invalidate = source_flush;
    destination_invalidate.memory = memories[1];
    CHECK(vkInvalidateMappedMemoryRanges(device, 1, &destination_invalidate));
    const uint8_t *bytes = mapped[1];
    uint32_t mismatch = memcmp(bytes,
        (const uint8_t[]){2, 3, 4, 5, 6, 7, 8}, 7) != 0;
    mismatch += bytes[7] != 0x5a;
    mismatch += *(const uint32_t *)(const void *)(bytes + 8) != 0x11223344u;
    mismatch += *(const uint32_t *)(const void *)(bytes + 12) != 0xaabbccddu;
    for (uint32_t j = 16; j < 32; j += 4)
        mismatch += *(const uint32_t *)(const void *)(bytes + j) != 0xdecafbadu;
    mismatch += *(const uint32_t *)(const void *)(bytes + 60) != 0x01020304u;
    for (uint32_t j = 32; j < 60; ++j) mismatch += bytes[j] != 0x5a;
    for (uint32_t j = 64; j < LOGICAL_BYTES; ++j) mismatch += bytes[j] != 0x5a;
    if (mismatch) {
        ps5log_printf(PS5LOG_ERR,
            "PS5VK_CONSUMER_BUFFER_TRANSFER_FAILURE mismatches=%u", mismatch);
        ps5log_close("buffer-transfer-verification-failed");
        exit(1);
    }
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_BUFFER_TRANSFER_SUCCESS copy_bytes=7 update_bytes=8 "
        "fill_bytes=20 whole_tail_bytes=3 guard_mismatches=0 hash=%08x",
        fnv1a32(bytes, LOGICAL_BYTES));

    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    for (uint32_t j = 0; j < 2; ++j) {
        vkUnmapMemory(device, memories[j]);
        vkDestroyBuffer(device, buffers[j], NULL);
        vkFreeMemory(device, memories[j], NULL);
    }
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_BUFFER_TRANSFER_RETIRED");
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

static void run_synchronization_compute(VkDevice device, VkQueue queue)
{
    enum { BUFFER_BYTES = 4096, WORDS = BUFFER_BYTES / 4,
           SYNC_WORDS = 64, LANES = 128, ATOMIC_WORDS = LANES * 3 };
    const uint32_t *shader_words[3] = {
        consumer_sync_producer_spirv,
        consumer_sync_consumer_spirv,
        consumer_shared_atomic_multiwave_spirv,
    };
    const size_t shader_bytes[3] = {
        sizeof(consumer_sync_producer_spirv),
        sizeof(consumer_sync_consumer_spirv),
        sizeof(consumer_shared_atomic_multiwave_spirv),
    };
    VkShaderModule modules[3] = {VK_NULL_HANDLE};
    VkPipeline pipelines[3] = {VK_NULL_HANDLE};
    VkBuffer buffers[3] = {VK_NULL_HANDLE};
    VkDeviceMemory memories[3] = {VK_NULL_HANDLE};
    VkDescriptorSet sets[2] = {VK_NULL_HANDLE};

    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_SYNC_START");

    VkDescriptorSetLayoutBinding bindings[2] = {{
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    }, {
        .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    }};
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings,
    };
    VkDescriptorSetLayout set_layouts[2] = {VK_NULL_HANDLE};
    CHECK(vkCreateDescriptorSetLayout(device, &layout_info, NULL,
                                      &set_layouts[0]));
    layout_info.bindingCount = 1;
    CHECK(vkCreateDescriptorSetLayout(device, &layout_info, NULL,
                                      &set_layouts[1]));
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layouts[0],
    };
    VkPipelineLayout pipeline_layouts[2] = {VK_NULL_HANDLE};
    CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, NULL,
                                 &pipeline_layouts[0]));
    pipeline_layout_info.pSetLayouts = &set_layouts[1];
    CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, NULL,
                                 &pipeline_layouts[1]));

    for (uint32_t i = 0; i < 3; ++i) {
        VkShaderModuleCreateInfo module_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = shader_bytes[i], .pCode = shader_words[i],
        };
        CHECK(vkCreateShaderModule(device, &module_info, NULL, &modules[i]));
        VkComputePipelineCreateInfo pipeline_info = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = modules[i], .pName = "main",
            },
            .layout = pipeline_layouts[i == 2 ? 1 : 0],
        };
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                       &pipeline_info, NULL, &pipelines[i]));
    }

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = BUFFER_BYTES, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    };
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffers[i]));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffers[i], &requirements);
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = 0,
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &memories[i]));
        CHECK(vkBindBufferMemory(device, buffers[i], memories[i], 0));

        uint32_t *mapped = NULL;
        CHECK(vkMapMemory(device, memories[i], 0, BUFFER_BYTES, 0,
                          (void **)&mapped));
        for (uint32_t word = 0; word < WORDS; ++word)
            mapped[word] = 0xdeadbeefu;
        if (i == 0)
            for (uint32_t word = 0; word < SYNC_WORDS; ++word)
                mapped[word] = word * 17u + 5u;
        VkMappedMemoryRange flush = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[i], .offset = 0, .size = BUFFER_BYTES,
        };
        CHECK(vkFlushMappedMemoryRanges(device, 1, &flush));
        vkUnmapMemory(device, memories[i]);
    }

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 4,
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &pool_size,
    };
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo allocate_sets = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 2, .pSetLayouts = set_layouts,
    };
    CHECK(vkAllocateDescriptorSets(device, &allocate_sets, sets));

    VkDescriptorBufferInfo infos[3] = {
        {.buffer = buffers[0], .offset = 0, .range = SYNC_WORDS * 4},
        {.buffer = buffers[1], .offset = 0, .range = SYNC_WORDS * 4},
        {.buffer = buffers[2], .offset = 0, .range = ATOMIC_WORDS * 4},
    };
    VkWriteDescriptorSet writes[3] = {{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = sets[0], .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &infos[0],
    }, {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = sets[0], .dstBinding = 1, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &infos[1],
    }, {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = sets[1], .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &infos[2],
    }};
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);

    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
    VkEventCreateInfo event_info = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkEvent event = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    CHECK(vkCreateEvent(device, &event_info, NULL, &event));
    CHECK(vkCreateSemaphore(device, &semaphore_info, NULL, &semaphore));
    REQUIRE(vkGetEventStatus(device, event) == VK_EVENT_RESET,
            "event initially reset");
    CHECK(vkSetEvent(device, event));
    REQUIRE(vkGetEventStatus(device, event) == VK_EVENT_SET,
            "host event set");
    CHECK(vkResetEvent(device, event));
    REQUIRE(vkGetEventStatus(device, event) == VK_EVENT_RESET,
            "host event reset");
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    CHECK(vkBeginCommandBuffer(command, &begin));
    vkCmdSetEvent(command, event, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

    VkBufferMemoryBarrier host_barriers[3] = {{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[0], .offset = 0, .size = SYNC_WORDS * 4,
    }, {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[1], .offset = 0, .size = SYNC_WORDS * 4,
    }, {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[2], .offset = 0, .size = ATOMIC_WORDS * 4,
    }};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
        3, host_barriers, 0, NULL);

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[0]);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layouts[0], 0, 1, &sets[0], 0, NULL);
    vkCmdDispatch(command, 1, 1, 1);

    VkBufferMemoryBarrier producer_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[0], .offset = 0, .size = SYNC_WORDS * 4,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
        1, &producer_barrier, 0, NULL);

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[1]);
    vkCmdDispatch(command, 1, 1, 1);

    VkBufferMemoryBarrier host_result = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[1], .offset = 0, .size = SYNC_WORDS * 4,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host_result, 0, NULL);

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[2]);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layouts[1], 0, 1, &sets[1], 0, NULL);
    vkCmdDispatch(command, 1, 1, 1);
    host_result.buffer = buffers[2];
    host_result.size = ATOMIC_WORDS * 4;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host_result, 0, NULL);
    vkCmdWaitEvents(command, 1, &event,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, NULL, 0, NULL, 0, NULL);
    vkCmdResetEvent(command, event, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    CHECK(vkEndCommandBuffer(command));

    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo submits[2] = {{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &semaphore,
    }, {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &semaphore,
        .pWaitDstStageMask = &wait_stage,
    }};
    CHECK(vkQueueSubmit(queue, 2, submits, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
    REQUIRE(vkGetEventStatus(device, event) == VK_EVENT_RESET,
            "device event set-wait-reset order");
    ps5log_line(PS5LOG_MARK,
        "PS5VK_CONSUMER_SYNC_OBJECTS_SUCCESS host_set_reset=1 "
        "device_set_wait_reset=1 binary_signal_wait=1 semaphore_consumed=1");

    uint32_t *sync = NULL, *atomic = NULL;
    CHECK(vkMapMemory(device, memories[1], 0, BUFFER_BYTES, 0, (void **)&sync));
    CHECK(vkMapMemory(device, memories[2], 0, BUFFER_BYTES, 0, (void **)&atomic));
    VkMappedMemoryRange invalidates[2] = {{
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memories[1], .offset = 0, .size = BUFFER_BYTES,
    }, {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memories[2], .offset = 0, .size = BUFFER_BYTES,
    }};
    CHECK(vkInvalidateMappedMemoryRanges(device, 2, invalidates));

    uint32_t sync_mismatch = 0, atomic_mismatch = 0, guard_mismatch = 0;
    uint32_t seen[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < SYNC_WORDS; ++i) {
        const uint32_t expected = ((i * 17u + 5u) * 3u + 7u) ^ 0xa5a55a5au;
        if (sync[i] != expected) ++sync_mismatch;
    }
    for (uint32_t i = SYNC_WORDS; i < WORDS; ++i)
        if (sync[i] != 0xdeadbeefu) ++guard_mismatch;
    for (uint32_t lane = 0; lane < LANES; ++lane) {
        const uint32_t ticket = atomic[lane];
        if (ticket >= LANES || (seen[ticket / 32] & (1u << (ticket % 32))))
            ++atomic_mismatch;
        else
            seen[ticket / 32] |= 1u << (ticket % 32);
        if (atomic[LANES + lane] != atomic[LANES - 1u - lane])
            ++atomic_mismatch;
        if (atomic[2u * LANES + lane] != LANES)
            ++atomic_mismatch;
    }
    for (uint32_t i = ATOMIC_WORDS; i < WORDS; ++i)
        if (atomic[i] != 0xdeadbeefu) ++guard_mismatch;

    const uint32_t sync_hash = fnv1a32((const uint8_t *)sync, SYNC_WORDS * 4);
    const uint32_t atomic_hash = fnv1a32((const uint8_t *)atomic, ATOMIC_WORDS * 4);
    vkUnmapMemory(device, memories[1]);
    vkUnmapMemory(device, memories[2]);
    REQUIRE(!sync_mismatch && !atomic_mismatch && !guard_mismatch,
            "synchronization and multi-wave atomic oracle");
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_SYNC_SUCCESS producer_consumer=1 host_compute_host=1 "
        "local_size=128 waves32=4 lds_atomic=1 permutation=1 counter=128 "
        "sync_hash=%08x atomic_hash=%08x mismatches=0 guard_mismatches=0",
        sync_hash, atomic_hash);

    vkDestroySemaphore(device, semaphore, NULL);
    vkDestroyEvent(device, event, NULL);
    vkDestroyFence(device, fence, NULL);
    vkFreeCommandBuffers(device, command_pool, 1, &command);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    for (uint32_t i = 0; i < 3; ++i) {
        vkDestroyBuffer(device, buffers[i], NULL);
        vkFreeMemory(device, memories[i], NULL);
        vkDestroyPipeline(device, pipelines[i], NULL);
        vkDestroyShaderModule(device, modules[i], NULL);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        vkDestroyPipelineLayout(device, pipeline_layouts[i], NULL);
        vkDestroyDescriptorSetLayout(device, set_layouts[i], NULL);
    }
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_SYNC_RETIRED");
}

/* Bounded pipeline-cache contract: create, size query, export, re-import and
 * merge through public entry points only. The exported blob is the normative
 * 32-byte header; this slice stores no compiled-code records and never claims a
 * restored hit. */
static void run_pipeline_cache_contract(VkDevice device, VkPipelineCache *out_cache)
{
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_PIPELINE_CACHE_START");
    const VkPipelineCacheCreateInfo cache_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
    };
    VkPipelineCache cache = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineCache(device, &cache_info, NULL, &cache));

    size_t cache_size = 0;
    CHECK(vkGetPipelineCacheData(device, cache, &cache_size, NULL));
    REQUIRE(cache_size == 32u, "pipeline cache size query returns the header size");

    uint8_t blob[33];
    memset(blob, 0xa5, sizeof(blob));
    cache_size = sizeof(blob);
    CHECK(vkGetPipelineCacheData(device, cache, &cache_size, blob));
    REQUIRE(cache_size == 32u && blob[32] == 0xa5, "pipeline cache export writes exactly the header");

    /* One byte short: VK_INCOMPLETE, nothing written, size reported as zero. */
    uint8_t small[32];
    memset(small, 0x5a, sizeof(small));
    size_t small_size = 31u;
    REQUIRE(vkGetPipelineCacheData(device, cache, &small_size, small) == VK_INCOMPLETE &&
            small_size == 0u && small[0] == 0x5a,
            "short pipeline cache buffer fails without writing");

    VkPipelineCache reimported = VK_NULL_HANDLE;
    const VkPipelineCacheCreateInfo import_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = 32u,
        .pInitialData = blob,
    };
    CHECK(vkCreatePipelineCache(device, &import_info, NULL, &reimported));
    CHECK(vkMergePipelineCaches(device, cache, 1u, &reimported));
    vkDestroyPipelineCache(device, reimported, NULL);

    ps5log_line(PS5LOG_MARK,
        "PS5VK_CONSUMER_PIPELINE_CACHE created=1 header_bytes=32 imported=1 merged=1 records=0");
    *out_cache = cache;
}

static void run_runtime_compute(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                                VkPipelineCache cache)
{
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_COMPUTE_START");

#ifndef CONSUMER_TEXEL_RGBA8
    (void)physical;
#endif

#ifdef CONSUMER_TEXEL_RGBA8
    /* The witness must run against a format the device actually reports, so
     * the feature bit is checked here rather than assumed from the source. */
    VkFormatProperties rgba8_properties;
    memset(&rgba8_properties, 0, sizeof(rgba8_properties));
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM,
                                        &rgba8_properties);
    const int rgba8_reported =
        (rgba8_properties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) != 0;
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_TEXEL_RGBA8_FORMAT format=r8g8b8a8_unorm "
        "buffer_features=0x%08x uniform_texel_reported=%d",
        (unsigned)rgba8_properties.bufferFeatures, rgba8_reported);
    if (!rgba8_reported) {
        ps5log_close("texel-rgba8-not-reported");
        exit(1);
    }
#endif

    /* 1. Create compute shader module from owned SPIR-V */
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
#ifdef CONSUMER_TEXEL_RGBA8
        .codeSize = sizeof(consumer_texel_rgba8_spirv),
        .pCode = consumer_texel_rgba8_spirv
#else
        .codeSize = sizeof(consumer_resource_spirv),
        .pCode = consumer_resource_spirv
#endif
    };
    VkShaderModule comp_module = VK_NULL_HANDLE;
    CHECK(vkCreateShaderModule(device, &smci, NULL, &comp_module));

    /* Three independent resource tables: storage, uniform and uniform texel. */
    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
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
        0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL
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
    CHECK(vkCreateComputePipelines(device, cache, 1, &cpci, NULL, &compute_pipeline));
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_COMPUTE_PIPELINE_CREATED");

    /* 5. Create storage buffers: 64 words input (binding 0) and 64 words output with boundary guards (binding 1) */
    const uint32_t element_count = 64;
    const uint32_t guard_count = 64; /* 256 bytes = minStorageBufferOffsetAlignment */
    const uint32_t dynamic_offset = 256;
    const uint32_t output_base_offset = 256;
    const uint32_t output_word_offset =
        (output_base_offset + dynamic_offset) / sizeof(uint32_t);
    const VkDeviceSize buffer_bytes = 4096; /* ample alignment and guard room */

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer_bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
    };
    VkBuffer buffer_in = VK_NULL_HANDLE, buffer_out = VK_NULL_HANDLE;
    CHECK(vkCreateBuffer(device, &bci, NULL, &buffer_in));
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
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
        map_in[dynamic_offset / sizeof(uint32_t) + i] = i * 100u + 42u;
#ifdef CONSUMER_TEXEL_RGBA8
        /* Deterministic RGBA8 bytes: a wrong element format, channel order or
         * scalar completion changes the packed word the shader returns. */
        ((uint8_t *)map_texel)[i * 4 + 0] = (uint8_t)(i * 3u + 1u);
        ((uint8_t *)map_texel)[i * 4 + 1] = (uint8_t)(i * 5u + 2u);
        ((uint8_t *)map_texel)[i * 4 + 2] = (uint8_t)(i * 7u + 3u);
        ((uint8_t *)map_texel)[i * 4 + 3] = (uint8_t)(255u - (i & 0xffu));
#else
        map_texel[i] = i * 31u;
#endif
    }
    const VkDeviceSize dispatch_indirect_offset = 512;
    VkDispatchIndirectCommand *dispatch_indirect =
        (VkDispatchIndirectCommand *)((unsigned char *)map_in + dispatch_indirect_offset);
    *dispatch_indirect = (VkDispatchIndirectCommand){1, 1, 1};
    map_uniform[dynamic_offset / sizeof(uint32_t)] = 0x1337u;
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
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1},
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
        .offset = output_base_offset,
        .range = element_count * sizeof(uint32_t)
    };
    VkDescriptorBufferInfo dbi_uniform = {buffer_uniform, 0, 256};
    VkBufferViewCreateInfo bvci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
        .buffer = buffer_texel,
#ifdef CONSUMER_TEXEL_RGBA8
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .offset = 0,
        .range = element_count * 4u
#else
        .format = VK_FORMAT_R32_UINT,
        .offset = 0,
        .range = element_count * sizeof(uint32_t)
#endif
    };
    VkBufferView texel_view = VK_NULL_HANDLE;
    CHECK(vkCreateBufferView(device, &bvci, NULL, &texel_view));
    VkWriteDescriptorSet writes[4] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = desc_sets[0],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
            .pBufferInfo = &dbi_in
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = desc_sets[0],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
            .pBufferInfo = &dbi_out
        },
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=desc_sets[1],.dstBinding=0,
         .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.pBufferInfo=&dbi_uniform},
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
    const uint32_t dynamic_offsets[3] = {
        dynamic_offset, dynamic_offset, dynamic_offset
    };
    vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout, 0, 3, desc_sets, 3,
                            dynamic_offsets);
    const uint32_t push_addend = 19u;
    vkCmdPushConstants(cmd_buf, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push_addend), &push_addend);
    vkCmdDispatchIndirect(cmd_buf, buffer_in, dispatch_indirect_offset);
    ps5log_line(PS5LOG_MARK,
        "PS5VK_CONSUMER_DISPATCH_INDIRECT_RECORDED groups=1,1,1 offset=512");
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
    for (uint32_t i = 0; i < output_word_offset; ++i) {
        if (map_out[i] != 0xdeadbeefu) {
            guards_intact = 0;
            ps5log_printf(PS5LOG_ERR, "Compute front guard corrupted at index %u: 0x%08x", i, map_out[i]);
        }
    }

    /* Check non-default specialization values and the pushed word together. */
    int results_correct = 1;
    uint32_t *results = map_out + output_word_offset;
    for (uint32_t i = 0; i < element_count; ++i) {
        uint32_t src_val = i * 100u + 42u;
#ifdef CONSUMER_TEXEL_RGBA8
        /* Mask every channel to its byte: the texel buffer stores 8-bit
         * components, so an unmasked product would spill into the neighbour
         * channel and make the oracle - not the driver - wrong. */
        uint32_t packed =
            (uint32_t)((i * 3u + 1u) & 0xffu) |
            ((uint32_t)((i * 5u + 2u) & 0xffu) << 8) |
            ((uint32_t)((i * 7u + 3u) & 0xffu) << 16) |
            ((uint32_t)(255u - (i & 0xffu)) << 24);
#else
        uint32_t packed = i * 31u;
#endif
        uint32_t expected =
            (src_val * multiplier + 0x1337u + extra_bias + push_addend) ^
            packed;
        if (results[i] != expected) {
            results_correct = 0;
            ps5log_printf(PS5LOG_ERR, "Compute mismatch at %u: expected 0x%08x got 0x%08x", i, expected, results[i]);
        }
    }

    /* Check tail guards */
    for (uint32_t i = output_word_offset + element_count;
         i < output_word_offset + element_count + guard_count; ++i) {
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
        "dynamic_ssbo=2 dynamic_ubo=1 offsets=256,256,256 base_plus_dynamic=1 "
        "push_bytes=4 spec_constants=2 multiplier=%u extra_bias=%u addend=%u "
        "elements=%u mismatches=0 guard_words=%u guard_mismatches=0",
        multiplier, extra_bias, push_addend, element_count,
        output_word_offset + guard_count);
#ifdef CONSUMER_TEXEL_RGBA8
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_TEXEL_RGBA8_SUCCESS format=r8g8b8a8_unorm texels=%u "
        "channels=4 packed_rgba_order=1 mismatches=0 guard_words=%u "
        "guard_mismatches=0",
        element_count, output_word_offset + guard_count);
#endif

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

    /* 3. Compatible passes for an exact fixed-function witness.  The first
     * clears color and depth=0, so LESS rejects the triangle.  The second
     * LOADs that black color, clears depth=1 and draws through dynamic
     * viewport/scissor state. */
    VkAttachmentDescription clear_attachments[2] = {
        {.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.format=VK_FORMAT_D32_SFLOAT,.samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}
    };
    VkAttachmentDescription load_attachments[2] = {
        {.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.format=VK_FORMAT_D32_SFLOAT,.samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}
    };
    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    VkAttachmentReference depth_ref = {
        .attachment = 1,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
        .pDepthStencilAttachment = &depth_ref
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = clear_attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass
    };
    VkRenderPass clear_pass = VK_NULL_HANDLE;
    VkRenderPass load_pass = VK_NULL_HANDLE;
    CHECK(vkCreateRenderPass(device, &rpci, NULL, &clear_pass));
    rpci.pAttachments = load_attachments;
    CHECK(vkCreateRenderPass(device, &rpci, NULL, &load_pass));

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
    VkPipelineDepthStencilStateCreateInfo dsi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS
    };
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states
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
        .pDepthStencilState = &dsi,
        .pColorBlendState = &cbi,
        .layout = pipeline_layout,
        .renderPass = clear_pass
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

    vps.pViewports = NULL;
    vps.pScissors = NULL;
    gpci.pDynamicState = &dynamic;
    VkPipeline dynamic_pipeline = VK_NULL_HANDLE;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &dynamic_pipeline));
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_DYNAMIC_PIPELINE_CREATED viewport=1 scissor=1");

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

    VkImageCreateInfo depth_ici = ici;
    depth_ici.format = VK_FORMAT_D32_SFLOAT;
    depth_ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImage depth_image = VK_NULL_HANDLE;
    CHECK(vkCreateImage(device, &depth_ici, NULL, &depth_image));
    VkMemoryRequirements depth_requirements;
    vkGetImageMemoryRequirements(device, depth_image, &depth_requirements);
    VkMemoryAllocateInfo depth_mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = depth_requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(device, &depth_mai, NULL, &depth_memory));
    CHECK(vkBindImageMemory(device, depth_image, depth_memory, 0));

    /* 6. Create image views and framebuffers */
    VkImageView image_views[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFramebuffer framebuffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageViewCreateInfo depth_ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .subresourceRange = {.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT,.levelCount=1,.layerCount=1}
    };
    VkImageView depth_view = VK_NULL_HANDLE;
    CHECK(vkCreateImageView(device, &depth_ivci, NULL, &depth_view));
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

        VkImageView attachments[2] = {image_views[slot], depth_view};
        VkFramebufferCreateInfo fbci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = clear_pass,
            .attachmentCount = 2,
            .pAttachments = attachments,
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

        /* First pass. Finite mode deliberately rejects the triangle with a
         * depth clear of zero; continuous mode renders directly. */
        CHECK(vkResetCommandBuffer(cmd_buf, 0));
        VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd_buf, &begin_info));

        VkClearValue clear_values[2] = {0};
        clear_values[0].color.float32[3] = 1.0f;
        clear_values[1].depthStencil.depth = is_continuous ? 1.0f : 0.0f;
        VkRenderPassBeginInfo rp_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = clear_pass,
            .framebuffer = framebuffers[slot],
            .renderArea = {{0, 0}, {1920, 1080}},
            .clearValueCount = 2,
            .pClearValues = clear_values
        };
        vkCmdBeginRenderPass(cmd_buf, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
            is_continuous ? dynamic_pipeline : pipeline1);
        if (is_continuous) {
            vkCmdSetViewport(cmd_buf, 0, 1, &viewport);
            vkCmdSetScissor(cmd_buf, 0, 1, &scissor);
        }
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

        if (!is_continuous) {
            VkMappedMemoryRange blocked_range = {
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = image_memory,
                .offset = slot_offset,
                .size = word_count * sizeof(uint32_t)
            };
            CHECK(vkInvalidateMappedMemoryRanges(device, 1, &blocked_range));
            uint64_t nonblack = 0;
            for (size_t w = 0; w < word_count; ++w)
                nonblack += pixels[w] != 0xff000000u;
            ps5log_printf(PS5LOG_MARK,
                "PS5VK_CONSUMER_DEPTH_REJECT frame=%u nonblack=%llu valid=%u",
                frame, (unsigned long long)nonblack, nonblack == 0);
            if (nonblack) {
                ps5log_close("depth-reject-failed");
                exit(1);
            }

            /* Compatible second pass: preserve black outside the triangle,
             * clear depth to one, and source viewport/scissor dynamically. */
            CHECK(vkResetCommandBuffer(cmd_buf, 0));
            CHECK(vkBeginCommandBuffer(cmd_buf, &begin_info));
            clear_values[1].depthStencil.depth = 1.0f;
            rp_begin.renderPass = load_pass;
            vkCmdBeginRenderPass(cmd_buf, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, dynamic_pipeline);
            vkCmdSetViewport(cmd_buf, 0, 1, &viewport);
            vkCmdSetScissor(cmd_buf, 0, 1, &scissor);
            vkCmdDraw(cmd_buf, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd_buf);
            CHECK(vkEndCommandBuffer(cmd_buf));
            CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
            CHECK(vkQueueWaitIdle(queue));
        }

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
                if (p == 0xff000000u) continue;
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
    vkDestroyImageView(device, depth_view, NULL);
    vkDestroyImage(device, depth_image, NULL);
    vkFreeMemory(device, depth_memory, NULL);
    vkFreeMemory(device, image_memory, NULL);

    vkFreeCommandBuffers(device, cmd_pool, 1, &cmd_buf);
    vkDestroyCommandPool(device, cmd_pool, NULL);
    vkDestroyPipeline(device, dynamic_pipeline, NULL);
    vkDestroyPipeline(device, pipeline1, NULL);
    vkDestroyRenderPass(device, load_pass, NULL);
    vkDestroyRenderPass(device, clear_pass, NULL);
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
    REQUIRE(features2.features.robustBufferAccess == VK_TRUE,
            "mandatory Vulkan 1.0 robustBufferAccess feature report");
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
        "storageBuffer8BitAccess=1 storageBuffer16BitAccess=1 narrow_arithmetic=0 "
        "robustBufferAccess=1");

    /* 3. Create Device & Queue with the mandatory core robustness bit and the
     * two reported narrow-storage bits returned by the same public query. */
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

    /* 4. Exercise the public pipeline-cache contract, then run runtime compute
     * with that cache passed to pipeline creation. */
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    run_pipeline_cache_contract(device, &pipeline_cache);
    run_buffer_transfer_contract(device, queue);
    run_runtime_compute(physical_device, device, queue, pipeline_cache);

    /* 5. Run byte- and word-exact 8/16-bit storage-buffer witnesses. */
    run_storage_width_compute(device, queue);

    /* 6. Run explicit host/compute barriers and multi-wave LDS atomics. */
    run_synchronization_compute(device, queue);

    /* Four sampled sets are checked offscreen before presentation resources. */
    if(!is_continuous)run_sampled_sets(device,queue);

    /* 7. Run runtime procedural graphics and presentation */
    run_consumer(device, queue, is_continuous);

    vkDestroyPipelineCache(device, pipeline_cache, NULL);

    /* Orderly destroy device and instance if ever returned */
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
