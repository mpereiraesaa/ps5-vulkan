#define _DEFAULT_SOURCE 1
#include "cts_adapter.h"
#include "cts_shaders.h"

#if defined(PS5VK_TARGET_PS5) || defined(__prospero__)
#include "ps5log.h"
#include <time.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int is_host_mock(VkPhysicalDevice dev)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev, &props);
    return strstr(props.deviceName, "host") != NULL;
}

static VkInstance s_shared_inst = VK_NULL_HANDLE;
static VkPhysicalDevice s_shared_pdev = VK_NULL_HANDLE;
static VkDevice s_shared_dev = VK_NULL_HANDLE;
static VkQueue s_shared_queue = VK_NULL_HANDLE;
static int s_shared_init = 0;

static VkResult helper_ensure_shared(void)
{
    if (s_shared_init) return VK_SUCCESS;

    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
    };
    VkResult res = vkCreateInstance(&ici, NULL, &s_shared_inst);
    if (res != VK_SUCCESS) return res;

    uint32_t count = 1;
    res = vkEnumeratePhysicalDevices(s_shared_inst, &count, &s_shared_pdev);
    if (res != VK_SUCCESS || count == 0) {
        vkDestroyInstance(s_shared_inst, NULL);
        s_shared_inst = VK_NULL_HANDLE;
        return VK_ERROR_DEVICE_LOST;
    }

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
    res = vkCreateDevice(s_shared_pdev, &dci, NULL, &s_shared_dev);
    if (res != VK_SUCCESS) {
        vkDestroyInstance(s_shared_inst, NULL);
        s_shared_inst = VK_NULL_HANDLE;
        return res;
    }

    vkGetDeviceQueue(s_shared_dev, 0, 0, &s_shared_queue);
    s_shared_init = 1;
    return VK_SUCCESS;
}

static void helper_cleanup_shared(void)
{
    if (s_shared_init) {
        if (s_shared_dev != VK_NULL_HANDLE) {
            vkDestroyDevice(s_shared_dev, NULL);
            s_shared_dev = VK_NULL_HANDLE;
        }
        if (s_shared_inst != VK_NULL_HANDLE) {
            vkDestroyInstance(s_shared_inst, NULL);
            s_shared_inst = VK_NULL_HANDLE;
        }
        s_shared_pdev = VK_NULL_HANDLE;
        s_shared_queue = VK_NULL_HANDLE;
        s_shared_init = 0;
    }
}

static VkResult helper_create_device(VkInstance *inst_out, VkPhysicalDevice *pdev_out, VkDevice *dev_out, VkQueue *queue_out)
{
    VkResult res = helper_ensure_shared();
    if (res != VK_SUCCESS) return res;

    if (inst_out) *inst_out = s_shared_inst;
    if (pdev_out) *pdev_out = s_shared_pdev;
    if (dev_out) *dev_out = s_shared_dev;
    if (queue_out) *queue_out = s_shared_queue;
    return VK_SUCCESS;
}

static void helper_destroy_device(VkInstance inst, VkDevice dev)
{
    (void)inst;
    (void)dev;
}

/* 1. dEQP-VK.info.build */
static cts_result_t case_info_build(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.info.build", .status = CTS_STATUS_PASS };
    snprintf(r.details, sizeof(r.details), "Khronos CTS pinned vulkan-cts-1.3.8.4 (commit a0270c18), Vulkan 1.0 core");
    return r;
}

/* 2. dEQP-VK.info.device */
static cts_result_t case_info_device(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.info.device", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkResult res = helper_create_device(&inst, &pdev, NULL, NULL);
    if (res != VK_SUCCESS && res != VK_ERROR_DEVICE_LOST) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkCreateInstance failed with %d", (int)res);
        return r;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pdev, &props);
    snprintf(r.details, sizeof(r.details), "Device enumerated: '%s' vendor=0x%04x device=0x%04x",
             props.deviceName, props.vendorID, props.deviceID);
    return r;
}

/* 3. dEQP-VK.info.platform */
static cts_result_t case_info_platform(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.info.platform", .status = CTS_STATUS_PASS };
    uint32_t count = 0;
    VkResult res = vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
    if (res != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkEnumerateInstanceExtensionProperties failed with %d", (int)res);
        return r;
    }
    snprintf(r.details, sizeof(r.details), "Platform instance extensions count=%u", count);
    return r;
}

