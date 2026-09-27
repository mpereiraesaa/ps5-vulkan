/* Core Vulkan 1.1-1.3 structures and names stay dormant on the Vulkan 1.0
 * device, and follow the per-extension answers once a version is reported.
 * The raised version is a test-only mutation of the mocked profile; shipping
 * builds report PS5VK_DEVICE_API_VERSION. */
#include "vk_internal.h"
#include "vk_descriptor.h"
#include "vk_core_version.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = calloc(1, size); *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult cache(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, cache, cache};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }
VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
        .max_allocation = 65536, .queue_flags = VK_QUEUE_COMPUTE_BIT,
        .supported_features = PS5VK_FEATURE_VULKAN_MEMORY_MODEL |
                              PS5VK_FEATURE_STORAGE_BUFFER_16BIT,
        .supported_features_t09 = PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE |
                                  PS5VK_T09_FEATURE_HOST_QUERY_RESET |
                                  PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU", .heap_size = 65536,
        .allocation_granularity = 1, .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

static VkPhysicalDevice physical(VkInstance *i, uint32_t app_version)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = app_version};
    const char *ext = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &ext};
    assert(vkCreateInstance(&info, NULL, i) == VK_SUCCESS);
    uint32_t count = 1;
    VkPhysicalDevice p = VK_NULL_HANDLE;
    assert(vkEnumeratePhysicalDevices(*i, &count, &p) == VK_SUCCESS && p);
    return p;
}

static VkResult create(VkPhysicalDevice p, const void *chain, VkDevice *d)
{
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = chain,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
    return vkCreateDevice(p, &info, NULL, d);
}

static void dormant_on_vulkan_1_0(void)
{
    VkInstance i;
    VkPhysicalDevice p = physical(&i, VK_API_VERSION_1_3);
    assert(p->platform.properties.apiVersion == PS5VK_DEVICE_API_VERSION);
    p->platform.properties.apiVersion = VK_API_VERSION_1_0;
    assert(ps5vk_effective_api_version(p) == VK_API_VERSION_1_0);

    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .drawIndirectCount = VK_TRUE};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12};
    vkGetPhysicalDeviceFeatures2(p, &features);
    assert(v12.drawIndirectCount == VK_TRUE && v12.timelineSemaphore == VK_FALSE); /* untouched */

    VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
                                       .deviceNodeMask = 7};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
    vkGetPhysicalDeviceProperties2(p, &properties);
    assert(id.deviceNodeMask == 7);

    VkPhysicalDeviceVulkan12Features request = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkDevice d = VK_NULL_HANDLE;
    assert(create(p, &request, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);

    assert(create(p, NULL, &d) == VK_SUCCESS);
    assert(!vkGetDeviceProcAddr(d, "vkWaitSemaphores"));
    assert(!vkGetDeviceProcAddr(d, "vkGetBufferMemoryRequirements2"));
    assert(!d->timeline_extension_enabled && !d->maintenance1_extension_enabled);
    assert(!vkGetInstanceProcAddr(i, "vkWaitSemaphores"));
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
}

static void raised(VkInstance i, VkPhysicalDevice p, uint32_t version)
{
    (void)i;
    p->platform.properties.apiVersion = version;
}

