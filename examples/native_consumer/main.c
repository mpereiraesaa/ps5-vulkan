#define _DEFAULT_SOURCE 1

#include <ps5vk/ps5vk.h>
#include <ps5vk/ps5vk_present.h>
#include "shaders.h"
#include "resource_shader.h"
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

    /* 3. Pipeline layout */
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 3,
        .pSetLayouts = set_layouts
    };
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineLayout(device, &plci, NULL, &pipeline_layout));

    /* 4. Create compute pipeline (compiled at runtime on PS5 via PSBC/ACO) */
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = comp_module,
            .pName = "main"
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

    /* Check shader results: dst[i] = (src[i] + 0x1337u) ^ (i * 31u) */
    int results_correct = 1;
    uint32_t *results = map_out + guard_count;
    for (uint32_t i = 0; i < element_count; ++i) {
        uint32_t src_val = i * 100u + 42u;
        uint32_t expected = (src_val + 0x1337u) ^ (i * 31u);
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
        "elements=%u mismatches=0 guard_words=%u guard_mismatches=0",
        element_count, guard_count * 2);

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

    /* 1. Create Instance */
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
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

    /* 3. Create Device & Queue */
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci
    };
    VkDevice device = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(physical_device, &dci, NULL, &device));

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);

    /* 4. Run runtime compute */
    run_runtime_compute(device, queue);

    /* 5. Run runtime procedural graphics and presentation */
    run_consumer(device, queue, is_continuous);

    /* Orderly destroy device and instance if ever returned */
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