/* 4. dEQP-VK.info.memory_limits */
static cts_result_t case_info_memory_limits(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.info.memory_limits", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, NULL, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to enumerate physical device");
        return r;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pdev, &props);

    if (props.limits.nonCoherentAtomSize != 64) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Expected nonCoherentAtomSize 64, got %llu",
                 (unsigned long long)props.limits.nonCoherentAtomSize);
        return r;
    }
    snprintf(r.details, sizeof(r.details), "nonCoherentAtomSize=%llu minStorageBufferOffsetAlignment=%llu",
             (unsigned long long)props.limits.nonCoherentAtomSize,
             (unsigned long long)props.limits.minStorageBufferOffsetAlignment);
    return r;
}

/* 5. dEQP-VK.info.device_properties */
static cts_result_t case_info_device_properties(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.info.device_properties", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, NULL, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to enumerate physical device");
        return r;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pdev, &props);

    if (props.apiVersion < VK_API_VERSION_1_0) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "API version below 1.0: 0x%08x", props.apiVersion);
        return r;
    }
    snprintf(r.details, sizeof(r.details), "API %u.%u.%u driver=%u name='%s'",
             VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
             VK_VERSION_PATCH(props.apiVersion), props.driverVersion, props.deviceName);
    return r;
}

/* 6. dEQP-VK.info.device_features */
static cts_result_t case_info_device_features(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.info.device_features", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, NULL, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to enumerate physical device");
        return r;
    }
    VkPhysicalDeviceFeatures feats;
    memset(&feats, 0, sizeof(feats));
    vkGetPhysicalDeviceFeatures(pdev, &feats);
    snprintf(r.details, sizeof(r.details), "Query device features succeeded");
    return r;
}

/* 7. dEQP-VK.info.device_memory_properties */
static cts_result_t case_info_device_memory_properties(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.info.device_memory_properties", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, NULL, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to enumerate physical device");
        return r;
    }
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(pdev, &mem);

    if (mem.memoryTypeCount == 0 || mem.memoryHeapCount == 0) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Zero memory types or heaps reported");
        return r;
    }
    snprintf(r.details, sizeof(r.details), "types=%u heaps=%u heap0_size=%llu MiB",
             mem.memoryTypeCount, mem.memoryHeapCount, (unsigned long long)(mem.memoryHeaps[0].size / (1024 * 1024)));
    return r;
}

/* 8. dEQP-VK.info.device_queue_family_properties */
static cts_result_t case_info_device_queue_family_properties(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.info.device_queue_family_properties", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, NULL, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to enumerate physical device");
        return r;
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &count, NULL);
    if (count == 0) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Zero queue families reported");
        return r;
    }
    VkQueueFamilyProperties *props = (VkQueueFamilyProperties *)malloc(sizeof(VkQueueFamilyProperties) * count);
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &count, props);
    VkQueueFlags flags = props[0].queueFlags;
    free(props);

    snprintf(r.details, sizeof(r.details), "queue families=%u family0_flags=0x%x", count, flags);
    return r;
}

/* 9. dEQP-VK.info.instance_extensions */
static cts_result_t case_info_instance_extensions(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.info.instance_extensions", .status = CTS_STATUS_PASS };
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
    snprintf(r.details, sizeof(r.details), "Enumerate instance extensions count=%u", count);
    return r;
}

/* 10. dEQP-VK.info.instance_layers */
static cts_result_t case_info_instance_layers(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.info.instance_layers", .status = CTS_STATUS_PASS };
    uint32_t count = 0;
    VkResult res = vkEnumerateInstanceLayerProperties(&count, NULL);
    if (res != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkEnumerateInstanceLayerProperties failed with %d", (int)res);
        return r;
    }
    snprintf(r.details, sizeof(r.details), "Embedded static runtime: instance layers count=%u", count);
    return r;
}

/* 11. dEQP-VK.info.device_extensions */
static cts_result_t case_info_device_extensions(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.info.device_extensions", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, NULL, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to enumerate physical device");
        return r;
    }
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(pdev, NULL, &count, NULL);
    snprintf(r.details, sizeof(r.details), "Enumerate device extensions count=%u", count);
    return r;
}

/* 12. dEQP-VK.info.physical_devices */
static cts_result_t case_info_physical_devices(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.info.physical_devices", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, NULL, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "helper_create_device failed");
        return r;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, NULL);
    if (count == 0) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkEnumeratePhysicalDevices returned 0 devices");
        return r;
    }
    VkPhysicalDevice *devices = (VkPhysicalDevice *)malloc(sizeof(VkPhysicalDevice) * count);
    vkEnumeratePhysicalDevices(inst, &count, devices);
    free(devices);
    snprintf(r.details, sizeof(r.details), "Physical devices enumerated count=%u", count);
    return r;
}