static void projections_when_reported(void)
{
    VkInstance i;
    VkPhysicalDevice p = physical(&i, VK_API_VERSION_1_3);
    i->api_version = VK_API_VERSION_1_3; /* the instance version would move in lockstep */
    raised(i, p, VK_API_VERSION_1_3);

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = &timeline, .synchronization2 = VK_TRUE};
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = &v13, .drawIndirectCount = VK_TRUE};
    VkPhysicalDeviceVulkan11Features v11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .pNext = &v12};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v11};
    vkGetPhysicalDeviceFeatures2(p, &features);
    assert(v11.pNext == &v12 && v12.pNext == &v13 && v13.pNext == &timeline);
    assert(v11.storageBuffer16BitAccess && !v11.multiview && !v11.samplerYcbcrConversion);
    assert(v12.timelineSemaphore == timeline.timelineSemaphore && v12.timelineSemaphore);
    assert(v12.hostQueryReset && v12.samplerMirrorClampToEdge && v12.vulkanMemoryModel);
    assert(!v12.drawIndirectCount && !v12.descriptorIndexing && !v12.bufferDeviceAddress);
    assert(!v13.synchronization2 && !v13.dynamicRendering && !v13.maintenance4);

    VkPhysicalDeviceMaintenance4Properties m4 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, .pNext = &m4};
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, .pNext = &p13};
    VkPhysicalDeviceVulkan11Properties p11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, .pNext = &p12};
    VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .pNext = &p11};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
    vkGetPhysicalDeviceProperties2(p, &properties);
    assert(!memcmp(id.deviceUUID, p11.deviceUUID, VK_UUID_SIZE));
    assert(memcmp(p11.deviceUUID, p11.driverUUID, VK_UUID_SIZE) && !p11.deviceLUIDValid);
    assert(p11.subgroupSize == 0 && p11.subgroupSupportedStages == 0);
    assert(p11.pointClippingBehavior == VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES);
    assert(p11.maxPerSetDescriptors == PS5VK_MAX_DESCRIPTORS);
    assert(p11.maxMemoryAllocationSize == p->platform.max_allocation);
    assert(!strcmp(p12.driverName, "ps5vk") && p12.driverID == 0);
    assert(p12.conformanceVersion.major == 0 && !p12.shaderDenormPreserveFloat32);
    assert(p12.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE);
    assert(p12.maxTimelineSemaphoreValueDifference == PS5VK_TIMELINE_MAX_VALUE_DIFFERENCE);
    assert(p12.framebufferIntegerColorSampleCounts == 0 && p12.supportedDepthResolveModes == 0);
    assert(p13.minSubgroupSize == 0 && p13.maxBufferSize == m4.maxBufferSize);
    assert(p13.storageTexelBufferOffsetAlignmentBytes ==
           p->platform.properties.limits.minTexelBufferOffsetAlignment);

    /* Creation: reported members enable their gates, others are refused. */
    VkDevice d = VK_NULL_HANDLE;
    VkPhysicalDeviceVulkan12Features want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore = VK_TRUE, .hostQueryReset = VK_TRUE};
    assert(create(p, &want, &d) == VK_SUCCESS);
    assert(d->enabled_features_t09 & PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE);
    assert(d->enabled_features_t09 & PS5VK_T09_FEATURE_HOST_QUERY_RESET);
    /* Core names resolve without the extension, to the same implementation. */
    assert(vkGetDeviceProcAddr(d, "vkWaitSemaphores") == (PFN_vkVoidFunction)vkWaitSemaphoresKHR);
    /* Promoted behaviour is on without the extension. */
    assert(d->timeline_extension_enabled && d->maintenance4_extension_enabled &&
           d->bind_memory2_extension_enabled && d->create_renderpass2_extension_enabled);
    assert(vkGetDeviceProcAddr(d, "vkTrimCommandPool") == (PFN_vkVoidFunction)vkTrimCommandPoolKHR);
    assert(!vkGetDeviceProcAddr(d, "vkCmdSetRasterizerDiscardEnable")); /* no implementation */
    assert(vkGetInstanceProcAddr(i, "vkWaitSemaphores") == (PFN_vkVoidFunction)vkWaitSemaphoresKHR);
    vkDestroyDevice(d, NULL);

    VkPhysicalDeviceVulkan12Features unsupported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .drawIndirectCount = VK_TRUE};
    d = VK_NULL_HANDLE;
    assert(create(p, &unsupported, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    VkPhysicalDeviceVulkan12Features twice = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan12Features first = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                              .pNext = &twice};
    assert(create(p, &first, &d) == VK_ERROR_UNKNOWN && !d);
    VkPhysicalDeviceVulkan13Features sync2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                              .synchronization2 = VK_TRUE};
    assert(create(p, &sync2, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);
}

