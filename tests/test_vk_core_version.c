/* Core Vulkan 1.1-1.3 structures and names stay dormant on the Vulkan 1.0
 * device, and follow the per-extension answers once a version is reported.
 * The raised version is a test-only mutation of the mocked profile; shipping
 * builds report PS5VK_DEVICE_API_VERSION. */
#include "vk_internal.h"
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

int main(void)
{
    dormant_on_vulkan_1_0();
    projections_when_reported();
    effective_version_is_the_lower();
    puts("vk core version: dormant on 1.0, projections and core names on raised versions");
    return 0;
}