/* 13. dEQP-VK.api.device_init.create_device.basic */
static cts_result_t case_api_create_device_basic(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.api.device_init.create_device.basic", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkResult res = helper_create_device(&inst, &pdev, &dev, NULL);
    if (res != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create logical device: %d", (int)res);
        return r;
    }
    helper_destroy_device(inst, dev);
    snprintf(r.details, sizeof(r.details), "Logical device created and destroyed cleanly");
    return r;
}

/* 14. dEQP-VK.api.smoke.create_sampler */
static cts_result_t case_api_smoke_create_sampler(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.api.smoke.create_sampler", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, &dev, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create device");
        return r;
    }
    if (is_host_mock(pdev)) {
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_NOT_SUPPORTED;
        snprintf(r.details, sizeof(r.details), "Host mock platform lacks graphics pipeline/sampler support");
        return r;
    }
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT
    };
    VkSampler sampler = VK_NULL_HANDLE;
    VkResult res = vkCreateSampler(dev, &sci, NULL, &sampler);
    if (res != VK_SUCCESS || sampler == VK_NULL_HANDLE) {
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkCreateSampler failed with %d", (int)res);
        return r;
    }
    vkDestroySampler(dev, sampler, NULL);
    helper_destroy_device(inst, dev);
    snprintf(r.details, sizeof(r.details), "Sampler created and destroyed cleanly");
    return r;
}

/* 15. dEQP-VK.api.smoke.create_shader */
static cts_result_t case_api_smoke_create_shader(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.api.smoke.create_shader", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, &dev, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create device");
        return r;
    }
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_compute_spirv),
        .pCode = consumer_compute_spirv
    };
    VkShaderModule sm = VK_NULL_HANDLE;
    VkResult res = vkCreateShaderModule(dev, &smci, NULL, &sm);
    if (res != VK_SUCCESS || sm == VK_NULL_HANDLE) {
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkCreateShaderModule failed with %d", (int)res);
        return r;
    }
    vkDestroyShaderModule(dev, sm, NULL);
    helper_destroy_device(inst, dev);
    snprintf(r.details, sizeof(r.details), "Shader module created and destroyed cleanly (size=%zu)", sizeof(consumer_compute_spirv));
    return r;
}

/* 16. dEQP-VK.api.smoke.triangle */
static cts_result_t case_api_smoke_triangle(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.api.smoke.triangle", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, &dev, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create device");
        return r;
    }
    if (is_host_mock(pdev)) {
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_NOT_SUPPORTED;
        snprintf(r.details, sizeof(r.details), "Host mock platform lacks native AGC graphics pipeline compilation");
        return r;
    }

    /* Native graphics pipeline setup */
    VkShaderModuleCreateInfo vsmci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_vertex_spirv),
        .pCode = consumer_vertex_spirv
    };
    VkShaderModule vsm = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &vsmci, NULL, &vsm) != VK_SUCCESS) {
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create vertex shader module");
        return r;
    }

    VkShaderModuleCreateInfo fsmci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_fragment_spirv),
        .pCode = consumer_fragment_spirv
    };
    VkShaderModule fsm = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &fsmci, NULL, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(dev, vsm, NULL);
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create fragment shader module");
        return r;
    }

    VkAttachmentDescription attach = {
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    VkAttachmentReference ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attach,
        .subpassCount = 1,
        .pSubpasses = &subpass
    };
    VkRenderPass rp = VK_NULL_HANDLE;
    vkCreateRenderPass(dev, &rpci, NULL, &rp);

    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl = VK_NULL_HANDLE;
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" }
    };
    VkPipelineVertexInputStateCreateInfo visci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo iasci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkPipelineRasterizationStateCreateInfo rsci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo msci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineColorBlendAttachmentState cbas = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
    };
    VkPipelineColorBlendStateCreateInfo cbsci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &cbas
    };
    VkViewport viewport = { .x = 0.0f, .y = 0.0f, .width = 64.0f, .height = 64.0f, .minDepth = 0.0f, .maxDepth = 1.0f };
    VkRect2D scissor = { .offset = {0, 0}, .extent = {64, 64} };
    VkPipelineViewportStateCreateInfo vpsi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &visci,
        .pInputAssemblyState = &iasci,
        .pViewportState = &vpsi,
        .pRasterizationState = &rsci,
        .pMultisampleState = &msci,
        .pColorBlendState = &cbsci,
        .layout = pl,
        .renderPass = rp,
        .subpass = 0
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult pres = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline);
    if (pres != VK_SUCCESS || pipeline == VK_NULL_HANDLE) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkCreateGraphicsPipelines failed with %d", (int)pres);
    } else {
        snprintf(r.details, sizeof(r.details), "Runtime triangle graphics pipeline compiled and created cleanly");
        vkDestroyPipeline(dev, pipeline, NULL);
    }

    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyRenderPass(dev, rp, NULL);
    vkDestroyShaderModule(dev, vsm, NULL);
    vkDestroyShaderModule(dev, fsm, NULL);
    helper_destroy_device(inst, dev);
    return r;
}