static void effective_version_is_the_lower(void)
{
    VkInstance i;
    VkPhysicalDevice p = physical(&i, VK_API_VERSION_1_1);
    raised(i, p, VK_API_VERSION_1_3);
    assert(ps5vk_effective_api_version(p) == VK_API_VERSION_1_1);
    VkDevice d = VK_NULL_HANDLE;
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    assert(create(p, &v13, &d) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(create(p, NULL, &d) == VK_SUCCESS);
    assert(vkGetDeviceProcAddr(d, "vkTrimCommandPool"));
    assert(!vkGetDeviceProcAddr(d, "vkWaitSemaphores"));
    assert(d->maintenance1_extension_enabled && !d->timeline_extension_enabled);
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
}

static void instance_versions(void)
{
    const uint32_t versions[] = {0, VK_API_VERSION_1_0, VK_API_VERSION_1_1,
        VK_API_VERSION_1_2, VK_API_VERSION_1_3, VK_API_VERSION_1_4};
    for (size_t n = 0; n < sizeof(versions) / sizeof(versions[0]); ++n) {
        VkInstance i;
        (void)physical(&i, versions[n]);
        const uint32_t requested = versions[n] ? versions[n] : VK_API_VERSION_1_0;
        assert(i->api_version == (requested < PS5VK_INSTANCE_API_VERSION ?
                                 requested : PS5VK_INSTANCE_API_VERSION));
        vkDestroyInstance(i, NULL);
    }
}

static void graphics_core13_negotiation(void)
{
    VkInstance i;
    VkPhysicalDevice p = physical(&i, VK_API_VERSION_1_3);
    i->api_version = VK_API_VERSION_1_3;
    raised(i, p, VK_API_VERSION_1_3);
    p->platform.supported_features_t09 |= PS5VK_T09_FEATURE_SYNCHRONIZATION2 |
        PS5VK_T09_FEATURE_DYNAMIC_RENDERING | PS5VK_T09_FEATURE_CREATE_RENDERPASS2 |
        PS5VK_T09_FEATURE_DEPTH_STENCIL_RESOLVE | PS5VK_T09_FEATURE_EXTENDED_DYNAMIC_STATE |
        PS5VK_T09_FEATURE_MAINTENANCE2;
    p->platform.supported_features |= PS5VK_FEATURE_MULTIVIEW;
    VkPhysicalDeviceDynamicRenderingFeatures rendering = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceSynchronization2Features sync = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, .pNext = &rendering};
    VkPhysicalDeviceVulkan13Features v13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &sync};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v13};
    vkGetPhysicalDeviceFeatures2(p, &f);
    assert(v13.synchronization2 && v13.synchronization2 == sync.synchronization2);
    assert(v13.dynamicRendering && v13.dynamicRendering == rendering.dynamicRendering);
    assert(!v13.maintenance4 && !v13.privateData && !v13.robustImageAccess);
    v13.pNext = NULL;
    /* ABI tail padding is not a requested feature and may contain any bytes. */
    const size_t last = offsetof(VkPhysicalDeviceVulkan13Features, maintenance4) + sizeof(VkBool32);
    memset((unsigned char *)&v13 + last, 0xa5, sizeof(v13) - last);
    VkDevice d = VK_NULL_HANDLE;
    assert(create(p, &v13, &d) == VK_SUCCESS);
    assert(d->enabled_features_t09 & PS5VK_T09_FEATURE_SYNCHRONIZATION2);
    assert(d->enabled_features_t09 & PS5VK_T09_FEATURE_DYNAMIC_RENDERING);
    assert(d->dynamic_rendering_enabled && d->dynamic_rendering_extension_enabled);
    assert(d->synchronization2_extension_enabled && d->extended_dynamic_state_enabled);
    const char *core[] = {"vkCmdBeginRendering", "vkCmdEndRendering", "vkQueueSubmit2",
        "vkCmdPipelineBarrier2", "vkCmdCopyBuffer2", "vkCmdSetCullMode"};
    const char *ext[] = {"vkCmdBeginRenderingKHR", "vkCmdEndRenderingKHR", "vkQueueSubmit2KHR",
        "vkCmdPipelineBarrier2KHR", "vkCmdCopyBuffer2KHR", "vkCmdSetCullModeEXT"};
    for (size_t n = 0; n < sizeof(core) / sizeof(core[0]); ++n) {
        assert(vkGetDeviceProcAddr(d, core[n]));
        assert(vkGetDeviceProcAddr(d, core[n]) == vkGetDeviceProcAddr(d, ext[n]));
    }
    vkDestroyDevice(d, NULL);
    d = VK_NULL_HANDLE;
    v13.synchronization2 = v13.dynamicRendering = VK_FALSE;
    assert(create(p, &v13, &d) == VK_SUCCESS);
    assert(!d->dynamic_rendering_enabled);
    assert(!(d->enabled_features_t09 & PS5VK_T09_FEATURE_SYNCHRONIZATION2));
    vkDestroyDevice(d, NULL);
    d = VK_NULL_HANDLE;
    /* Individual promoted structs remain usable without extension names. */
    sync.pNext = &rendering;
    assert(create(p, &sync, &d) == VK_SUCCESS);
    assert(d->dynamic_rendering_enabled &&
           (d->enabled_features_t09 & PS5VK_T09_FEATURE_SYNCHRONIZATION2));
    vkDestroyDevice(d, NULL);
    d = VK_NULL_HANDLE;
    /* An extension may satisfy its dependency through the core version. */
    i->features2_extension_enabled = VK_FALSE;
    const char *extension = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &rendering, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
    assert(d->dynamic_rendering_enabled);
    vkDestroyDevice(d, NULL);
    d = VK_NULL_HANDLE;
    /* Aggregate/constituent overlap is invalid in either order, even false. */
    sync.synchronization2 = VK_FALSE; sync.pNext = NULL; v13.pNext = &sync;
    assert(create(p, &v13, &d) == VK_ERROR_UNKNOWN && !d);
    v13.pNext = NULL; sync.pNext = &v13;
    assert(create(p, &sync, &d) == VK_ERROR_UNKNOWN && !d);
    v13.dynamicRendering = VK_TRUE;
    p->platform.supported_features_t09 &= ~PS5VK_T09_FEATURE_DEPTH_STENCIL_RESOLVE;
    assert(create(p, &v13, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    v13.dynamicRendering = VK_FALSE; v13.synchronization2 = 2;
    assert(create(p, &v13, &d) == VK_ERROR_UNKNOWN && !d);
    vkDestroyInstance(i, NULL);
}

/* Isolate feature negotiation and module admission from shader execution. */
static void zero_initialize_negotiation(void)
{
    const uint32_t versions[] = {VK_API_VERSION_1_0, VK_API_VERSION_1_2, VK_API_VERSION_1_3};
    for (unsigned v = 0; v < 3; ++v) {
        VkInstance instance;
        VkPhysicalDevice p = physical(&instance, versions[v]);
        p->platform.properties.apiVersion = versions[v];
        VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures zero = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES,
            .shaderZeroInitializeWorkgroupMemory = VK_TRUE};
        VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                           .pNext = &zero};
        vkGetPhysicalDeviceFeatures2(p, &query);
        assert(!zero.shaderZeroInitializeWorkgroupMemory);
        const char *extension = VK_KHR_ZERO_INITIALIZE_WORKGROUP_MEMORY_EXTENSION_NAME;
        const float priority = 1;
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &zero, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
            .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
        VkDevice d = VK_NULL_HANDLE;
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
        p->platform.supported_features_t09 |= PS5VK_T09_FEATURE_ZERO_INITIALIZE_WORKGROUP_MEMORY;
        vkGetPhysicalDeviceFeatures2(p, &query);
        assert(zero.shaderZeroInitializeWorkgroupMemory);
        VkExtensionProperties extensions[64]; uint32_t count = 64; unsigned found = 0;
        assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, extensions) == VK_SUCCESS);
        for (uint32_t j = 0; j < count; ++j) found += !strcmp(extensions[j].extensionName, extension);
        assert(found == 1);
        if (versions[v] < VK_API_VERSION_1_3)
            assert(create(p, &zero, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
        assert(d->enabled_features_t09 & PS5VK_T09_FEATURE_ZERO_INITIALIZE_WORKGROUP_MEMORY);
        /* Structural fixture: module creation only, not a runnable shader. */
        uint32_t words[] = {0x07230203, 0x10000, 0, 5, 0,
            4u << 16 | 21, 1, 32, 0, 4u << 16 | 32, 2, 4, 1,
            3u << 16 | 46, 1, 3, 5u << 16 | 59, 2, 4, 4, 3};
        VkShaderModuleCreateInfo module_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(words), .pCode = words};
        VkShaderModule module;
        assert(vkCreateShaderModule(d, &module_info, NULL, &module) == VK_SUCCESS);
        vkDestroyShaderModule(d, module, NULL);
        words[14] = 2; /* Null initializer type differs from the pointee. */
        assert(vkCreateShaderModule(d, &module_info, NULL, &module) != VK_SUCCESS && !module);
        words[14] = 1;
        vkDestroyDevice(d, NULL);
        zero.shaderZeroInitializeWorkgroupMemory = VK_FALSE;
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
        assert(!(d->enabled_features_t09 & PS5VK_T09_FEATURE_ZERO_INITIALIZE_WORKGROUP_MEMORY));
        assert(vkCreateShaderModule(d, &module_info, NULL, &module) == VK_ERROR_FEATURE_NOT_PRESENT && !module);
        words[16] = 4u << 16 | 59; words[20] = 1u << 16; /* no initializer, trailing OpNop */
        assert(vkCreateShaderModule(d, &module_info, NULL, &module) == VK_SUCCESS);
        vkDestroyShaderModule(d, module, NULL);
        vkDestroyDevice(d, NULL);
        zero.shaderZeroInitializeWorkgroupMemory = 2;
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_UNKNOWN && !d);
        zero.shaderZeroInitializeWorkgroupMemory = VK_TRUE;
        VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures duplicate = zero;
        zero.pNext = &duplicate;
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_UNKNOWN && !d);
        zero.pNext = NULL;
        if (versions[v] == VK_API_VERSION_1_0) {
            instance->features2_extension_enabled = VK_FALSE;
            assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
        }
        if (versions[v] == VK_API_VERSION_1_3) {
            VkPhysicalDeviceVulkan13Features core = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            query.pNext = &core; vkGetPhysicalDeviceFeatures2(p, &query);
            assert(core.shaderZeroInitializeWorkgroupMemory);
            memset(&core, 0, sizeof(core)); core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            core.shaderZeroInitializeWorkgroupMemory = VK_TRUE;
            assert(create(p, &core, &d) == VK_SUCCESS);
            assert(d->enabled_features_t09 & PS5VK_T09_FEATURE_ZERO_INITIALIZE_WORKGROUP_MEMORY);
            vkDestroyDevice(d, NULL);
            assert(create(p, &zero, &d) == VK_SUCCESS); /* Core name of individual structure. */
            vkDestroyDevice(d, NULL);
            core.pNext = &zero;
            assert(create(p, &core, &d) == VK_ERROR_UNKNOWN && !d);
            core.pNext = NULL;
            p->platform.supported_features_t09 &= ~PS5VK_T09_FEATURE_ZERO_INITIALIZE_WORKGROUP_MEMORY;
            assert(create(p, &core, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
        }
        vkDestroyInstance(instance, NULL);
    }
}