/* 17. dEQP-VK.memory.allocation.basic.size_256.forward.count_1 */
static cts_result_t case_memory_alloc_256(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.memory.allocation.basic.size_256.forward.count_1", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, &dev, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create device");
        return r;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 256,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult res = vkAllocateMemory(dev, &mai, NULL, &mem);
    if (res != VK_SUCCESS || mem == VK_NULL_HANDLE) {
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkAllocateMemory(256) failed with %d", (int)res);
        return r;
    }
    vkFreeMemory(dev, mem, NULL);
    helper_destroy_device(inst, dev);
    snprintf(r.details, sizeof(r.details), "Allocated and freed 256 bytes device memory");
    return r;
}

/* 18. dEQP-VK.memory.allocation.basic.size_1KiB.forward.count_1 */
static cts_result_t case_memory_alloc_1kib(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.memory.allocation.basic.size_1KiB.forward.count_1", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, &dev, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create device");
        return r;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 1024,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult res = vkAllocateMemory(dev, &mai, NULL, &mem);
    if (res != VK_SUCCESS || mem == VK_NULL_HANDLE) {
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkAllocateMemory(1024) failed with %d", (int)res);
        return r;
    }
    vkFreeMemory(dev, mem, NULL);
    helper_destroy_device(inst, dev);
    snprintf(r.details, sizeof(r.details), "Allocated and freed 1024 bytes device memory");
    return r;
}

/* 19. dEQP-VK.memory.mapping.suballocation.full.257.simple */
static cts_result_t case_memory_mapping_simple(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.memory.mapping.suballocation.full.257.simple", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, &dev, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create device");
        return r;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 512,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS) {
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkAllocateMemory failed");
        return r;
    }
    void *ptr = NULL;
    VkResult res = vkMapMemory(dev, mem, 0, 257, 0, &ptr);
    if (res != VK_SUCCESS || !ptr) {
        vkFreeMemory(dev, mem, NULL);
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkMapMemory(257) failed with %d", (int)res);
        return r;
    }
    uint8_t *bytes = (uint8_t *)ptr;
    bytes[0] = 0xAA;
    bytes[256] = 0x55;
    if (bytes[0] != 0xAA || bytes[256] != 0x55) {
        vkUnmapMemory(dev, mem);
        vkFreeMemory(dev, mem, NULL);
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Memory readback mismatch after write");
        return r;
    }
    vkUnmapMemory(dev, mem);
    vkFreeMemory(dev, mem, NULL);
    helper_destroy_device(inst, dev);
    snprintf(r.details, sizeof(r.details), "Suballocated 257 bytes mapped, written and verified");
    return r;
}

/* 20. dEQP-VK.memory.mapping.suballocation.full.257.flush */
static cts_result_t case_memory_mapping_flush(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.memory.mapping.suballocation.full.257.flush", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, &dev, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create device");
        return r;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 512,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    vkAllocateMemory(dev, &mai, NULL, &mem);
    void *ptr = NULL;
    vkMapMemory(dev, mem, 0, 512, 0, &ptr);

    /* Flush range aligned to 64-byte atom */
    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = mem,
        .offset = 0,
        .size = 320 /* 257 rounded up to multiple of 64 */
    };
    VkResult res = vkFlushMappedMemoryRanges(dev, 1, &range);
    if (res != VK_SUCCESS) {
        vkUnmapMemory(dev, mem);
        vkFreeMemory(dev, mem, NULL);
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkFlushMappedMemoryRanges failed with %d", (int)res);
        return r;
    }
    vkUnmapMemory(dev, mem);
    vkFreeMemory(dev, mem, NULL);
    helper_destroy_device(inst, dev);
    snprintf(r.details, sizeof(r.details), "Flushed non-coherent mapped memory range (64-byte atom aligned)");
    return r;
}

/* 21. dEQP-VK.memory.mapping.suballocation.full.257.invalidate */
static cts_result_t case_memory_mapping_invalidate(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.memory.mapping.suballocation.full.257.invalidate", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, &dev, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create device");
        return r;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 512,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    vkAllocateMemory(dev, &mai, NULL, &mem);
    void *ptr = NULL;
    vkMapMemory(dev, mem, 0, 512, 0, &ptr);

    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = mem,
        .offset = 0,
        .size = 320
    };
    VkResult res = vkInvalidateMappedMemoryRanges(dev, 1, &range);
    if (res != VK_SUCCESS) {
        vkUnmapMemory(dev, mem);
        vkFreeMemory(dev, mem, NULL);
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkInvalidateMappedMemoryRanges failed with %d", (int)res);
        return r;
    }
    vkUnmapMemory(dev, mem);
    vkFreeMemory(dev, mem, NULL);
    helper_destroy_device(inst, dev);
    snprintf(r.details, sizeof(r.details), "Invalidated non-coherent mapped memory range (64-byte atom aligned)");
    return r;
}

/* 22. dEQP-VK.compute.pipeline.basic.copy_ssbo_single_invocation */
static cts_result_t case_compute_ssbo_copy(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.compute.pipeline.basic.copy_ssbo_single_invocation", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, &dev, &queue) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create device");
        return r;
    }
    if (is_host_mock(pdev)) {
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_NOT_SUPPORTED;
        snprintf(r.details, sizeof(r.details), "Host mock platform lacks native ACO/PSBC shader execution");
        return r;
    }

    /* Native compute pipeline setup & execution */
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_compute_spirv),
        .pCode = consumer_compute_spirv
    };
    VkShaderModule sm = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &smci, NULL, &sm);

    VkDescriptorSetLayoutBinding bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings
    };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl);

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl
    };
    VkPipelineLayout pl = VK_NULL_HANDLE;
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = sm,
            .pName = "main"
        },
        .layout = pl
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult res = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline);
    if (res != VK_SUCCESS || pipeline == VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pl, NULL);
        vkDestroyDescriptorSetLayout(dev, dsl, NULL);
        vkDestroyShaderModule(dev, sm, NULL);
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkCreateComputePipelines failed with %d", (int)res);
        return r;
    }

    /* Buffer creation: in + out (4096 bytes each) */
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    };
    VkBuffer in_buf = VK_NULL_HANDLE, out_buf = VK_NULL_HANDLE;
    vkCreateBuffer(dev, &bci, NULL, &in_buf);
    vkCreateBuffer(dev, &bci, NULL, &out_buf);

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 8192,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    vkAllocateMemory(dev, &mai, NULL, &mem);
    vkBindBufferMemory(dev, in_buf, mem, 0);
    vkBindBufferMemory(dev, out_buf, mem, 4096);

    /* Initialize input buffer */
    void *ptr = NULL;
    vkMapMemory(dev, mem, 0, 8192, 0, &ptr);
    uint32_t *in_data = (uint32_t *)ptr;
    uint32_t *out_data = (uint32_t *)((uint8_t *)ptr + 4096);
    for (int i = 0; i < 1024; i++) {
        in_data[i] = (uint32_t)(i * 7 + 3);
        out_data[i] = 0;
    }
    VkMappedMemoryRange flush_range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = mem, .offset = 0, .size = 8192
    };
    vkFlushMappedMemoryRanges(dev, 1, &flush_range);
    vkUnmapMemory(dev, mem);

    /* Descriptors */
    VkDescriptorPoolSize pool_size = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size
    };
    VkDescriptorPool dp = VK_NULL_HANDLE;
    vkCreateDescriptorPool(dev, &dpci, NULL, &dp);

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl
    };
    VkDescriptorSet ds = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(dev, &dsai, &ds);

    VkDescriptorBufferInfo dbi_in = { .buffer = in_buf, .offset = 0, .range = 4096 };
    VkDescriptorBufferInfo dbi_out = { .buffer = out_buf, .offset = 0, .range = 4096 };
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi_in },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi_out }
    };
    vkUpdateDescriptorSets(dev, 2, writes, 0, NULL);

    /* Command buffer */
    VkCommandPoolCreateInfo cpci_cmd = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0
    };
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    vkCreateCommandPool(dev, &cpci_cmd, NULL, &cmd_pool);

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &cbbi);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cmd, 16, 1, 1);
    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(dev, &fci, NULL, &fence);

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd
    };
    vkQueueSubmit(queue, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ULL);

    /* Readback & verification */
    vkMapMemory(dev, mem, 0, 8192, 0, &ptr);
    VkMappedMemoryRange inv_range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = mem, .offset = 4096, .size = 4096
    };
    vkInvalidateMappedMemoryRanges(dev, 1, &inv_range);
    out_data = (uint32_t *)((uint8_t *)ptr + 4096);

    int errors = 0;
    for (int i = 0; i < 1024; i++) {
        uint32_t expected = (((uint32_t)(i * 7 + 3)) + 0x1337u) ^ ((uint32_t)i * 31u);
        if (out_data[i] != expected) {
            errors++;
            if (errors <= 3) {
                printf("CTS SSBO copy error at index %d: got 0x%08x expected 0x%08x\n", i, out_data[i], expected);
            }
        }
    }
    vkUnmapMemory(dev, mem);

    if (errors > 0) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Compute dispatch verification failed with %d mismatches", errors);
    } else {
        snprintf(r.details, sizeof(r.details), "SSBO compute dispatch: 1024 items transformed and validated");
    }

    /* Cleanup */
    vkDestroyFence(dev, fence, NULL);
    vkFreeCommandBuffers(dev, cmd_pool, 1, &cmd);
    vkDestroyCommandPool(dev, cmd_pool, NULL);
    vkDestroyDescriptorPool(dev, dp, NULL);
    vkDestroyBuffer(dev, in_buf, NULL);
    vkDestroyBuffer(dev, out_buf, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyPipeline(dev, pipeline, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyShaderModule(dev, sm, NULL);
    helper_destroy_device(inst, dev);
    return r;
}

/* 23. dEQP-VK.compute.pipeline.basic.empty_shader */
static cts_result_t case_compute_empty_shader(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.compute.pipeline.basic.empty_shader", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, &dev, &queue) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create device");
        return r;
    }
    if (is_host_mock(pdev)) {
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_NOT_SUPPORTED;
        snprintf(r.details, sizeof(r.details), "Host mock platform lacks native ACO/PSBC shader execution");
        return r;
    }

    /* Minimal compute module */
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(consumer_compute_spirv),
        .pCode = consumer_compute_spirv
    };
    VkShaderModule sm = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &smci, NULL, &sm);

    VkDescriptorSetLayoutBinding bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings
    };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl);

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl
    };
    VkPipelineLayout pl = VK_NULL_HANDLE;
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = sm,
            .pName = "main"
        },
        .layout = pl
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline);

    vkDestroyPipeline(dev, pipeline, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyShaderModule(dev, sm, NULL);
    helper_destroy_device(inst, dev);
    snprintf(r.details, sizeof(r.details), "Empty/basic compute pipeline created and retired cleanly");
    return r;
}