static void cache_control_negotiation(void)
{
    const uint32_t versions[] = {VK_API_VERSION_1_0, VK_API_VERSION_1_2, VK_API_VERSION_1_3};
    for (unsigned v = 0; v < 3; ++v) {
        VkInstance instance;
        VkPhysicalDevice p = physical(&instance, versions[v]);
        p->platform.properties.apiVersion = versions[v];
        VkPhysicalDevicePipelineCreationCacheControlFeatures zero = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES,
            .pipelineCreationCacheControl = VK_TRUE};
        VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                           .pNext = &zero};
        vkGetPhysicalDeviceFeatures2(p, &query);
        assert(!zero.pipelineCreationCacheControl);
        const char *extension = VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME;
        const float priority = 1;
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &zero, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
            .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
        VkDevice d = VK_NULL_HANDLE;
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
        p->platform.supported_features_t09 |= PS5VK_T09_FEATURE_PIPELINE_CREATION_CACHE_CONTROL;
        vkGetPhysicalDeviceFeatures2(p, &query);
        assert(zero.pipelineCreationCacheControl);
        VkExtensionProperties extensions[64]; uint32_t count = 64; unsigned found = 0;
        assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, extensions) == VK_SUCCESS);
        for (uint32_t j = 0; j < count; ++j) found += !strcmp(extensions[j].extensionName, extension);
        assert(found == 1);
        if (versions[v] < VK_API_VERSION_1_3)
            assert(create(p, &zero, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
        assert(d->enabled_features_t09 & PS5VK_T09_FEATURE_PIPELINE_CREATION_CACHE_CONTROL);
        vkDestroyDevice(d, NULL);
        zero.pipelineCreationCacheControl = VK_FALSE;
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
        assert(!(d->enabled_features_t09 & PS5VK_T09_FEATURE_PIPELINE_CREATION_CACHE_CONTROL));
        vkDestroyDevice(d, NULL);
        zero.pipelineCreationCacheControl = 2;
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_UNKNOWN && !d);
        zero.pipelineCreationCacheControl = VK_TRUE;
        VkPhysicalDevicePipelineCreationCacheControlFeatures duplicate = zero;
        zero.pNext = &duplicate;
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_UNKNOWN && !d);
        zero.pNext = NULL;
        if (versions[v] == VK_API_VERSION_1_0) {
            instance->features2_extension_enabled = VK_FALSE;
            assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
        }
        if (versions[v] == VK_API_VERSION_1_3) {
            VkPhysicalDeviceVulkan13Features core = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            query.pNext = &core; vkGetPhysicalDeviceFeatures2(p, &query);
            assert(core.pipelineCreationCacheControl);
            memset(&core, 0, sizeof(core)); core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            core.pipelineCreationCacheControl = VK_TRUE;
            assert(create(p, &core, &d) == VK_SUCCESS);
            assert(d->enabled_features_t09 & PS5VK_T09_FEATURE_PIPELINE_CREATION_CACHE_CONTROL);
            vkDestroyDevice(d, NULL);
            assert(create(p, &zero, &d) == VK_SUCCESS); /* Core name of individual structure. */
            vkDestroyDevice(d, NULL);
            core.pNext = &zero;
            assert(create(p, &core, &d) == VK_ERROR_UNKNOWN && !d);
            core.pNext = NULL;
            p->platform.supported_features_t09 &= ~PS5VK_T09_FEATURE_PIPELINE_CREATION_CACHE_CONTROL;
            assert(create(p, &core, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
        }
        vkDestroyInstance(instance, NULL);
    }
}