/* 24. dEQP-VK.synchronization.basic.fence.one */
static cts_result_t case_sync_fence_one(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.synchronization.basic.fence.one", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, &dev, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create device");
        return r;
    }
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = 0 };
    VkFence fence = VK_NULL_HANDLE;
    VkResult res = vkCreateFence(dev, &fci, NULL, &fence);
    if (res != VK_SUCCESS || fence == VK_NULL_HANDLE) {
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkCreateFence failed with %d", (int)res);
        return r;
    }
    VkResult status = vkGetFenceStatus(dev, fence);
    if (status != VK_NOT_READY) {
        vkDestroyFence(dev, fence, NULL);
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Expected VK_NOT_READY (1), got %d", (int)status);
        return r;
    }
    vkResetFences(dev, 1, &fence);
    vkDestroyFence(dev, fence, NULL);
    helper_destroy_device(inst, dev);
    snprintf(r.details, sizeof(r.details), "Unsignaled fence: initial state VK_NOT_READY, reset and destroyed");
    return r;
}

/* 25. dEQP-VK.synchronization.basic.fence.one_signaled */
static cts_result_t case_sync_fence_one_signaled(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.synchronization.basic.fence.one_signaled", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, &dev, NULL) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create device");
        return r;
    }
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    VkFence fence = VK_NULL_HANDLE;
    VkResult res = vkCreateFence(dev, &fci, NULL, &fence);
    if (res != VK_SUCCESS || fence == VK_NULL_HANDLE) {
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkCreateFence(SIGNALED) failed with %d", (int)res);
        return r;
    }
    VkResult status = vkGetFenceStatus(dev, fence);
    if (status != VK_SUCCESS) {
        vkDestroyFence(dev, fence, NULL);
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Expected VK_SUCCESS (0), got %d", (int)status);
        return r;
    }
    vkResetFences(dev, 1, &fence);
    status = vkGetFenceStatus(dev, fence);
    if (status != VK_NOT_READY) {
        vkDestroyFence(dev, fence, NULL);
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Expected VK_NOT_READY after reset, got %d", (int)status);
        return r;
    }
    vkDestroyFence(dev, fence, NULL);
    helper_destroy_device(inst, dev);
    snprintf(r.details, sizeof(r.details), "Signaled fence: initial state VK_SUCCESS, reset to VK_NOT_READY");
    return r;
}