static void inline_uniform_negotiation(void)
{
    const uint32_t versions[]={VK_API_VERSION_1_0,VK_API_VERSION_1_1,VK_API_VERSION_1_2,VK_API_VERSION_1_3};
    for(unsigned v=0;v<4;++v) {
        VkInstance instance;VkPhysicalDevice p=physical(&instance,versions[v]);
        p->platform.properties.apiVersion=versions[v];
        VkPhysicalDeviceInlineUniformBlockFeatures f={
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES,
            .inlineUniformBlock=VK_TRUE,.descriptorBindingInlineUniformBlockUpdateAfterBind=VK_TRUE};
        VkPhysicalDeviceFeatures2 query={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&f};
        vkGetPhysicalDeviceFeatures2(p,&query);
        assert(!f.inlineUniformBlock && !f.descriptorBindingInlineUniformBlockUpdateAfterBind);
        VkPhysicalDeviceInlineUniformBlockProperties limits={
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES,
            .maxInlineUniformBlockSize=UINT32_MAX};
        VkPhysicalDeviceProperties2 properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&limits};
        vkGetPhysicalDeviceProperties2(p,&properties);assert(!limits.maxInlineUniformBlockSize);
        const char *extensions[]={VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME,VK_KHR_MAINTENANCE_1_EXTENSION_NAME};
        float priority=1;
        VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount=1,.pQueuePriorities=&priority};
        VkDeviceCreateInfo info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f,
            .queueCreateInfoCount=1,.pQueueCreateInfos=&queue,
            .enabledExtensionCount=v?1:2,.ppEnabledExtensionNames=extensions};
        VkDevice d=NULL;
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_EXTENSION_NOT_PRESENT && !d);
        p->platform.supported_features_t09|=PS5VK_T09_FEATURE_INLINE_UNIFORM_BLOCK;
        if(!v) {
            vkGetPhysicalDeviceFeatures2(p,&query);assert(!f.inlineUniformBlock);
            p->platform.supported_features_t09|=PS5VK_T09_FEATURE_MAINTENANCE1;
            info.enabledExtensionCount=1;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_EXTENSION_NOT_PRESENT && !d);
            info.enabledExtensionCount=2;
        }
        vkGetPhysicalDeviceFeatures2(p,&query);assert(f.inlineUniformBlock && !f.descriptorBindingInlineUniformBlockUpdateAfterBind);
        vkGetPhysicalDeviceProperties2(p,&properties);
        assert(limits.maxInlineUniformBlockSize==PS5VK_MAX_INLINE_UNIFORM_BLOCK_BYTES &&
            limits.maxPerStageDescriptorInlineUniformBlocks==PS5VK_MAX_INLINE_UNIFORM_BLOCKS_PER_STAGE &&
            limits.maxDescriptorSetInlineUniformBlocks==PS5VK_MAX_INLINE_UNIFORM_BLOCKS_PER_SET &&
            limits.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks==limits.maxPerStageDescriptorInlineUniformBlocks &&
            limits.maxDescriptorSetUpdateAfterBindInlineUniformBlocks==limits.maxDescriptorSetInlineUniformBlocks);
        VkExtensionProperties available[64];uint32_t count=64,found=0;
        assert(vkEnumerateDeviceExtensionProperties(p,NULL,&count,available)==VK_SUCCESS);
        for(uint32_t i=0;i<count;++i)found+=!strcmp(available[i].extensionName,extensions[0]);
        assert(found==1);
        if(v<3)assert(create(p,&f,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d->inline_uniform_block_enabled &&
            (d->enabled_features_t09&PS5VK_T09_FEATURE_INLINE_UNIFORM_BLOCK));
        VkDescriptorSetLayoutBinding binding={0,VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,16,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
        VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount=1,.pBindings=&binding};
        VkDescriptorSetLayout layout=NULL;
        assert(vkCreateDescriptorSetLayout(d,&li,NULL,&layout)==VK_SUCCESS);
        vkDestroyDescriptorSetLayout(d,layout,NULL);vkDestroyDevice(d,NULL);
        f.inlineUniformBlock=VK_FALSE;
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && !d->inline_uniform_block_enabled);
        assert(vkCreateDescriptorSetLayout(d,&li,NULL,&layout)==VK_ERROR_FEATURE_NOT_PRESENT && !layout);
        vkDestroyDevice(d,NULL);
        f.inlineUniformBlock=2;assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);
        f.inlineUniformBlock=VK_TRUE;f.descriptorBindingInlineUniformBlockUpdateAfterBind=VK_TRUE;
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
        f.descriptorBindingInlineUniformBlockUpdateAfterBind=2;
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);
        f.descriptorBindingInlineUniformBlockUpdateAfterBind=VK_FALSE;
        VkPhysicalDeviceInlineUniformBlockFeatures duplicate=f;f.pNext=&duplicate;
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);f.pNext=NULL;
        if(!v) {
            instance->features2_extension_enabled=VK_FALSE;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_EXTENSION_NOT_PRESENT && !d);
        }
        if(v==3) {
            VkPhysicalDeviceVulkan13Features core={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            query.pNext=&core;vkGetPhysicalDeviceFeatures2(p,&query);assert(core.inlineUniformBlock);
            memset(&core,0,sizeof(core));core.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;core.inlineUniformBlock=VK_TRUE;
            assert(create(p,&core,&d)==VK_SUCCESS && d->inline_uniform_block_enabled);vkDestroyDevice(d,NULL);
            assert(create(p,&f,&d)==VK_SUCCESS && d->inline_uniform_block_enabled);vkDestroyDevice(d,NULL);
            core.pNext=&f;assert(create(p,&core,&d)==VK_ERROR_UNKNOWN && !d);core.pNext=NULL;
            VkPhysicalDeviceVulkan13Properties cprops={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
            properties.pNext=&cprops;vkGetPhysicalDeviceProperties2(p,&properties);
            assert(cprops.maxInlineUniformBlockSize==limits.maxInlineUniformBlockSize &&
                cprops.maxPerStageDescriptorInlineUniformBlocks==limits.maxPerStageDescriptorInlineUniformBlocks &&
                cprops.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks==limits.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks &&
                cprops.maxDescriptorSetInlineUniformBlocks==limits.maxDescriptorSetInlineUniformBlocks &&
                cprops.maxDescriptorSetUpdateAfterBindInlineUniformBlocks==limits.maxDescriptorSetUpdateAfterBindInlineUniformBlocks &&
                cprops.maxInlineUniformTotalSize==PS5VK_MAX_INLINE_UNIFORM_TOTAL_BYTES);
            p->platform.supported_features_t09&=~PS5VK_T09_FEATURE_INLINE_UNIFORM_BLOCK;
            assert(create(p,&core,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
            vkGetPhysicalDeviceProperties2(p,&properties);assert(!cprops.maxInlineUniformBlockSize && !cprops.maxInlineUniformTotalSize);
        }
        vkDestroyInstance(instance,NULL);
    }
}

static void subgroup_size_negotiation(void)
{
    const uint32_t versions[]={VK_API_VERSION_1_0,VK_API_VERSION_1_1,VK_API_VERSION_1_2,VK_API_VERSION_1_3};
    for(unsigned v=0;v<4;++v) {
        VkInstance instance;VkPhysicalDevice p=physical(&instance,versions[v]);
        p->platform.properties.apiVersion=versions[v];
        VkPhysicalDeviceSubgroupSizeControlFeatures f={
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
        VkPhysicalDeviceFeatures2 query={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&f};
        VkPhysicalDeviceSubgroupSizeControlProperties props={
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        VkPhysicalDeviceProperties2 property={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&props};
        const char *extension=VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME;
        float priority=1;
        VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount=1,.pQueuePriorities=&priority};
        VkDeviceCreateInfo info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f,
            .queueCreateInfoCount=1,.pQueueCreateInfos=&queue,.enabledExtensionCount=1,.ppEnabledExtensionNames=&extension};
        VkDevice d=NULL;
        p->platform.supported_features_v13=PS5VK_V13_FEATURE_SUBGROUP_SIZE_CONTROL|PS5VK_V13_FEATURE_COMPUTE_FULL_SUBGROUPS;
        vkGetPhysicalDeviceFeatures2(p,&query);assert(!f.subgroupSizeControl && !f.computeFullSubgroups);
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_EXTENSION_NOT_PRESENT && !d);
        p->platform.supported_features_t09|=PS5VK_T09_FEATURE_SUBGROUP_BASIC_COMPUTE;
        vkGetPhysicalDeviceFeatures2(p,&query);vkGetPhysicalDeviceProperties2(p,&property);
        VkExtensionProperties available[64];uint32_t count=64,found=0;
        assert(vkEnumerateDeviceExtensionProperties(p,NULL,&count,available)==VK_SUCCESS);
        for(uint32_t i=0;i<count;++i) found+=!strcmp(available[i].extensionName,extension);
        if(!v) {
            assert(!found && !f.subgroupSizeControl && !f.computeFullSubgroups && !props.minSubgroupSize);
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_EXTENSION_NOT_PRESENT && !d);
            vkDestroyInstance(instance,NULL);continue;
        }
        assert(found==1 && f.subgroupSizeControl && f.computeFullSubgroups);
        assert(props.minSubgroupSize==32 && props.maxSubgroupSize==32 &&
            props.maxComputeWorkgroupSubgroups==p->platform.properties.limits.maxComputeWorkGroupInvocations/32 &&
            props.requiredSubgroupSizeStages==VK_SHADER_STAGE_COMPUTE_BIT);
        if(v<3) assert(create(p,&f,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
        for(unsigned mask=0;mask<4;++mask) {
            f.subgroupSizeControl=!!(mask&1);f.computeFullSubgroups=!!(mask&2);
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS);
            assert(d->subgroup_size_control_enabled==f.subgroupSizeControl &&
                d->compute_full_subgroups_enabled==f.computeFullSubgroups && d->enabled_features_v13==mask);
            vkDestroyDevice(d,NULL);
        }
        for(unsigned bit=0;bit<2;++bit) {
            p->platform.supported_features_v13=1u<<bit;
            vkGetPhysicalDeviceFeatures2(p,&query);
            assert(f.subgroupSizeControl==(bit==0) && f.computeFullSubgroups==(bit==1));
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS);vkDestroyDevice(d,NULL);
            f.subgroupSizeControl=f.computeFullSubgroups=VK_TRUE;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
        }
        p->platform.supported_features_v13=3;
        f.subgroupSizeControl=2;assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);
        f.subgroupSizeControl=VK_TRUE;f.computeFullSubgroups=2;
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);f.computeFullSubgroups=VK_TRUE;
        VkPhysicalDeviceSubgroupSizeControlFeatures duplicate=f;f.pNext=&duplicate;
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);f.pNext=NULL;
        if(v==3) {
            VkPhysicalDeviceVulkan13Features core={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            query.pNext=&core;vkGetPhysicalDeviceFeatures2(p,&query);
            assert(core.subgroupSizeControl && core.computeFullSubgroups);
            memset(&core,0,sizeof(core));core.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            core.subgroupSizeControl=core.computeFullSubgroups=VK_TRUE;
            assert(create(p,&core,&d)==VK_SUCCESS && d->subgroup_size_control_enabled && d->compute_full_subgroups_enabled);
            vkDestroyDevice(d,NULL);
            assert(create(p,&f,&d)==VK_SUCCESS && d->enabled_features_v13==3);vkDestroyDevice(d,NULL);
            core.pNext=&f;assert(create(p,&core,&d)==VK_ERROR_UNKNOWN && !d);core.pNext=NULL;
            VkPhysicalDeviceVulkan13Properties cprops={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
            property.pNext=&cprops;vkGetPhysicalDeviceProperties2(p,&property);
            assert(cprops.minSubgroupSize==props.minSubgroupSize && cprops.maxSubgroupSize==props.maxSubgroupSize &&
                cprops.maxComputeWorkgroupSubgroups==props.maxComputeWorkgroupSubgroups &&
                cprops.requiredSubgroupSizeStages==props.requiredSubgroupSizeStages);
            p->platform.supported_features_v13=0;
            assert(create(p,&core,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
            vkGetPhysicalDeviceProperties2(p,&property);assert(!cprops.requiredSubgroupSizeStages && cprops.minSubgroupSize==32);
            instance->api_version=VK_API_VERSION_1_0;p->platform.supported_features_v13=3;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_EXTENSION_NOT_PRESENT && !d);
        }
        vkDestroyInstance(instance,NULL);
    }
}

#define ASSERT_DOT_NOT_ACCELERATED(p) do { \
    assert(!(p).integerDotProduct8BitUnsignedAccelerated); \
    assert(!(p).integerDotProduct8BitSignedAccelerated); \
    assert(!(p).integerDotProduct8BitMixedSignednessAccelerated); \
    assert(!(p).integerDotProduct4x8BitPackedUnsignedAccelerated); \
    assert(!(p).integerDotProduct4x8BitPackedSignedAccelerated); \
    assert(!(p).integerDotProduct4x8BitPackedMixedSignednessAccelerated); \
    assert(!(p).integerDotProduct16BitUnsignedAccelerated); \
    assert(!(p).integerDotProduct16BitSignedAccelerated); \
    assert(!(p).integerDotProduct16BitMixedSignednessAccelerated); \
    assert(!(p).integerDotProduct32BitUnsignedAccelerated); \
    assert(!(p).integerDotProduct32BitSignedAccelerated); \
    assert(!(p).integerDotProduct32BitMixedSignednessAccelerated); \
    assert(!(p).integerDotProduct64BitUnsignedAccelerated); \
    assert(!(p).integerDotProduct64BitSignedAccelerated); \
    assert(!(p).integerDotProduct64BitMixedSignednessAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating8BitUnsignedAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating8BitSignedAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating16BitUnsignedAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating16BitSignedAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating32BitUnsignedAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating32BitSignedAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating64BitUnsignedAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating64BitSignedAccelerated); \
    assert(!(p).integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated); \
} while (0)