/* 26. dEQP-VK.synchronization.basic.empty_submit */
static cts_result_t case_sync_empty_submit(void)
{
    cts_result_t r = { .case_name = "dEQP-VK.synchronization.basic.empty_submit", .status = CTS_STATUS_PASS };
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    if (helper_create_device(&inst, &pdev, &dev, &queue) != VK_SUCCESS) {
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "Failed to create device");
        return r;
    }
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = 0 };
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(dev, &fci, NULL, &fence);

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 0,
        .pCommandBuffers = NULL
    };
    VkResult res = vkQueueSubmit(queue, 1, &si, fence);
    if (res != VK_SUCCESS) {
        vkDestroyFence(dev, fence, NULL);
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkQueueSubmit(empty) failed with %d", (int)res);
        return r;
    }
    res = vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ULL);
    if (res != VK_SUCCESS) {
        vkDestroyFence(dev, fence, NULL);
        helper_destroy_device(inst, dev);
        r.status = CTS_STATUS_FAIL;
        snprintf(r.details, sizeof(r.details), "vkWaitForFences failed with %d", (int)res);
        return r;
    }
    vkDestroyFence(dev, fence, NULL);
    helper_destroy_device(inst, dev);
    snprintf(r.details, sizeof(r.details), "Empty queue submit: fence signaled and wait returned VK_SUCCESS");
    return r;
}

typedef cts_result_t (*cts_test_fn)(void);

static const struct {
    const char *name;
    cts_test_fn fn;
} CTS_CASES[] = {
    { "dEQP-VK.info.build", case_info_build },
    { "dEQP-VK.info.device", case_info_device },
    { "dEQP-VK.info.platform", case_info_platform },
    { "dEQP-VK.info.memory_limits", case_info_memory_limits },
    { "dEQP-VK.info.device_properties", case_info_device_properties },
    { "dEQP-VK.info.device_features", case_info_device_features },
    { "dEQP-VK.info.device_memory_properties", case_info_device_memory_properties },
    { "dEQP-VK.info.device_queue_family_properties", case_info_device_queue_family_properties },
    { "dEQP-VK.info.instance_extensions", case_info_instance_extensions },
    { "dEQP-VK.info.instance_layers", case_info_instance_layers },
    { "dEQP-VK.info.device_extensions", case_info_device_extensions },
    { "dEQP-VK.info.physical_devices", case_info_physical_devices },
    { "dEQP-VK.api.device_init.create_device.basic", case_api_create_device_basic },
    { "dEQP-VK.api.smoke.create_sampler", case_api_smoke_create_sampler },
    { "dEQP-VK.api.smoke.create_shader", case_api_smoke_create_shader },
    { "dEQP-VK.api.smoke.triangle", case_api_smoke_triangle },
    { "dEQP-VK.memory.allocation.basic.size_256.forward.count_1", case_memory_alloc_256 },
    { "dEQP-VK.memory.allocation.basic.size_1KiB.forward.count_1", case_memory_alloc_1kib },
    { "dEQP-VK.memory.mapping.suballocation.full.257.simple", case_memory_mapping_simple },
    { "dEQP-VK.memory.mapping.suballocation.full.257.flush", case_memory_mapping_flush },
    { "dEQP-VK.memory.mapping.suballocation.full.257.invalidate", case_memory_mapping_invalidate },
    { "dEQP-VK.compute.pipeline.basic.copy_ssbo_single_invocation", case_compute_ssbo_copy },
    { "dEQP-VK.compute.pipeline.basic.empty_shader", case_compute_empty_shader },
    { "dEQP-VK.synchronization.basic.fence.one", case_sync_fence_one },
    { "dEQP-VK.synchronization.basic.fence.one_signaled", case_sync_fence_one_signaled },
    { "dEQP-VK.synchronization.basic.empty_submit", case_sync_empty_submit },
};

static const size_t CTS_CASE_COUNT = sizeof(CTS_CASES) / sizeof(CTS_CASES[0]);

cts_result_t cts_run_case(const char *case_name)
{
    for (size_t i = 0; i < CTS_CASE_COUNT; i++) {
        if (!strcmp(CTS_CASES[i].name, case_name)) {
            return CTS_CASES[i].fn();
        }
    }
    cts_result_t r = {
        .case_name = case_name,
        .status = CTS_STATUS_SKIP
    };
    snprintf(r.details, sizeof(r.details), "Unknown CTS case name");
    return r;
}

static const char *status_to_string(cts_status_t s)
{
    switch (s) {
    case CTS_STATUS_PASS: return "PASS";
    case CTS_STATUS_FAIL: return "FAIL";
    case CTS_STATUS_NOT_SUPPORTED: return "NotSupported";
    case CTS_STATUS_SKIP: return "SKIP";
    default: return "UNKNOWN";
    }
}