static void integer_dot_negotiation(void)
{
    const uint32_t versions[] = {VK_API_VERSION_1_0, VK_API_VERSION_1_1,
                                VK_API_VERSION_1_2, VK_API_VERSION_1_3};
    for (unsigned v = 0; v < 4; ++v) {
        VkInstance instance;
        VkPhysicalDevice p = physical(&instance, versions[v]);
        p->platform.properties.apiVersion = versions[v];
        VkPhysicalDeviceShaderIntegerDotProductFeatures f = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES,
            .shaderIntegerDotProduct = VK_TRUE};
        VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f};
        VkPhysicalDeviceShaderIntegerDotProductProperties props;
        memset(&props, 0xff, sizeof(props));
        VkBaseOutStructure sentinel = {.sType = (VkStructureType)0x7ffffffe};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES;
        props.pNext = &sentinel;
        VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &props};
        const char *extensions[] = {VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME,
                                   VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME};
        float priority = 1;
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
            .enabledExtensionCount = 1, .ppEnabledExtensionNames = extensions};
        VkDevice d = NULL;
        for (unsigned supported = 0; supported < 2; ++supported) {
            p->platform.supported_features_v13 = supported ? PS5VK_V13_FEATURE_SHADER_INTEGER_DOT_PRODUCT : 0;
            vkGetPhysicalDeviceFeatures2(p, &query);
            assert(f.shaderIntegerDotProduct == supported && f.pNext == NULL);
            vkGetPhysicalDeviceProperties2(p, &properties);
            ASSERT_DOT_NOT_ACCELERATED(props);
            assert(props.pNext == &sentinel && sentinel.sType == (VkStructureType)0x7ffffffe);
            VkExtensionProperties available[64]; uint32_t count = 64, found = 0;
            assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, available) == VK_SUCCESS);
            for (uint32_t n = 0; n < count; ++n)
                if (!strcmp(available[n].extensionName, extensions[0])) {
                    ++found; assert(available[n].specVersion == VK_KHR_SHADER_INTEGER_DOT_PRODUCT_SPEC_VERSION);
                }
            assert(found == supported);
            assert(vkCreateDevice(p, &info, NULL, &d) ==
                (supported ? VK_SUCCESS : VK_ERROR_EXTENSION_NOT_PRESENT));
            if (d) { assert(d->enabled_features_v13 == PS5VK_V13_FEATURE_SHADER_INTEGER_DOT_PRODUCT); vkDestroyDevice(d, NULL); }
        }
        /* Enumerating or enabling the extension alone never enables the feature. */
        f.shaderIntegerDotProduct = VK_FALSE;
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS && !d->enabled_features_v13);
        vkDestroyDevice(d, NULL);
        f.shaderIntegerDotProduct = VK_TRUE;
        assert(create(p, &f, &d) == (v == 3 ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT));
        if (d) { assert(d->enabled_features_v13 == PS5VK_V13_FEATURE_SHADER_INTEGER_DOT_PRODUCT); vkDestroyDevice(d, NULL); }
        info.enabledExtensionCount = 2;
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_UNKNOWN && !d);
        info.enabledExtensionCount = 1;
        f.shaderIntegerDotProduct = 2;
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_UNKNOWN && !d);
        f.shaderIntegerDotProduct = VK_TRUE;
        VkPhysicalDeviceShaderIntegerDotProductFeatures duplicate = f; f.pNext = &duplicate;
        assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_UNKNOWN && !d); f.pNext = NULL;
        if (v == 0) {
            instance->features2_extension_enabled = VK_FALSE;
            assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
            instance->features2_extension_enabled = VK_TRUE;
        }
        if (v == 3) {
            VkPhysicalDeviceVulkan13Features core = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            query.pNext = &core; vkGetPhysicalDeviceFeatures2(p, &query);
            assert(core.shaderIntegerDotProduct && !core.subgroupSizeControl && !core.computeFullSubgroups);
            memset(&core, 0, sizeof(core)); core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            core.shaderIntegerDotProduct = VK_TRUE;
            assert(create(p, &core, &d) == VK_SUCCESS && d->enabled_features_v13 == PS5VK_V13_FEATURE_SHADER_INTEGER_DOT_PRODUCT);
            vkDestroyDevice(d, NULL);
            core.shaderIntegerDotProduct = 2;
            assert(create(p, &core, &d) == VK_ERROR_UNKNOWN && !d); core.shaderIntegerDotProduct = VK_TRUE;
            core.pNext = &f; assert(create(p, &core, &d) == VK_ERROR_UNKNOWN && !d); core.pNext = NULL;
            VkPhysicalDeviceVulkan13Properties cp; memset(&cp, 0xff, sizeof(cp));
            cp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES; cp.pNext = NULL;
            properties.pNext = &cp; vkGetPhysicalDeviceProperties2(p, &properties);
            ASSERT_DOT_NOT_ACCELERATED(cp);
            p->platform.supported_features_v13 = 0;
            assert(create(p, &core, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
            assert(create(p, &f, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
            vkGetPhysicalDeviceFeatures2(p, &query); assert(!core.shaderIntegerDotProduct);
            /* Physical reporting does not override an older application's opt-in route. */
            p->platform.supported_features_v13 = PS5VK_V13_FEATURE_SHADER_INTEGER_DOT_PRODUCT;
            instance->api_version = VK_API_VERSION_1_2;
            assert(create(p, &f, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
            assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS); vkDestroyDevice(d, NULL);
        }
        vkDestroyInstance(instance, NULL);
    }
}
#undef ASSERT_DOT_NOT_ACCELERATED

int main(void)
{
    integer_dot_negotiation();
    subgroup_size_negotiation();
    dormant_on_vulkan_1_0();
    projections_when_reported();
    effective_version_is_the_lower();
    instance_versions();
    graphics_core13_negotiation();
    zero_initialize_negotiation();
    cache_control_negotiation();
    inline_uniform_negotiation();
    puts("vk core version: dormant on 1.0, projections and core names on raised versions");
    return 0;
}