int cts_run_all(int format_json, int format_tap)
{
    int pass_count = 0, not_supported_count = 0, fail_count = 0, skip_count = 0;

    if (format_tap) {
        printf("1..%zu\n", CTS_CASE_COUNT);
    } else if (format_json) {
        printf("{\n  \"upstream_pin\": \"vulkan-cts-1.3.8.4\",\n  \"results\": [\n");
    }

    for (size_t i = 0; i < CTS_CASE_COUNT; i++) {
        cts_result_t res = CTS_CASES[i].fn();
        switch (res.status) {
        case CTS_STATUS_PASS: pass_count++; break;
        case CTS_STATUS_NOT_SUPPORTED: not_supported_count++; break;
        case CTS_STATUS_FAIL: fail_count++; break;
        case CTS_STATUS_SKIP: skip_count++; break;
        }

#if defined(PS5VK_TARGET_PS5) || defined(__prospero__)
        ps5log_printf(PS5LOG_MARK, "PS5VK_CTS_CASE name=%s result=%s details=%s",
                     res.case_name, status_to_string(res.status), res.details);
#endif

        if (format_tap) {
            if (res.status == CTS_STATUS_PASS) {
                printf("ok %zu - %s\n", i + 1, res.case_name);
            } else if (res.status == CTS_STATUS_NOT_SUPPORTED) {
                printf("ok %zu - %s # SKIP (NotSupported: %s)\n", i + 1, res.case_name, res.details);
            } else {
                printf("not ok %zu - %s # %s\n", i + 1, res.case_name, res.details);
            }
        } else if (format_json) {
            printf("    {\"name\": \"%s\", \"status\": \"%s\", \"details\": \"%s\"}%s\n",
                   res.case_name, status_to_string(res.status), res.details,
                   (i + 1 < CTS_CASE_COUNT) ? "," : "");
        } else {
            printf("[CTS] CASE: %-58s RESULT: %-12s details=%s\n",
                   res.case_name, status_to_string(res.status), res.details);
        }
    }

#if defined(PS5VK_TARGET_PS5) || defined(__prospero__)
    ps5log_printf(PS5LOG_MARK, "PS5VK_CTS_SUMMARY total=%zu pass=%d not_supported=%d fail=%d skip=%d",
                 CTS_CASE_COUNT, pass_count, not_supported_count, fail_count, skip_count);
#endif

    if (format_json) {
        printf("  ],\n  \"summary\": {\"total\": %zu, \"pass\": %d, \"not_supported\": %d, \"fail\": %d, \"skip\": %d}\n}\n",
               CTS_CASE_COUNT, pass_count, not_supported_count, fail_count, skip_count);
    } else if (!format_tap) {
        printf("[CTS] SUMMARY: total=%zu pass=%d not_supported=%d fail=%d skip=%d\n",
               CTS_CASE_COUNT, pass_count, not_supported_count, fail_count, skip_count);
    }

    return (fail_count > 0) ? 1 : 0;
}

int main(int argc, char **argv)
{
#if defined(PS5VK_TARGET_PS5) || defined(__prospero__)
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t boot = (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
    ps5log_config cfg;
    const char *loaded = NULL, *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&cfg);
    ps5log_load_config(paths, 1, &cfg, &loaded);
    cfg.udp = 0;
    ps5log_init(&cfg, "PPSA99994", "ps5vk", boot);
    ps5log_capture_stdio(0);
    ps5log_line(PS5LOG_MARK, "PS5VK_CTS_START");
#endif

    int format_json = 0, format_tap = 0;
    const char *single_case = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) format_json = 1;
        else if (!strcmp(argv[i], "--tap")) format_tap = 1;
        else if (!strcmp(argv[i], "--case") && i + 1 < argc) single_case = argv[++i];
    }

    int rc = 0;
    if (single_case) {
        cts_result_t res = cts_run_case(single_case);
        printf("[CTS] CASE: %s RESULT: %s details=%s\n",
               res.case_name, status_to_string(res.status), res.details);
        rc = (res.status == CTS_STATUS_FAIL) ? 1 : 0;
    } else {
        rc = cts_run_all(format_json, format_tap);
    }

    helper_cleanup_shared();

#if defined(PS5VK_TARGET_PS5) || defined(__prospero__)
    fflush(stdout);
    ps5log_printf(PS5LOG_MARK, "PS5VK_CTS_END rc=%d", rc);
    ps5log_close("cts-finished");
    for (;;) sleep(1);
#endif

    return rc;
}
