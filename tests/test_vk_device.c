#include "vk_internal.h"
#include "graphics_formats.h"
#include "physical_device_profile.h"
#include "device_profile_report.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>

/* Compile the *consumer's* independent assertions unchanged against the real
 * public query entry points. Only platform discovery is mocked here. */
static jmp_buf consumer_rejection;
static int expect_consumer_rejection;
static const char *consumer_failed_contract;
static void consumer_require(int condition, const char *message)
{
    if (condition) return;
    consumer_failed_contract = message;
    if (expect_consumer_rejection) longjmp(consumer_rejection, 1);
    fprintf(stderr, "Consumer contract failed: %s\n", message);
    abort();
}
static void consumer_log(int level, const char *fmt, ...)
{
    (void)level;
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n');
}
#define REQUIRE(c, m) consumer_require(!!(c), (m))
#define PS5LOG_MARK 0
#define ps5log_printf consumer_log
#define ps5log_line(level, message) consumer_log(level, "%s", message)
#include "../examples/native_consumer/physical_device_contract.h"
#undef REQUIRE
#undef PS5LOG_MARK
#undef ps5log_printf
#undef ps5log_line

static unsigned opened, closed;
static VkResult query_result, open_result;
static int malformed;
static VkBool32 mock_wsi_available;
VkBool32 ps5vk_wsi_present_available(void) { return mock_wsi_available; }
static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = malloc(size); *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult cache(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    if (open_result) return open_result;
    ++opened;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, cache, cache};
    if (malformed == 2) backend->flush = NULL;
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; ++closed; }
VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    if (query_result) return query_result;
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
                               .max_allocation = 65536, .queue_flags = VK_QUEUE_COMPUTE_BIT,
                               .supported_features = PS5VK_FEATURE_ROBUST_BUFFER_ACCESS};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU",
        .heap_size = 65536,
        .allocation_granularity = 1,
        .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    if (malformed == 1) p->properties.limits.nonCoherentAtomSize = 3;
    if (malformed == 3) p->memory_properties.memoryHeaps[0].flags = 0;
    if (malformed == 4) p->properties.limits.maxMemoryAllocationCount = 0;
    if (malformed == 5) p->format_properties = ps5vk_graphics_format_properties;
    if (malformed == 6) p->properties.limits.maxStorageBufferRange = 65537;
    if (malformed == 7)
        memset(p->properties.deviceName, 'x', sizeof(p->properties.deviceName));
    return VK_SUCCESS;
}
static VkInstance instance(void)
{
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance i;
    assert(vkCreateInstance(&info, NULL, &i) == VK_SUCCESS);
    return i;
}
static VkInstance features2_instance(void)
{
    const char *extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
    VkInstance i;
    assert(vkCreateInstance(&info, NULL, &i) == VK_SUCCESS);
    return i;
}
static VkPhysicalDevice physical(VkInstance i)
{
    uint32_t count = 0;
    assert(vkEnumeratePhysicalDevices(i, &count, NULL) == VK_SUCCESS && count == 1);
    VkPhysicalDevice p[2] = {NULL, NULL};
    count = 0;
    assert(vkEnumeratePhysicalDevices(i, &count, p) == VK_INCOMPLETE && !p[0] && count == 0);
    count = 2;
    assert(vkEnumeratePhysicalDevices(i, &count, p) == VK_SUCCESS && count == 1 && !p[1]);
    return p[0];
}
static VkDeviceCreateInfo device_info(VkDeviceQueueCreateInfo *q, float *priority)
{
    *priority = 1.0f;
    *q = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                 .queueCount = 1, .pQueuePriorities = priority};
    return (VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                .queueCreateInfoCount = 1, .pQueueCreateInfos = q};
}
static void lifecycle(void)
{
    VkPhysicalDeviceLimits gl={.maxComputeWorkGroupInvocations=1024,.nonCoherentAtomSize=64,
        .maxPerStageDescriptorStorageBuffers=128,.maxDescriptorSetStorageBuffers=128,.maxPerStageResources=128};
    ps5vk_graphics_limits(&gl);
    assert(gl.maxComputeWorkGroupInvocations==1024 && gl.nonCoherentAtomSize==64);
    assert(gl.maxPerStageDescriptorStorageBuffers==128 && gl.maxDescriptorSetStorageBuffers==128 && gl.maxPerStageResources==128);
    assert(gl.maxImageDimension1D==PS5VK_MAX_IMAGE_1D &&
        gl.maxImageDimension2D==16383 &&
        gl.maxImageDimension3D==PS5VK_MAX_IMAGE_3D &&
        gl.maxImageDimensionCube==PS5VK_MAX_IMAGE_CUBE &&
        gl.maxImageArrayLayers==PS5VK_MAX_IMAGE_ARRAY_LAYERS);
    /* Sampled descriptors report the qualified Vulkan 1.0 floors, not the old
     * one-descriptor contract: one set carries the whole table the runtime draw
     * ABI addresses and one stage reads as many records as the layout reserves. */
    assert(gl.maxPerStageDescriptorSamplers==PS5VK_QUALIFIED_STAGE_SAMPLED_DESCRIPTORS &&
        gl.maxPerStageDescriptorSampledImages==PS5VK_QUALIFIED_STAGE_SAMPLED_DESCRIPTORS);
    assert(gl.maxDescriptorSetSamplers==PS5VK_QUALIFIED_SET_SAMPLED_DESCRIPTORS &&
        gl.maxDescriptorSetSampledImages==PS5VK_QUALIFIED_SET_SAMPLED_DESCRIPTORS);
    assert(gl.maxSamplerAllocationCount==PS5VK_MAX_SAMPLERS && PS5VK_MAX_SAMPLERS==4096);
    /* DXVK262-T06 independentBlend is promoted: the ABI, the render pass, the
     * framebuffer, the pipeline key and the native per-target programming carry
     * two colour attachments, both upstream leaves that require the feature
     * pass on hardware, and the advertised bound is the served one. */
    assert(gl.maxColorAttachments==PS5VK_MAX_COLOR_ATTACHMENTS &&
           gl.maxFragmentOutputAttachments==PS5VK_MAX_COLOR_ATTACHMENTS &&
           gl.maxFragmentCombinedOutputResources==PS5VK_MAX_COLOR_ATTACHMENTS &&
           PS5VK_MAX_COLOR_ATTACHMENTS==2);
    const VkFormat guarantee_formats[]={VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_D32_SFLOAT};
    const VkImageUsageFlags guarantee_usages[]={VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    for(unsigned f=0;f<3;++f) {
        VkImageFormatProperties image_limits;
        assert(ps5vk_graphics_image_properties(guarantee_formats[f],VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL,guarantee_usages[f],0,1u<<28,&image_limits)==VK_SUCCESS);
        assert(image_limits.maxExtent.width>=gl.maxImageDimension2D);
        assert(image_limits.maxExtent.height>=gl.maxImageDimension2D);
    }
    assert(gl.maxFramebufferWidth==16383 && gl.maxFramebufferHeight==16383);
    assert(gl.maxFramebufferLayers==1);
    assert(gl.framebufferColorSampleCounts==1 && gl.framebufferDepthSampleCounts==1);
    assert(gl.sampledImageColorSampleCounts==1 &&
        gl.sampledImageIntegerSampleCounts==1 && !gl.sampledImageDepthSampleCounts);
    assert(gl.maxViewports==1 && gl.maxViewportDimensions[0]==16384 && gl.maxViewportDimensions[1]==16384);
    assert(gl.viewportBoundsRange[0]==-32768 && gl.viewportBoundsRange[1]==32767);
    assert(gl.maxVertexInputBindings==16 && gl.maxVertexInputAttributes==32);
    assert(ps5vk_graphics_vertex_bindings_available(&gl,1));
    assert(ps5vk_graphics_vertex_bindings_available(&gl,16));
    assert(!ps5vk_graphics_vertex_bindings_available(&gl,17));
    VkPhysicalDeviceLimits old_binding_limit=gl;old_binding_limit.maxVertexInputBindings=1;
    assert(ps5vk_graphics_vertex_bindings_available(&old_binding_limit,1));
    assert(!ps5vk_graphics_vertex_bindings_available(&old_binding_limit,16));
    assert(gl.maxVertexInputBindingStride==16380 && gl.maxVertexInputAttributeOffset==16376);
    assert(gl.maxDrawIndexedIndexValue==UINT32_MAX);
    assert(gl.subPixelPrecisionBits==8);
    /* Raster quantization is not a claim about sampler/viewport precision. */
    assert(!gl.viewportSubPixelBits);
    /* The full graphics profile reports the Vulkan 1.0 mandatory floors for
     * texture precision and the compiled VS/FS interface, plus the fixed 1.0
     * sizes implied by largePoints/wideLines being VK_FALSE. Those come from
     * the shared initializer, so they hold for every shipped profile, and the
     * validator rejects a report that drops any of them. */
    {
        VkPhysicalDeviceProperties profile;
        VkPhysicalDeviceMemoryProperties profile_memory;
        ps5vk_device_profile_init(&profile, &profile_memory, VK_TRUE, VK_TRUE, 0);
        const VkPhysicalDeviceLimits *pl = &profile.limits;
        assert(pl->subTexelPrecisionBits==PS5VK_REQUIRED_SUBTEXEL_BITS);
        assert(pl->mipmapPrecisionBits==PS5VK_REQUIRED_MIPMAP_PRECISION_BITS);
        assert(pl->maxVertexOutputComponents==PS5VK_REQUIRED_INTERFACE_COMPONENTS);
        assert(pl->maxFragmentInputComponents==PS5VK_REQUIRED_INTERFACE_COMPONENTS);
        assert(pl->maxSampleMaskWords==PS5VK_REQUIRED_SAMPLE_MASK_WORDS);
        assert(pl->sampledImageIntegerSampleCounts==VK_SAMPLE_COUNT_1_BIT);
        assert(pl->pointSizeRange[0]==PS5VK_REQUIRED_POINT_SIZE &&
               pl->pointSizeRange[1]==PS5VK_REQUIRED_POINT_SIZE);
        assert(pl->lineWidthRange[0]==PS5VK_REQUIRED_LINE_WIDTH &&
               pl->lineWidthRange[1]==PS5VK_REQUIRED_LINE_WIDTH);
        assert(strcmp(profile.deviceName, PS5VK_PROFILE_GRAPHICS_NAME)==0);
        assert(profile_memory.memoryHeaps[0].size==PS5VK_PROFILE_GRAPHICS_HEAP_BYTES);
        /* The compute-only profile shares the same floors. */
        VkPhysicalDeviceProperties compute_profile;
        VkPhysicalDeviceMemoryProperties compute_memory;
        ps5vk_device_profile_init(&compute_profile, &compute_memory, VK_FALSE, VK_FALSE, 0);
        assert(compute_profile.limits.subTexelPrecisionBits==PS5VK_REQUIRED_SUBTEXEL_BITS);
        assert(compute_profile.limits.pointSizeRange[1]==PS5VK_REQUIRED_POINT_SIZE);
        assert(compute_profile.limits.sampledImageIntegerSampleCounts==VK_SAMPLE_COUNT_1_BIT);
        assert(strcmp(compute_profile.deviceName, PS5VK_PROFILE_COMPUTE_NAME)==0);
        /* maxDrawIndirectCount follows the platform mask through the shared
         * initializer: exactly 1 without multiDrawIndirect, the core floor
         * 65535 with it, on either profile, so a platform cannot report the
         * feature and the old limit or the limit without the feature. */
        assert(pl->maxDrawIndirectCount==1 && compute_profile.limits.maxDrawIndirectCount==1);
        VkPhysicalDeviceProperties multi_profile;
        VkPhysicalDeviceMemoryProperties multi_memory;
        ps5vk_device_profile_init(&multi_profile, &multi_memory, VK_TRUE, VK_TRUE,
            PS5VK_FEATURE_MULTI_DRAW_INDIRECT|PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE);
        assert(multi_profile.limits.maxDrawIndirectCount==65535);
        ps5vk_device_profile_init(&multi_profile, &multi_memory, VK_TRUE, VK_TRUE,
            PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE|PS5VK_FEATURE_FULL_DRAW_INDEX_UINT32);
        assert(multi_profile.limits.maxDrawIndirectCount==1);
        /* maxViewports follows multiViewport the same way: the Vulkan floor
         * of 16 with the bit, exactly 1 without it, and the pipeline's array
         * capacity is the same constant. */
        assert(ps5vk_platform_max_viewports(0)==1 &&
               ps5vk_platform_max_viewports(PS5VK_FEATURE_MULTI_VIEWPORT)==16 &&
               ps5vk_platform_max_viewports(PS5VK_FEATURE_MULTI_VIEWPORT|PS5VK_FEATURE_DEPTH_CLAMP)==16 &&
               ps5vk_platform_max_viewports(PS5VK_FEATURE_DEPTH_CLAMP)==1 &&
               PS5VK_MULTI_VIEWPORT_COUNT==16);
        /* ...and the shared profile initializer reports that limit from the
         * same mask that reports the feature, so a platform can never say
         * multiViewport with maxViewports 1 (measured run 20260917T200309802Z). */
        assert(pl->maxViewports==1 && compute_profile.limits.maxViewports==1);
        ps5vk_device_profile_init(&multi_profile, &multi_memory, VK_TRUE, VK_TRUE,
            PS5VK_FEATURE_MULTI_VIEWPORT|PS5VK_FEATURE_DEPTH_CLAMP|PS5VK_FEATURE_MULTI_DRAW_INDIRECT);
        assert(multi_profile.limits.maxViewports==PS5VK_MULTI_VIEWPORT_COUNT &&
               multi_profile.limits.maxDrawIndirectCount==65535);
        ps5vk_device_profile_init(&multi_profile, &multi_memory, VK_TRUE, VK_TRUE,
            PS5VK_FEATURE_DEPTH_BIAS_CLAMP|PS5VK_FEATURE_DEPTH_CLAMP|PS5VK_FEATURE_FILL_MODE_NON_SOLID);
        assert(multi_profile.limits.maxViewports==1);
        assert(compute_memory.memoryHeaps[0].size==PS5VK_PROFILE_COMPUTE_HEAP_BYTES);
        const VkDeviceSize max_allocation = PS5VK_PROFILE_GRAPHICS_HEAP_BYTES;
        const VkQueueFlags queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        assert(ps5vk_physical_profile_valid(&profile, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        VkPhysicalDeviceProperties broken = profile;
        broken.limits.subTexelPrecisionBits = 0;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.sampledImageIntegerSampleCounts = 0;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.mipmapPrecisionBits = 0;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.maxVertexOutputComponents = 0;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.maxFragmentInputComponents = 0;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.maxSampleMaskWords = 0;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.pointSizeRange[0] = 0.0f;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.lineWidthRange[1] = 0.0f;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        /* Sampled-descriptor limits: the shipped graphics profile reports the
         * qualified minima, and both a dropped value and an inflated claim past
         * the descriptor table capacity fail the profile. */
        assert(profile.limits.maxPerStageDescriptorSamplers ==
               PS5VK_QUALIFIED_STAGE_SAMPLED_DESCRIPTORS &&
               profile.limits.maxPerStageDescriptorSampledImages ==
               PS5VK_QUALIFIED_STAGE_SAMPLED_DESCRIPTORS &&
               profile.limits.maxDescriptorSetSamplers ==
               PS5VK_QUALIFIED_SET_SAMPLED_DESCRIPTORS &&
               profile.limits.maxDescriptorSetSampledImages ==
               PS5VK_QUALIFIED_SET_SAMPLED_DESCRIPTORS);
        broken = profile;
        broken.limits.maxPerStageDescriptorSamplers = PS5VK_QUALIFIED_STAGE_SAMPLED_DESCRIPTORS - 1;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.maxPerStageDescriptorSamplers = PS5VK_MAX_DESCRIPTORS + 1;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.maxPerStageDescriptorSampledImages = PS5VK_QUALIFIED_STAGE_SAMPLED_DESCRIPTORS - 1;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.maxPerStageDescriptorSampledImages = PS5VK_MAX_DESCRIPTORS + 1;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.maxDescriptorSetSamplers = PS5VK_QUALIFIED_SET_SAMPLED_DESCRIPTORS - 1;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.maxDescriptorSetSamplers = PS5VK_MAX_DESCRIPTORS + 1;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.maxDescriptorSetSampledImages = PS5VK_QUALIFIED_SET_SAMPLED_DESCRIPTORS - 1;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        broken = profile;
        broken.limits.maxDescriptorSetSampledImages = PS5VK_MAX_DESCRIPTORS + 1;
        assert(!ps5vk_physical_profile_valid(&broken, &profile_memory, max_allocation,
            queue_flags, 1, 1));
        assert(ps5vk_physical_profile_valid(&profile, &profile_memory, max_allocation,
            queue_flags, 1, 1));
    }
    /* Enumerate all combinations of known core image role bits, not only the
     * three happy paths. Mixed executable/non-executable roles must fail. */
    for(unsigned usage=0;usage<256;++usage) {
        assert(!!ps5vk_graphics_image_usage(VK_FORMAT_B8G8R8A8_UNORM,usage)==
            (usage==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ||
             usage==VK_IMAGE_USAGE_TRANSFER_DST_BIT ||
             usage==(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT)));
        /* D32 also has a separate bounded sampled-depth role for Dref gather.
         * The depth attachment forms remain tiled, and transfer source never
         * appears without the attachment. */
        assert(!!ps5vk_graphics_image_usage(VK_FORMAT_D32_SFLOAT,usage)==
            (usage==VK_IMAGE_USAGE_SAMPLED_BIT ||
             usage==(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
             usage==VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT ||
             usage==VK_IMAGE_USAGE_TRANSFER_DST_BIT ||
             usage==(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
             usage==(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
             usage==(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT)));
    assert(!!ps5vk_graphics_image_usage(VK_FORMAT_R8G8B8A8_UNORM,usage)==
            (usage==VK_IMAGE_USAGE_SAMPLED_BIT || usage==VK_IMAGE_USAGE_TRANSFER_DST_BIT ||
             usage==(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
             usage==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ||
             usage==(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
             /* The pinned upstream draw tests create their colour target with a
              * transfer destination as well, so that exact attachment shape
              * exists; nothing else does. */
             usage==(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
             /* The pinned render-pass module derives its attachment usage from
              * the format's reported features, so its colour target - the one
              * the two independentBlend leaves render into - adds the sampled
              * role to that same readback shape. Served since the promotion,
              * and nothing else gains it. */
             usage==(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT) ||
             /* The pinned multiview helper's attachment: that same readback
              * shape plus an input-attachment role, and nothing else. */
             usage==(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) ||
             usage==VK_IMAGE_USAGE_TRANSFER_SRC_BIT ||
             usage==(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT)));
    }
    assert(!ps5vk_graphics_image_usage(VK_FORMAT_UNDEFINED,VK_IMAGE_USAGE_SAMPLED_BIT));
    VkInstance i = instance(); VkPhysicalDevice p = physical(i);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(p, &properties);
    assert(strcmp(properties.deviceName, "host mock, not a GPU") == 0);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(p, &memory);
    assert(memory.memoryHeapCount == 1 && memory.memoryHeaps[0].size == 65536);
    VkPhysicalDeviceFeatures features, expected_features = {0};
    expected_features.robustBufferAccess = VK_TRUE;
    memset(&features, 0xff, sizeof(features));
    vkGetPhysicalDeviceFeatures(p, &features);
    assert(!memcmp(&features, &expected_features, sizeof(expected_features)));
    VkQueueFamilyProperties queues[2];
    memset(queues, 0xa5, sizeof(queues));
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &count, NULL);
    assert(count == 1);
    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &count, queues);
    for (size_t byte = 0; byte < sizeof(queues); ++byte)
        assert(((unsigned char *)queues)[byte] == 0xa5);
    count = 2;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &count, queues);
    assert(count == 1 && queues[0].queueCount == 1 && queues[0].queueFlags == VK_QUEUE_COMPUTE_BIT);
    for (size_t byte = sizeof(queues[0]); byte < sizeof(queues); ++byte)
        assert(((unsigned char *)queues)[byte] == 0xa5);
    VkFormatProperties fp;
    memset(&fp, 0xff, sizeof(fp));
    vkGetPhysicalDeviceFormatProperties(p, VK_FORMAT_B8G8R8A8_UNORM, &fp);
    assert(!fp.linearTilingFeatures && !fp.optimalTilingFeatures && !fp.bufferFeatures);
    /* Explicit host graphics profile: no hidden GPU access or device creation. */
    p->platform.format_properties = ps5vk_graphics_format_properties;
    VkImageFormatProperties ip,zero_ip={0};
    memset(&ip,0xff,sizeof(ip));
    assert(vkGetPhysicalDeviceImageFormatProperties(p,VK_FORMAT_B8G8R8A8_UNORM,
        VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,0,&ip)
        ==VK_ERROR_FORMAT_NOT_SUPPORTED && !memcmp(&ip,&zero_ip,sizeof(ip)));
    p->platform.image_properties=ps5vk_graphics_image_properties;
    const VkFormat image_formats[]={VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_D32_SFLOAT,VK_FORMAT_R8_UNORM,VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_R8_SNORM,VK_FORMAT_R8G8_SNORM,VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        VK_FORMAT_R16_UNORM,VK_FORMAT_R16_SNORM,VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16_SNORM,VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_R16G16B16A16_UNORM,VK_FORMAT_R16G16B16A16_SNORM,
        VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,
        VK_FORMAT_R8_UINT,VK_FORMAT_R8_SINT,VK_FORMAT_R8G8_UINT,VK_FORMAT_R8G8_SINT,
        VK_FORMAT_R8G8B8A8_UINT,VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_R16_UINT,VK_FORMAT_R16_SINT,VK_FORMAT_R16G16_UINT,VK_FORMAT_R16G16_SINT,
        VK_FORMAT_R16G16B16A16_UINT,VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R32_UINT,VK_FORMAT_R32_SINT,VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32_SINT,
        VK_FORMAT_R32G32B32A32_UINT,VK_FORMAT_R32G32B32A32_SINT};
    for(unsigned f=0;f<sizeof(image_formats)/sizeof(image_formats[0]);++f)
    for(unsigned usage=0;usage<256;++usage) {
        memset(&ip,0xff,sizeof(ip));
        VkResult result=vkGetPhysicalDeviceImageFormatProperties(p,image_formats[f],
            VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&ip);
        if(ps5vk_graphics_image_usage(image_formats[f],usage)) {
            const VkBool32 storage_image_shape =
                image_formats[f]==VK_FORMAT_R32_UINT &&
                usage==(VK_IMAGE_USAGE_STORAGE_BIT|
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            const VkBool32 bounded_d32_sampled =
                image_formats[f]==VK_FORMAT_D32_SFLOAT &&
                (usage==VK_IMAGE_USAGE_SAMPLED_BIT ||
                 usage==(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT));
            assert(result==VK_SUCCESS &&
                ip.maxExtent.width==(storage_image_shape?8u:
                    bounded_d32_sampled?64u:(f==0?16383u:16384u)));
            assert(ip.maxExtent.height==ip.maxExtent.width && ip.maxExtent.depth==1);
            /* D32 sampling has its own bounded descriptor profile; other D32
             * roles use the tiled depth attachment and readback surface. */
            const VkBool32 attachment=(usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))!=0 ||
                image_formats[f]==VK_FORMAT_D32_SFLOAT ||
                (image_formats[f]==VK_FORMAT_B8G8R8A8_UNORM &&
                 usage==VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            /* The query takes the padded-linear transfer branch for any row
             * that carries the transfer-source role, which now includes the
             * integer colour target the promotion serves; the model mirrors
             * that predicate instead of naming one format. */
            const VkBool32 transfer_only=ps5vk_texture_format_has(image_formats[f],
                PS5VK_FORMAT_CAP_TRANSFER_SRC) && usage &&
                !(usage&~(VkImageUsageFlags)(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT));
            const VkBool32 multi_transfer = transfer_only &&
                ps5vk_bc_transfer_subresources(image_formats[f]);
            const uint32_t expected_mips=bounded_d32_sampled?7u:
                ((!attachment && (usage&VK_IMAGE_USAGE_SAMPLED_BIT)) ||
                 multi_transfer)?15u:1u;
            /* The one input-attachment shape the pinned multiview helper needs
             * reports the measured six-view layer floor; every other attachment
             * (and the pure transfer role) stays single-layer, so the query and
             * creation agree about exactly which shape has layers. */
            const VkBool32 input_attachment_shape =
                image_formats[f]==VK_FORMAT_R8G8B8A8_UNORM &&
                usage==(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT|
                        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
            const uint32_t expected_layers = input_attachment_shape ?
                PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR :
                (bounded_d32_sampled||attachment||storage_image_shape||
                 (transfer_only&&!multi_transfer)?1u:
                    PS5VK_MAX_IMAGE_ARRAY_LAYERS);
            assert(ip.maxMipLevels==expected_mips &&
                ip.maxArrayLayers==expected_layers &&
                ip.sampleCounts==VK_SAMPLE_COUNT_1_BIT);
            assert(ip.maxResourceSize==p->platform.max_allocation);
        } else assert(result==VK_ERROR_FORMAT_NOT_SUPPORTED && !memcmp(&ip,&zero_ip,sizeof(ip)));
    }
    assert(vkGetPhysicalDeviceImageFormatProperties(p,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TYPE_1D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_SAMPLED_BIT,0,&ip)==VK_SUCCESS &&
        ip.maxExtent.width==PS5VK_MAX_IMAGE_1D && ip.maxExtent.height==1 &&
        ip.maxExtent.depth==1 && ip.maxArrayLayers==PS5VK_MAX_IMAGE_ARRAY_LAYERS);
    assert(vkGetPhysicalDeviceImageFormatProperties(p,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TYPE_3D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_SAMPLED_BIT,0,&ip)==VK_SUCCESS &&
        ip.maxExtent.width==PS5VK_MAX_IMAGE_3D && ip.maxExtent.height==PS5VK_MAX_IMAGE_3D &&
        ip.maxExtent.depth==PS5VK_MAX_IMAGE_3D && ip.maxArrayLayers==1);
    assert(vkGetPhysicalDeviceImageFormatProperties(p,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,&ip)==VK_SUCCESS &&
        ip.maxExtent.width==PS5VK_MAX_IMAGE_CUBE && ip.maxExtent.height==PS5VK_MAX_IMAGE_CUBE &&
        ip.maxExtent.depth==1 &&
        ip.maxArrayLayers==PS5VK_MAX_IMAGE_ARRAY_LAYERS);
    /* The pinned upstream cube-array image-view leaf creates this exact
     * sampled colour-attachment shape. It remains limited to cube-compatible
     * RGBA8 images; the same usage without the cube flag and neighbouring
     * transfer combinations remain refused. */
    const VkImageUsageFlags cube_sampled_attachment_usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    assert(vkGetPhysicalDeviceImageFormatProperties(p,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,cube_sampled_attachment_usage,
        VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,&ip)==VK_SUCCESS &&
        ip.maxExtent.width==PS5VK_MAX_IMAGE_CUBE &&
        ip.maxExtent.height==PS5VK_MAX_IMAGE_CUBE && ip.maxExtent.depth==1 &&
        ip.maxMipLevels==1 &&
        ip.maxArrayLayers==PS5VK_MAX_IMAGE_ARRAY_LAYERS);
    assert(vkGetPhysicalDeviceImageFormatProperties(p,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,cube_sampled_attachment_usage,0,&ip)
        ==VK_ERROR_FORMAT_NOT_SUPPORTED && !memcmp(&ip,&zero_ip,sizeof(ip)));
    assert(vkGetPhysicalDeviceImageFormatProperties(p,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
        cube_sampled_attachment_usage|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,&ip)
        ==VK_ERROR_FORMAT_NOT_SUPPORTED && !memcmp(&ip,&zero_ip,sizeof(ip)));
    for(unsigned variant=0;variant<2;++variant) {
        memset(&ip,0xff,sizeof(ip));
        assert(vkGetPhysicalDeviceImageFormatProperties(p,
            variant==1?VK_FORMAT_UNDEFINED:VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_TYPE_2D,
            variant==0?VK_IMAGE_TILING_LINEAR:VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT,0,&ip)
            ==VK_ERROR_FORMAT_NOT_SUPPORTED && !memcmp(&ip,&zero_ip,sizeof(ip)));
    }
    /* The one linear-tiling combination this profile publishes is the pinned
     * upstream draw module's host-readback staging shape, and the query reports
     * exactly what creation accepts: one mip, one layer, one sample. */
    assert(vkGetPhysicalDeviceImageFormatProperties(p,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_LINEAR,VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,&ip)
        ==VK_SUCCESS && ip.maxExtent.width==PS5VK_MAX_IMAGE_2D &&
        ip.maxExtent.height==PS5VK_MAX_IMAGE_2D && ip.maxExtent.depth==1 &&
        ip.maxMipLevels==1 && ip.maxArrayLayers==1 &&
        ip.sampleCounts==VK_SAMPLE_COUNT_1_BIT &&
        ip.maxResourceSize==p->platform.max_allocation);
    /* Every neighbouring linear request stays refused with the output untouched. */
    {
        const struct { VkFormat format; VkImageType type; VkImageUsageFlags usage;
                       VkImageCreateFlags flags; } not_linear[] = {
            {VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0u},
            {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_1D, VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0u},
            {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_3D, VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0u},
            {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0u},
            {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0u},
            {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0u},
            {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
             VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT}};
        for (unsigned n = 0; n < sizeof(not_linear) / sizeof(not_linear[0]); ++n) {
            memset(&ip, 0xff, sizeof(ip));
            assert(vkGetPhysicalDeviceImageFormatProperties(p, not_linear[n].format,
                not_linear[n].type, VK_IMAGE_TILING_LINEAR, not_linear[n].usage,
                not_linear[n].flags, &ip) == VK_ERROR_FORMAT_NOT_SUPPORTED &&
                !memcmp(&ip, &zero_ip, sizeof(ip)));
        }
    }
    const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
        VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_R8_SNORM,VK_FORMAT_R8G8_SNORM,VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        VK_FORMAT_R16_UNORM,VK_FORMAT_R16_SNORM,VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16_SNORM,VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_R16G16B16A16_UNORM,VK_FORMAT_R16G16B16A16_SNORM,
        VK_FORMAT_R8G8B8A8_UINT,VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT};
    for (unsigned n=0; n<sizeof(formats)/sizeof(formats[0]); ++n) {
        memset(&fp, 0xff, sizeof(fp));
        vkGetPhysicalDeviceFormatProperties(p, formats[n], &fp);
        const struct ps5vk_texture_format *sampled=
            ps5vk_texture_format_lookup(formats[n]);
        VkFormatFeatureFlags buffer_bits=ps5vk_vertex_format_size(formats[n])?
            VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT:0;
        if(sampled && (sampled->witnessed & PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER))
            buffer_bits|=VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
        VkFormatFeatureFlags optimal_bits=0;
        /* The published bits come from the witnessed column only. */
        if(sampled && (sampled->witnessed & PS5VK_FORMAT_CAP_SAMPLED_IMAGE)) {
            optimal_bits=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if(sampled->witnessed & PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR)
                optimal_bits|=VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        }
        if(formats[n]==VK_FORMAT_B8G8R8A8_UNORM)
            optimal_bits=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        else if(formats[n]==VK_FORMAT_R8G8B8A8_UNORM)
            /* DXVK262-T06: the blend bit joins the advertised role, because
             * the upstream blend family gates every leaf on it and all 98
             * applicable leaves passed with it reported. */
            optimal_bits|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                VK_FORMAT_FEATURE_BLIT_DST_BIT;
        else if(formats[n]==VK_FORMAT_R8G8B8A8_SRGB)
            optimal_bits|=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                VK_FORMAT_FEATURE_BLIT_DST_BIT;
        else if(formats[n]==VK_FORMAT_D32_SFLOAT)
            /* TRANSFER_DST is the whole-subresource depth clear and
             * TRANSFER_SRC is the whole-surface readback, which 64KB_Z_X has
             * had since its pixel addressing was implemented. */
            optimal_bits=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        else if(formats[n]==VK_FORMAT_R8G8B8A8_UINT ||
                formats[n]==VK_FORMAT_R8G8B8A8_SINT)
            /* DXVK262-T06 independentBlend is promoted, so the integer colour
             * target the two upstream leaves draw into reports its colour
             * attachment role (and the transfer source its readback needs)
             * beside the sampled and transfer-destination bits above. It has no
             * blend bit: an integer target is never blended into. */
            optimal_bits|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        /* One format publishes a linear-tiling role: RGBA8 carries the transfer
         * destination of the pinned host-readback staging image. */
        const VkFormatFeatureFlags linear_bits = formats[n]==VK_FORMAT_R8G8B8A8_UNORM ?
            (VkFormatFeatureFlags)(VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                VK_FORMAT_FEATURE_BLIT_DST_BIT) : 0;
        assert(fp.linearTilingFeatures==linear_bits && fp.bufferFeatures==buffer_bits &&
            fp.optimalTilingFeatures==optimal_bits);
    }
    /* Query/create coherence: for every format the capability table knows, the
     * (format, optimal tiling, usage) answer of the image-format query must
     * agree with the feature bits the format query reports. A format whose
     * sampled encoding is implemented but not yet witnessed is reported as
     * unsupported and refused, never silently creatable. */
    const struct { VkFormat format; VkImageUsageFlags usage; VkFormatFeatureFlags bit; }
        coherence[] = {
        {VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_FORMAT_FEATURE_TRANSFER_DST_BIT},
        {VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_FORMAT_FEATURE_TRANSFER_SRC_BIT},
        {VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT},
        {VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT},
        {VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         VK_FORMAT_FEATURE_TRANSFER_DST_BIT},
        {VK_FORMAT_B8G8R8A8_UNORM,
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT},
        {VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT,
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
         VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT},
        {VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         VK_FORMAT_FEATURE_TRANSFER_DST_BIT},
        {VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_IMAGE_USAGE_SAMPLED_BIT,
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         VK_FORMAT_FEATURE_TRANSFER_DST_BIT},
        {VK_FORMAT_A8B8G8R8_SRGB_PACK32, VK_IMAGE_USAGE_SAMPLED_BIT,
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {VK_FORMAT_R32G32B32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT,
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT,
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_SAMPLED_BIT,
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
    };
    for (unsigned n = 0; n < sizeof(coherence) / sizeof(coherence[0]); ++n) {
        vkGetPhysicalDeviceFormatProperties(p, coherence[n].format, &fp);
        memset(&ip, 0xff, sizeof(ip));
        VkResult result = vkGetPhysicalDeviceImageFormatProperties(p, coherence[n].format,
            VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, coherence[n].usage, 0, &ip);
        if (fp.optimalTilingFeatures & coherence[n].bit)
            assert(result == VK_SUCCESS);
        else
            assert(result == VK_ERROR_FORMAT_NOT_SUPPORTED &&
                   !memcmp(&ip, &zero_ip, sizeof(ip)));
    }
    /* Packed rows expose sampled/upload and non-integer filtering, but no
     * unrelated render-target, storage-image or blit role. */
    VkFormatFeatureFlags packed_sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    vkGetPhysicalDeviceFormatProperties(p, VK_FORMAT_A8B8G8R8_UNORM_PACK32, &fp);
    assert(fp.optimalTilingFeatures == packed_sampled &&
        fp.bufferFeatures == (VkFormatFeatureFlags)(VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
                                                    VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT));
    vkGetPhysicalDeviceFormatProperties(p, VK_FORMAT_A8B8G8R8_SRGB_PACK32, &fp);
    assert(fp.optimalTilingFeatures == packed_sampled && !fp.bufferFeatures &&
        !fp.linearTilingFeatures);

    p->platform.queue_flags=VK_QUEUE_GRAPHICS_BIT;
    const VkFormat vertex_formats[]={VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,
        VK_FORMAT_R32G32B32_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R32_SINT,VK_FORMAT_R32G32_SINT,VK_FORMAT_R32G32B32_SINT,
        VK_FORMAT_R32G32B32A32_SINT,VK_FORMAT_R32_UINT,VK_FORMAT_R32G32_UINT,
        VK_FORMAT_R32G32B32_UINT,VK_FORMAT_R32G32B32A32_UINT,
        VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    for(unsigned n=0;n<sizeof(vertex_formats)/sizeof(vertex_formats[0]);++n) {
        vkGetPhysicalDeviceFormatProperties(p,vertex_formats[n],&fp);
        const struct ps5vk_texture_format *sampled=
            ps5vk_texture_format_lookup(vertex_formats[n]);
        VkFormatFeatureFlags expected=VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
        if(sampled && (sampled->witnessed & PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER))
            expected|=VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
        assert(fp.bufferFeatures==expected);
        /* RGBA8 is also the one linear-tiling staging row; the other vertex
         * formats publish nothing there. */
        assert(fp.linearTilingFeatures==(vertex_formats[n]==VK_FORMAT_R8G8B8A8_UNORM ?
            (VkFormatFeatureFlags)(VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                VK_FORMAT_FEATURE_BLIT_DST_BIT) : 0));
        VkFormatFeatureFlags expected_optimal=0;
        if(sampled && (sampled->witnessed & PS5VK_FORMAT_CAP_SAMPLED_IMAGE)) {
            expected_optimal=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if(sampled->witnessed & PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR)
                expected_optimal|=VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        }
        if(sampled && (sampled->witnessed & PS5VK_FORMAT_CAP_STORAGE_IMAGE))
            expected_optimal|=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if(vertex_formats[n]==VK_FORMAT_R8G8B8A8_UNORM)
            expected_optimal|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                VK_FORMAT_FEATURE_BLIT_DST_BIT;
        else if(vertex_formats[n]==VK_FORMAT_B8G8R8A8_UNORM)
            expected_optimal=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        assert(fp.optimalTilingFeatures==expected_optimal);
        assert(ps5vk_vertex_format_size(vertex_formats[n])==(n<12?4*((n%4)+1):4));
        VkResult image_result=vkGetPhysicalDeviceImageFormatProperties(p,vertex_formats[n],
            VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_SAMPLED_BIT,0,&ip);
        if(sampled && (sampled->witnessed & PS5VK_FORMAT_CAP_SAMPLED_IMAGE))
            assert(image_result==VK_SUCCESS);
        else assert(image_result==VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    const VkFormat narrow_vertex_formats[]={
        VK_FORMAT_R8_UNORM,VK_FORMAT_R8_SNORM,VK_FORMAT_R8_UINT,VK_FORMAT_R8_SINT,
        VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8_SNORM,VK_FORMAT_R8G8_UINT,VK_FORMAT_R8G8_SINT,
        VK_FORMAT_R8G8B8A8_SNORM,VK_FORMAT_R8G8B8A8_UINT,VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_A8B8G8R8_UNORM_PACK32,VK_FORMAT_A8B8G8R8_SNORM_PACK32,
        VK_FORMAT_A8B8G8R8_UINT_PACK32,VK_FORMAT_A8B8G8R8_SINT_PACK32,
        VK_FORMAT_R16_UNORM,VK_FORMAT_R16_SNORM,VK_FORMAT_R16_UINT,
        VK_FORMAT_R16_SINT,VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16_SNORM,VK_FORMAT_R16G16_UINT,
        VK_FORMAT_R16G16_SINT,VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_R16G16B16A16_UNORM,VK_FORMAT_R16G16B16A16_SNORM,
        VK_FORMAT_R16G16B16A16_UINT,VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R16G16B16A16_SFLOAT};
    for(unsigned n=0;n<sizeof(narrow_vertex_formats)/sizeof(narrow_vertex_formats[0]);++n) {
        vkGetPhysicalDeviceFormatProperties(p,narrow_vertex_formats[n],&fp);
        assert(fp.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
        assert(ps5vk_vertex_format_size(narrow_vertex_formats[n])>0);
    }
    vkGetPhysicalDeviceFormatProperties(p,VK_FORMAT_R8_USCALED,&fp);
    assert(!fp.bufferFeatures && !ps5vk_vertex_format_size(VK_FORMAT_R8_USCALED));
    count=1; vkGetPhysicalDeviceQueueFamilyProperties(p, &count, queues);
    assert(count==1 && queues[0].queueFlags==VK_QUEUE_GRAPHICS_BIT);
    p->platform.queue_flags=VK_QUEUE_COMPUTE_BIT;
    p->platform.format_properties=NULL;
    VkDeviceQueueCreateInfo q; float priority; VkDeviceCreateInfo info = device_info(&q, &priority);
    VkDevice d, other;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
    assert(vkCreateDevice(p, &info, NULL, &other) == VK_SUCCESS && d != other);
    /* T02-E1b: the multiview surface. The extension is enumerated exactly when
     * the platform capability is present, the feature and property chains answer
     * the measured truth, and the device negotiation is fail-closed without ever
     * creating a device it should not. */
    {
        /* The device under test carries whatever platform mask the surrounding
         * test installed, so the capability is set explicitly here and restored
         * afterwards - the points being tested are the reporting and the
         * negotiation, not how this fixture was built. */
        const uint32_t saved_features = p->platform.supported_features;
        p->platform.supported_features |= PS5VK_FEATURE_MULTIVIEW;
        VkExtensionProperties extensions[8]; uint32_t extension_count = 8;
        assert(vkEnumerateDeviceExtensionProperties(p, NULL, &extension_count, extensions) == VK_SUCCESS);
        int enumerated = 0;
        for (uint32_t n = 0; n < extension_count; ++n)
            if (!strcmp(extensions[n].extensionName, VK_KHR_MULTIVIEW_EXTENSION_NAME))
                enumerated = 1;
        assert(enumerated);
        /* Enumeration follows the Vulkan two-call contract: a partial buffer is
         * reported as INCOMPLETE with the count of what actually fitted, and an
         * empty query reports the full count. */
        {
            const uint32_t extra = (uint32_t)(PS5VK_FEATURE_STORAGE_BUFFER_8BIT |
                PS5VK_FEATURE_STORAGE_BUFFER_16BIT | PS5VK_FEATURE_SHADER_DRAW_PARAMETERS);
            const uint32_t with_extra = p->platform.supported_features | extra;
            p->platform.supported_features = with_extra;
            uint32_t total = 0;
            assert(vkEnumerateDeviceExtensionProperties(p, NULL, &total, NULL) == VK_SUCCESS);
            assert(total >= 5);
            uint32_t partial = 2;
            assert(vkEnumerateDeviceExtensionProperties(p, NULL, &partial, extensions) == VK_INCOMPLETE);
            assert(partial == 2);
            for (uint32_t n = 0; n < partial; ++n) assert(extensions[n].extensionName[0]);
            uint32_t none = 0;
            assert(vkEnumerateDeviceExtensionProperties(p, NULL, &none, extensions) == VK_INCOMPLETE && !none);
            p->platform.supported_features = with_extra & ~extra;
        }
        VkPhysicalDeviceMultiviewFeatures mv_features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
        VkPhysicalDeviceFeatures2 features2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &mv_features};
        vkGetPhysicalDeviceFeatures2KHR(p, &features2);
        assert(mv_features.multiview == VK_TRUE);
        assert(mv_features.multiviewGeometryShader == VK_FALSE &&
               mv_features.multiviewTessellationShader == VK_FALSE);
        VkPhysicalDeviceMultiviewProperties mv_properties = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &mv_properties};
        vkGetPhysicalDeviceProperties2KHR(p, &properties2);
        assert(mv_properties.maxMultiviewViewCount == PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR);
        assert(mv_properties.maxMultiviewInstanceIndex == PS5VK_MULTIVIEW_INSTANCE_INDEX_FLOOR);
        /* VK_KHR_maintenance2 point clipping: the Vulkan 1.0 rule, reported in
         * the same chain without disturbing the structure before it. */
        VkPhysicalDevicePointClippingProperties clipping = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES,
            .pointClippingBehavior = (VkPointClippingBehavior)0x7FFFFFFF};
        mv_properties.pNext = &clipping;
        vkGetPhysicalDeviceProperties2KHR(p, &properties2);
        assert(clipping.pointClippingBehavior == VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES);
        assert(mv_properties.maxMultiviewViewCount == PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR);
        mv_properties.pNext = NULL;
        /* Without the capability both answers are zero and the extension is not
         * enumerated at all. */
        p->platform.supported_features &= ~(uint32_t)PS5VK_FEATURE_MULTIVIEW;
        extension_count = 8;
        assert(vkEnumerateDeviceExtensionProperties(p, NULL, &extension_count, extensions) == VK_SUCCESS);
        for (uint32_t n = 0; n < extension_count; ++n)
            assert(strcmp(extensions[n].extensionName, VK_KHR_MULTIVIEW_EXTENSION_NAME));
        mv_features.multiview = VK_TRUE;
        vkGetPhysicalDeviceFeatures2KHR(p, &features2);
        assert(mv_features.multiview == VK_FALSE);
        mv_properties.maxMultiviewViewCount = 99u; mv_properties.maxMultiviewInstanceIndex = 99u;
        vkGetPhysicalDeviceProperties2KHR(p, &properties2);
        assert(!mv_properties.maxMultiviewViewCount && !mv_properties.maxMultiviewInstanceIndex);
        p->platform.supported_features = saved_features | PS5VK_FEATURE_MULTIVIEW;
        /* The extension needs the instance's properties2 dependency... */
        VkDeviceQueueCreateInfo mv_queue; float mv_priority;
        VkDeviceCreateInfo mv_info = device_info(&mv_queue, &mv_priority);
        const char *mv_names[1] = {VK_KHR_MULTIVIEW_EXTENSION_NAME};
        mv_info.enabledExtensionCount = 1; mv_info.ppEnabledExtensionNames = mv_names;
        VkDevice mv_device = VK_NULL_HANDLE;
        VkInstance plain = instance();
        VkPhysicalDevice plain_physical = physical(plain);
        assert(!plain->features2_extension_enabled);
        assert(vkCreateDevice(plain_physical, &mv_info, NULL, &mv_device) ==
               VK_ERROR_EXTENSION_NOT_PRESENT && !mv_device);
        /* A platform that does NOT carry the capability and is asked for the
         * extension is refused, and refused before anything is created. */
        {
            const uint32_t without = saved_features & ~(uint32_t)PS5VK_FEATURE_MULTIVIEW;
            /* The instance dependency is satisfied here on purpose, so the ONLY
             * reason left for the refusal is the missing platform capability. */
            const VkBool32 saved_f2 = p->instance->features2_extension_enabled;
            p->instance->features2_extension_enabled = VK_TRUE;
            p->platform.supported_features = without;
            VkDevice refused = VK_NULL_HANDLE;
            assert(vkCreateDevice(p, &mv_info, NULL, &refused) ==
                   VK_ERROR_EXTENSION_NOT_PRESENT && !refused);
            p->instance->features2_extension_enabled = saved_f2;
            p->platform.supported_features = saved_features | PS5VK_FEATURE_MULTIVIEW;
        }
        /* An unknown structure inside an output chain is ignored, as a query
         * must be: the known structures still get their answers. */
        {
            VkBaseOutStructure unknown = {.sType = (VkStructureType)0x7fffffff, .pNext = NULL};
            mv_features.multiview = VK_FALSE;
            mv_properties.maxMultiviewViewCount = 0; mv_properties.maxMultiviewInstanceIndex = 0;
            mv_features.pNext = &unknown;
            mv_properties.pNext = &unknown;
            vkGetPhysicalDeviceFeatures2KHR(p, &features2);
            assert(mv_features.multiview == VK_TRUE);
            vkGetPhysicalDeviceProperties2KHR(p, &properties2);
            assert(mv_properties.maxMultiviewViewCount == PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR &&
                   mv_properties.maxMultiviewInstanceIndex == PS5VK_MULTIVIEW_INSTANCE_INDEX_FLOOR);
            mv_features.pNext = NULL; mv_properties.pNext = NULL;
        }
        /* ...and on an instance that has it, the extension alone is enough: the
         * feature structure is optional, so the device is created with the
         * feature DISABLED and a non-zero mask would still be refused. */
        /* The dependency rule itself is what the negative above proved; for the
         * positive cases the flag is set on the device under test's own instance,
         * because this fixture enumerates one physical device whose instance is
         * the first one it was asked with. Both are restored at the end. */
        const VkBool32 saved_features2 = p->instance->features2_extension_enabled;
        p->instance->features2_extension_enabled = VK_TRUE;
        VkPhysicalDevice dependency_physical = p;
        assert(vkCreateDevice(dependency_physical, &mv_info, NULL, &mv_device) == VK_SUCCESS);
        assert(!(mv_device->enabled_features & PS5VK_FEATURE_MULTIVIEW));
        vkDestroyDevice(mv_device, NULL);
        /* Asking for the feature turns it on... */
        VkPhysicalDeviceMultiviewFeatures requested = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, .multiview = VK_TRUE};
        mv_info.pNext = &requested;
        assert(vkCreateDevice(dependency_physical, &mv_info, NULL, &mv_device) == VK_SUCCESS &&
               (mv_device->enabled_features & PS5VK_FEATURE_MULTIVIEW));
        vkDestroyDevice(mv_device, NULL);
        /* ...duplicating the structure is refused, and so is asking for it
         * without the extension, and so are the two unimplemented flavours. */
        VkPhysicalDeviceMultiviewFeatures duplicate = requested;
        requested.pNext = &duplicate;
        mv_device = VK_NULL_HANDLE;
        assert(vkCreateDevice(dependency_physical, &mv_info, NULL, &mv_device) == VK_ERROR_UNKNOWN && !mv_device);
        requested.pNext = NULL;
        VkDeviceCreateInfo no_extension = device_info(&mv_queue, &mv_priority);
        no_extension.pNext = &requested;
        mv_device = VK_NULL_HANDLE;
        assert(vkCreateDevice(dependency_physical, &no_extension, NULL, &mv_device) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !mv_device);
        requested.multiviewGeometryShader = VK_TRUE;
        mv_device = VK_NULL_HANDLE;
        assert(vkCreateDevice(p, &mv_info, NULL, &mv_device) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !mv_device);
        requested.multiviewGeometryShader = VK_FALSE;
        requested.multiviewTessellationShader = VK_TRUE;
        mv_device = VK_NULL_HANDLE;
        assert(vkCreateDevice(p, &mv_info, NULL, &mv_device) ==
               VK_ERROR_FEATURE_NOT_PRESENT && !mv_device);
        requested.multiviewTessellationShader = VK_FALSE;
        requested.multiview = 7u;   /* not a boolean */
        mv_device = VK_NULL_HANDLE;
        assert(vkCreateDevice(dependency_physical, &mv_info, NULL, &mv_device) == VK_ERROR_UNKNOWN && !mv_device);
        requested.multiview = VK_TRUE;
        /* The CTS builds one device chain for every rendering type it
         * exercises, so a multiview case always chains
         * VkPhysicalDeviceDynamicRenderingFeatures behind the multiview
         * structure with the feature left at its default false. That neutral
         * shape must create the device; the feature itself stays unimplemented,
         * so a true request fails closed and the extension stays unadvertised. */
        {
            VkPhysicalDeviceDynamicRenderingFeatures dynamic = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
                .pNext = NULL,
                .dynamicRendering = VK_FALSE};
            const unsigned devices_before = p->instance->devices;
            unsigned opens_before = opened;
            requested.pNext = &dynamic;
            assert(vkCreateDevice(dependency_physical, &mv_info, NULL, &mv_device) == VK_SUCCESS);
            assert(opened == opens_before + 1 && p->instance->devices == devices_before + 1);
            assert(mv_device->enabled_features & PS5VK_FEATURE_MULTIVIEW);
            vkDestroyDevice(mv_device, NULL);
            assert(p->instance->devices == devices_before);
            /* Asking for the feature is refused with the precise
             * unsupported-feature result, before a backend is opened or a
             * device is published. */
            dynamic.dynamicRendering = VK_TRUE;
            mv_device = VK_NULL_HANDLE; opens_before = opened;
            assert(vkCreateDevice(dependency_physical, &mv_info, NULL, &mv_device) ==
                   VK_ERROR_FEATURE_NOT_PRESENT && !mv_device && opened == opens_before &&
                   p->instance->devices == devices_before);
            /* Not a boolean at all. */
            dynamic.dynamicRendering = 2u;
            mv_device = VK_NULL_HANDLE; opens_before = opened;
            assert(vkCreateDevice(dependency_physical, &mv_info, NULL, &mv_device) ==
                   VK_ERROR_UNKNOWN && !mv_device && opened == opens_before &&
                   p->instance->devices == devices_before);
            /* One structure only, whatever value it carries. */
            dynamic.dynamicRendering = VK_FALSE;
            VkPhysicalDeviceDynamicRenderingFeatures dynamic_copy = dynamic;
            dynamic.pNext = &dynamic_copy;
            mv_device = VK_NULL_HANDLE; opens_before = opened;
            assert(vkCreateDevice(dependency_physical, &mv_info, NULL, &mv_device) ==
                   VK_ERROR_UNKNOWN && !mv_device && opened == opens_before &&
                   p->instance->devices == devices_before);
            dynamic.pNext = NULL;
            /* The recognised structure is the only thing that changed: dynamic
             * rendering stays unadvertised, and an input structure this profile
             * does not implement is still refused rather than ignored. */
            {
                VkExtensionProperties names[8]; uint32_t count = 8;
                assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, names) == VK_SUCCESS);
                for (uint32_t n = 0; n < count; ++n)
                    assert(strcmp(names[n].extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME));
            }
            VkBaseInStructure unhandled = {.sType = (VkStructureType)0x7ffffffe, .pNext = NULL};
            requested.pNext = &unhandled;
            mv_device = VK_NULL_HANDLE; opens_before = opened;
            assert(vkCreateDevice(dependency_physical, &mv_info, NULL, &mv_device) ==
                   VK_ERROR_FEATURE_NOT_PRESENT && !mv_device && opened == opens_before &&
                   p->instance->devices == devices_before);
            requested.pNext = NULL;
        }
        p->instance->features2_extension_enabled = saved_features2;
        vkDestroyInstance(plain, NULL);
    }
    assert(vkGetInstanceProcAddr(NULL, "vkCreateInstance") == (PFN_vkVoidFunction)vkCreateInstance);
    assert(!vkGetInstanceProcAddr(NULL, "vkCreateDevice"));
    assert(vkGetInstanceProcAddr(i, "vkCreateDevice") == (PFN_vkVoidFunction)vkCreateDevice);
    assert(vkGetInstanceProcAddr(i, "vkGetPhysicalDeviceFormatProperties") == (PFN_vkVoidFunction)vkGetPhysicalDeviceFormatProperties);
    assert(!vkGetInstanceProcAddr(NULL, "vkGetPhysicalDeviceFormatProperties"));
    PFN_vkGetPhysicalDeviceImageFormatProperties image_query=(PFN_vkGetPhysicalDeviceImageFormatProperties)
        vkGetInstanceProcAddr(i,"vkGetPhysicalDeviceImageFormatProperties");
    assert(image_query==vkGetPhysicalDeviceImageFormatProperties);
    assert(!vkGetInstanceProcAddr(NULL,"vkGetPhysicalDeviceImageFormatProperties"));
    assert(image_query(p,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,&ip)==VK_SUCCESS);
    assert(vkGetDeviceProcAddr(d, "vkMapMemory") == (PFN_vkVoidFunction)vkMapMemory);
    assert(!vkGetDeviceProcAddr(d, "vkCreateInstance"));
    assert(!vkGetDeviceProcAddr(d, "vkGetPhysicalDeviceFormatProperties"));
    assert(!vkGetDeviceProcAddr(d,"vkGetPhysicalDeviceImageFormatProperties"));
    assert(vkGetDeviceProcAddr(d, "vkCmdDispatch") == (PFN_vkVoidFunction)vkCmdDispatch);
    assert(!vkGetDeviceProcAddr(NULL, "vkMapMemory"));
    assert(!vkGetInstanceProcAddr(i, "vkMadeUpExtension") && !vkGetInstanceProcAddr(i, NULL));
    /* graphics profile discovery: real function identity, both valid lookup routes, and scope. */
#define CHECK_GRAPHICS(name) do { \
    assert(vkGetInstanceProcAddr(i, #name) == (PFN_vkVoidFunction)name); \
    assert(vkGetDeviceProcAddr(d, #name) == (PFN_vkVoidFunction)name); \
    assert(!vkGetInstanceProcAddr(NULL, #name)); \
    assert(!vkGetDeviceProcAddr(NULL, #name)); \
} while (0)
    CHECK_GRAPHICS(vkCreateImage);
    CHECK_GRAPHICS(vkDestroyImage);
    CHECK_GRAPHICS(vkGetImageMemoryRequirements);
    CHECK_GRAPHICS(vkBindImageMemory);
    CHECK_GRAPHICS(vkCreateImageView);
    CHECK_GRAPHICS(vkDestroyImageView);
    CHECK_GRAPHICS(vkCreateSampler);
    CHECK_GRAPHICS(vkDestroySampler);
    CHECK_GRAPHICS(vkCreateRenderPass);
    CHECK_GRAPHICS(vkDestroyRenderPass);
    CHECK_GRAPHICS(vkCreateFramebuffer);
    CHECK_GRAPHICS(vkDestroyFramebuffer);
    CHECK_GRAPHICS(vkCreateGraphicsPipelines);
    CHECK_GRAPHICS(vkCmdBeginRenderPass);
    CHECK_GRAPHICS(vkCmdEndRenderPass);
    CHECK_GRAPHICS(vkCmdBindVertexBuffers);
    CHECK_GRAPHICS(vkCmdBindIndexBuffer);
    CHECK_GRAPHICS(vkCmdDraw);
    CHECK_GRAPHICS(vkCmdDrawIndexed);
    CHECK_GRAPHICS(vkCmdCopyImage);
    CHECK_GRAPHICS(vkCmdBlitImage);
    CHECK_GRAPHICS(vkCmdResolveImage);
    CHECK_GRAPHICS(vkCmdClearColorImage);
    CHECK_GRAPHICS(vkCmdClearDepthStencilImage);
    CHECK_GRAPHICS(vkCmdClearAttachments);
    CHECK_GRAPHICS(vkCmdCopyBufferToImage);
    CHECK_GRAPHICS(vkCmdCopyImageToBuffer);
    CHECK_GRAPHICS(vkCmdSetLineWidth);
    CHECK_GRAPHICS(vkCmdSetDepthBias);
    CHECK_GRAPHICS(vkCmdSetBlendConstants);
    CHECK_GRAPHICS(vkCmdSetDepthBounds);
    CHECK_GRAPHICS(vkCmdSetStencilCompareMask);
    CHECK_GRAPHICS(vkCmdSetStencilWriteMask);
    CHECK_GRAPHICS(vkCmdSetStencilReference);
    CHECK_GRAPHICS(vkGetQueryPoolResults);
    CHECK_GRAPHICS(vkCmdResetQueryPool);
    CHECK_GRAPHICS(vkCmdBeginQuery);
    CHECK_GRAPHICS(vkCmdEndQuery);
    CHECK_GRAPHICS(vkCmdWriteTimestamp);
    CHECK_GRAPHICS(vkCmdCopyQueryPoolResults);
    CHECK_GRAPHICS(vkCmdNextSubpass);
    CHECK_GRAPHICS(vkCmdExecuteCommands);
    CHECK_GRAPHICS(vkQueueBindSparse);
#undef CHECK_GRAPHICS
    /* Vulkan 1.0 bookkeeping discovery: identity, scopes, and availability */
#define CHECK_BOOKKEEPING_DEV(name) do { \
    assert(vkGetInstanceProcAddr(i, #name) == (PFN_vkVoidFunction)name); \
    assert(vkGetDeviceProcAddr(d, #name) == (PFN_vkVoidFunction)name); \
    assert(!vkGetInstanceProcAddr(NULL, #name)); \
    assert(!vkGetDeviceProcAddr(NULL, #name)); \
} while (0)
    CHECK_BOOKKEEPING_DEV(vkGetImageSubresourceLayout);
    CHECK_BOOKKEEPING_DEV(vkGetRenderAreaGranularity);
    CHECK_BOOKKEEPING_DEV(vkGetDeviceMemoryCommitment);
    CHECK_BOOKKEEPING_DEV(vkResetDescriptorPool);
#undef CHECK_BOOKKEEPING_DEV
    assert(vkGetInstanceProcAddr(i, "vkEnumerateDeviceLayerProperties") == (PFN_vkVoidFunction)vkEnumerateDeviceLayerProperties);
    assert(!vkGetDeviceProcAddr(d, "vkEnumerateDeviceLayerProperties"));
    assert(!vkGetInstanceProcAddr(NULL, "vkEnumerateDeviceLayerProperties"));

    uint32_t layer_count = 10;
    assert(vkEnumerateDeviceLayerProperties(NULL, &layer_count, NULL) == VK_ERROR_UNKNOWN);
    assert(vkEnumerateDeviceLayerProperties(p, NULL, NULL) == VK_ERROR_UNKNOWN);
    assert(vkEnumerateDeviceLayerProperties(p, &layer_count, NULL) == VK_SUCCESS && layer_count == 0);
    VkLayerProperties layer_props[2];
    layer_count = 2;
    assert(vkEnumerateDeviceLayerProperties(p, &layer_count, layer_props) == VK_SUCCESS && layer_count == 0);

    assert(!vkGetDeviceProcAddr(d, "vkCreateSwapchainKHR"));
    assert(!vkGetDeviceProcAddr(d, "vkGetPhysicalDeviceProperties"));
    PFN_vkCreateSampler create_sampler = (PFN_vkCreateSampler)vkGetDeviceProcAddr(d, "vkCreateSampler");
    VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler sampler = VK_NULL_HANDLE;
    /* Resolving the entry point must not silently enable unsupported graphics. */
    assert(create_sampler(d, &sampler_info, NULL, &sampler) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(sampler == VK_NULL_HANDLE);
    assert(i->devices == 2);
    VkQueue queue, second;
    vkGetDeviceQueue(d, 0, 0, &queue); vkGetDeviceQueue(d, 0, 0, &second);
    assert(queue && second == queue && queue->device == d);
    vkGetDeviceQueue(d, 1, 0, &second); assert(!second);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 256, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buffer; assert(vkCreateBuffer(d, &bi, NULL, &buffer) == VK_SUCCESS);
    vkDestroyDevice(d, NULL); assert(d->lifetime_errors == 1 && i->devices == 2);
    vkDestroyInstance(i, NULL); assert(i->lifetime_errors == 1);
    vkDestroyBuffer(d, buffer, NULL);
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 1024};
    VkDeviceMemory m; assert(vkAllocateMemory(d, &mi, NULL, &m) == VK_SUCCESS);
    vkDestroyDevice(d, NULL); assert(d->lifetime_errors == 2);
    vkFreeMemory(d, m, NULL);
    VkDescriptorSetLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout layout;
    assert(vkCreateDescriptorSetLayout(d, &layout_info, NULL, &layout) == VK_SUCCESS);
    assert(vkGetDeviceProcAddr(d, "vkUpdateDescriptorSets") == (PFN_vkVoidFunction)vkUpdateDescriptorSets);
    vkDestroyDevice(d, NULL); assert(d->lifetime_errors == 3);
    vkDestroyDescriptorSetLayout(d, layout, NULL);
    queue->next_serial = 2; /* Synthetic pending state, no submit is emulated. */
    vkDestroyDevice(d, NULL); assert(d->lifetime_errors == 4);
    queue->completed_serial = 1;
    vkDestroyDevice(d, NULL); vkDestroyDevice(other, NULL);
    assert(!i->devices && opened == closed); vkDestroyInstance(i, NULL);
}
static void negative(void)
{
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; VkInstance i;
    query_result = VK_ERROR_INITIALIZATION_FAILED;
    assert(vkCreateInstance(&ii, NULL, &i) == query_result && !i); query_result = VK_SUCCESS;
    malformed = 1;
    assert(vkCreateInstance(&ii, NULL, &i) == VK_ERROR_INITIALIZATION_FAILED && !i); malformed = 0;
    for (malformed = 3; malformed <= 7; ++malformed)
        assert(vkCreateInstance(&ii, NULL, &i) == VK_ERROR_INITIALIZATION_FAILED && !i);
    malformed = 0;
    ii.enabledLayerCount = 1;
    assert(vkCreateInstance(&ii, NULL, &i) == VK_ERROR_LAYER_NOT_PRESENT); ii.enabledLayerCount = 0;
    ii.enabledExtensionCount = 1;
    assert(vkCreateInstance(&ii, NULL, &i) == VK_ERROR_UNKNOWN);
    const char *unknown_instance_extension = "VK_EXT_not_real";
    ii.ppEnabledExtensionNames = &unknown_instance_extension;
    assert(vkCreateInstance(&ii, NULL, &i) == VK_ERROR_EXTENSION_NOT_PRESENT);
    ii.enabledExtensionCount = 0; ii.ppEnabledExtensionNames = NULL;
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pNext = &ai};
    ii.pApplicationInfo = &ai;
    assert(vkCreateInstance(&ii, NULL, &i) == VK_ERROR_UNKNOWN && !i); ii.pApplicationInfo = NULL;
    i = instance(); VkPhysicalDevice p = physical(i);
    VkDeviceQueueCreateInfo q; float priority; VkDeviceCreateInfo info = device_info(&q, &priority);
    VkDevice d; unsigned before = opened;
    info.enabledExtensionCount = 1;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_UNKNOWN && !d);
    const char *unknown_device_extension = "VK_EXT_not_real";
    info.ppEnabledExtensionNames = &unknown_device_extension;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    info.enabledExtensionCount = 0; info.ppEnabledExtensionNames = NULL;
    VkPhysicalDeviceFeatures features = {.shaderInt64 = VK_TRUE}; info.pEnabledFeatures = &features;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_FEATURE_NOT_PRESENT);
    /* robustBufferAccess is the one supported Vulkan 1.0 core feature.  Every
     * other field must reject before a backend/device is opened. */
    _Static_assert(sizeof(features)%sizeof(VkBool32)==0,"feature word layout");
    for(size_t offset=0;offset<sizeof(features);offset+=sizeof(VkBool32)) {
        memset(&features,0,sizeof(features));
        const VkBool32 enabled=VK_TRUE;
        memcpy((unsigned char *)&features+offset,&enabled,sizeof(enabled));
        d=(VkDevice)(uintptr_t)1;
        if (offset == offsetof(VkPhysicalDeviceFeatures, robustBufferAccess)) {
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
            assert(d->enabled_features == PS5VK_FEATURE_ROBUST_BUFFER_ACCESS);
            vkDestroyDevice(d, NULL);
            before = opened;
        } else {
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT);
            assert(!d && opened==before && !i->devices);
        }
    }
    memset(&features, 0, sizeof(features));
    features.robustBufferAccess = 2;
    d=(VkDevice)(uintptr_t)1;
    assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);
    features.robustBufferAccess = VK_TRUE;
    p->platform.supported_features &= ~PS5VK_FEATURE_ROBUST_BUFFER_ACCESS;
    d=(VkDevice)(uintptr_t)1;
    assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
    p->platform.supported_features |= PS5VK_FEATURE_ROBUST_BUFFER_ACCESS;
    /* The three DXVK262-T03 core features: each is reported and enabled only
     * behind its own platform bit, independently of the other two, through the
     * same core-feature path; the enabled set records exactly the requested
     * bits and an out-of-range VkBool32 is still refused. */
    {
        const struct { size_t offset; uint32_t bit; } t03[3] = {
            {offsetof(VkPhysicalDeviceFeatures, drawIndirectFirstInstance),
             PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE},
            {offsetof(VkPhysicalDeviceFeatures, multiDrawIndirect),
             PS5VK_FEATURE_MULTI_DRAW_INDIRECT},
            {offsetof(VkPhysicalDeviceFeatures, fullDrawIndexUint32),
             PS5VK_FEATURE_FULL_DRAW_INDEX_UINT32},
        };
        const uint32_t saved = p->platform.supported_features;
        for (unsigned n = 0; n < 3; ++n) {
            p->platform.supported_features = saved | t03[n].bit;
            VkPhysicalDeviceFeatures reported, expected = {0};
            expected.robustBufferAccess = VK_TRUE;
            const VkBool32 yes = VK_TRUE;
            memcpy((unsigned char *)&expected + t03[n].offset, &yes, sizeof(yes));
            vkGetPhysicalDeviceFeatures(p, &reported);
            assert(!memcmp(&reported, &expected, sizeof(expected)));
            /* Enabling the reported member alone records exactly its bit. */
            memset(&features, 0, sizeof(features));
            memcpy((unsigned char *)&features + t03[n].offset, &yes, sizeof(yes));
            d=(VkDevice)(uintptr_t)1;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
            assert(d->enabled_features == t03[n].bit);
            vkDestroyDevice(d, NULL);
            /* Together with robustBufferAccess both bits are recorded. */
            features.robustBufferAccess = VK_TRUE;
            d=(VkDevice)(uintptr_t)1;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
            assert(d->enabled_features == (PS5VK_FEATURE_ROBUST_BUFFER_ACCESS | t03[n].bit));
            vkDestroyDevice(d, NULL);
            /* The other two stay unreported and unenableable. */
            for (unsigned other = 0; other < 3; ++other) {
                if (other == n) continue;
                memset(&features, 0, sizeof(features));
                memcpy((unsigned char *)&features + t03[other].offset, &yes, sizeof(yes));
                d=(VkDevice)(uintptr_t)1;
                assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
            }
            /* A non-boolean request is invalid before the bit is consulted. */
            memset(&features, 0, sizeof(features));
            const VkBool32 bad = 2;
            memcpy((unsigned char *)&features + t03[n].offset, &bad, sizeof(bad));
            d=(VkDevice)(uintptr_t)1;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);
            /* Without the bit the member is false again and refused. */
            p->platform.supported_features = saved;
            vkGetPhysicalDeviceFeatures(p, &reported);
            memset(&expected, 0, sizeof(expected));
            expected.robustBufferAccess = VK_TRUE;
            assert(!memcmp(&reported, &expected, sizeof(expected)));
            memset(&features, 0, sizeof(features));
            memcpy((unsigned char *)&features + t03[n].offset, &yes, sizeof(yes));
            d=(VkDevice)(uintptr_t)1;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
        }
        /* All three at once through VkPhysicalDeviceFeatures2, the chain the
         * public consumer uses, record all three bits. */
        p->platform.supported_features = saved | t03[0].bit | t03[1].bit | t03[2].bit;
        VkPhysicalDeviceFeatures2 all = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        vkGetPhysicalDeviceFeatures2KHR(p, &all);
        assert(all.features.robustBufferAccess && all.features.multiDrawIndirect &&
               all.features.drawIndirectFirstInstance && all.features.fullDrawIndexUint32);
        VkDeviceCreateInfo chained = info;
        chained.pEnabledFeatures = NULL; chained.pNext = &all;
        d=(VkDevice)(uintptr_t)1;
        assert(vkCreateDevice(p,&chained,NULL,&d)==VK_SUCCESS && d);
        assert(d->enabled_features == (PS5VK_FEATURE_ROBUST_BUFFER_ACCESS | t03[0].bit |
                                       t03[1].bit | t03[2].bit));
        vkDestroyDevice(d, NULL);
        p->platform.supported_features = saved;
        memset(&features, 0, sizeof(features));
        features.robustBufferAccess = VK_TRUE;
    }
    /* The four DXVK262-T05 core features go through the same table: each is
     * reported and enabled only behind its own platform bit; without the bit
     * the member is false and refused; the full T05 mask enables all four at
     * once and records exactly those bits. The host platform sets none of
     * them, so the default report stays false for all four. */
    {
        const struct { size_t offset; uint32_t bit; } t05[4] = {
            {offsetof(VkPhysicalDeviceFeatures, depthBiasClamp), PS5VK_FEATURE_DEPTH_BIAS_CLAMP},
            {offsetof(VkPhysicalDeviceFeatures, depthClamp), PS5VK_FEATURE_DEPTH_CLAMP},
            {offsetof(VkPhysicalDeviceFeatures, fillModeNonSolid), PS5VK_FEATURE_FILL_MODE_NON_SOLID},
            {offsetof(VkPhysicalDeviceFeatures, multiViewport), PS5VK_FEATURE_MULTI_VIEWPORT},
        };
        const uint32_t saved = p->platform.supported_features;
        const VkBool32 yes = VK_TRUE;
        VkPhysicalDeviceFeatures reported;
        vkGetPhysicalDeviceFeatures(p, &reported);
        assert(!reported.depthBiasClamp && !reported.depthClamp &&
               !reported.fillModeNonSolid && !reported.multiViewport);
        uint32_t all_bits = 0;
        for (unsigned n = 0; n < 4; ++n) {
            all_bits |= t05[n].bit;
            /* Requested without the bit: refused before a device opens. */
            memset(&features, 0, sizeof(features));
            memcpy((unsigned char *)&features + t05[n].offset, &yes, sizeof(yes));
            d=(VkDevice)(uintptr_t)1;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
            /* With exactly this bit: reported alone among the four, enabled
             * alone, and the other three still refused. */
            p->platform.supported_features = saved | t05[n].bit;
            vkGetPhysicalDeviceFeatures(p, &reported);
            for (unsigned other = 0; other < 4; ++other) {
                VkBool32 value;
                memcpy(&value, (unsigned char *)&reported + t05[other].offset, sizeof(value));
                assert(value == (other == n ? VK_TRUE : VK_FALSE));
                if (other == n) continue;
                memset(&features, 0, sizeof(features));
                memcpy((unsigned char *)&features + t05[other].offset, &yes, sizeof(yes));
                d=(VkDevice)(uintptr_t)1;
                assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
            }
            memset(&features, 0, sizeof(features));
            memcpy((unsigned char *)&features + t05[n].offset, &yes, sizeof(yes));
            d=(VkDevice)(uintptr_t)1;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
            assert(d->enabled_features == t05[n].bit);
            vkDestroyDevice(d, NULL);
            p->platform.supported_features = saved;
        }
        /* The whole T05 mask through VkPhysicalDeviceFeatures2, as the pinned
         * CTS enables every reported feature: all four bits recorded. */
        p->platform.supported_features = saved | all_bits;
        VkPhysicalDeviceFeatures2 all = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        vkGetPhysicalDeviceFeatures2KHR(p, &all);
        assert(all.features.depthBiasClamp && all.features.depthClamp &&
               all.features.fillModeNonSolid && all.features.multiViewport);
        VkDeviceCreateInfo chained = info;
        chained.pEnabledFeatures = NULL; chained.pNext = &all;
        d=(VkDevice)(uintptr_t)1;
        assert(vkCreateDevice(p,&chained,NULL,&d)==VK_SUCCESS && d);
        assert(d->enabled_features == (PS5VK_FEATURE_ROBUST_BUFFER_ACCESS | all_bits));
        vkDestroyDevice(d, NULL);
        p->platform.supported_features = saved;
        memset(&features, 0, sizeof(features));
        features.robustBufferAccess = VK_TRUE;
    }
    /* T06 follows the same one-member/one-platform-bit contract. This test
     * deliberately supplies the bits only through the host fixture: the host
     * and PS5 platform defaults advertise none until native evidence promotes
     * an individual capability. */
    {
        const struct { size_t offset; uint32_t bit; } t06[4] = {
            {offsetof(VkPhysicalDeviceFeatures, independentBlend),
             PS5VK_FEATURE_INDEPENDENT_BLEND},
            {offsetof(VkPhysicalDeviceFeatures, dualSrcBlend),
             PS5VK_FEATURE_DUAL_SRC_BLEND},
            {offsetof(VkPhysicalDeviceFeatures, fragmentStoresAndAtomics),
             PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS},
            {offsetof(VkPhysicalDeviceFeatures, sampleRateShading),
             PS5VK_FEATURE_SAMPLE_RATE_SHADING},
        };
        const uint32_t saved = p->platform.supported_features;
        const VkBool32 yes = VK_TRUE;
        VkPhysicalDeviceFeatures reported;
        vkGetPhysicalDeviceFeatures(p, &reported);
        assert(!reported.independentBlend && !reported.dualSrcBlend &&
               !reported.fragmentStoresAndAtomics && !reported.sampleRateShading);
        uint32_t all_bits = 0;
        for (unsigned n = 0; n < 4; ++n) {
            all_bits |= t06[n].bit;
            memset(&features, 0, sizeof(features));
            memcpy((unsigned char *)&features + t06[n].offset, &yes, sizeof(yes));
            d=(VkDevice)(uintptr_t)1;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);

            p->platform.supported_features = saved | t06[n].bit;
            vkGetPhysicalDeviceFeatures(p, &reported);
            for (unsigned other = 0; other < 4; ++other) {
                VkBool32 value;
                memcpy(&value, (unsigned char *)&reported + t06[other].offset, sizeof(value));
                assert(value == (other == n ? VK_TRUE : VK_FALSE));
            }
            memset(&features, 0, sizeof(features));
            memcpy((unsigned char *)&features + t06[n].offset, &yes, sizeof(yes));
            d=(VkDevice)(uintptr_t)1;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
            assert(d->enabled_features == t06[n].bit);
            vkDestroyDevice(d, NULL);
            p->platform.supported_features = saved;
        }

        p->platform.supported_features = saved | all_bits;
        VkPhysicalDeviceFeatures2 all = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        vkGetPhysicalDeviceFeatures2KHR(p, &all);
        assert(all.features.independentBlend && all.features.dualSrcBlend &&
               all.features.fragmentStoresAndAtomics && all.features.sampleRateShading);
        VkDeviceCreateInfo chained = info;
        chained.pEnabledFeatures = NULL; chained.pNext = &all;
        d=(VkDevice)(uintptr_t)1;
        assert(vkCreateDevice(p,&chained,NULL,&d)==VK_SUCCESS && d);
        assert(d->enabled_features == (PS5VK_FEATURE_ROBUST_BUFFER_ACCESS | all_bits));
        vkDestroyDevice(d, NULL);
        p->platform.supported_features = saved;
        memset(&features, 0, sizeof(features));
        features.robustBufferAccess = VK_TRUE;
    }
    /* Every T07 core feature uses the same one-member/one-platform bit
     * contract. This host fixture starts with the bits off and turns each on
     * to prove the query and logical-device routes independently of the
     * native platform's reporting decision. */
    {
        const struct { size_t offset; uint32_t bit; } t07[] = {
            {offsetof(VkPhysicalDeviceFeatures, imageCubeArray),
             PS5VK_FEATURE_IMAGE_CUBE_ARRAY},
            {offsetof(VkPhysicalDeviceFeatures, textureCompressionBC),
             PS5VK_FEATURE_TEXTURE_COMPRESSION_BC},
            {offsetof(VkPhysicalDeviceFeatures, shaderImageGatherExtended),
             PS5VK_FEATURE_SHADER_IMAGE_GATHER_EXTENDED},
            {offsetof(VkPhysicalDeviceFeatures, occlusionQueryPrecise),
             PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE},
        };
        const unsigned t07_count = sizeof(t07) / sizeof(t07[0]);
        const uint32_t saved = p->platform.supported_features;
        const VkBool32 yes = VK_TRUE;
        VkPhysicalDeviceFeatures reported;
        vkGetPhysicalDeviceFeatures(p, &reported);
        assert(!reported.imageCubeArray && !reported.textureCompressionBC &&
               !reported.shaderImageGatherExtended && !reported.occlusionQueryPrecise);
        uint32_t all_bits = 0;
        for (unsigned n = 0; n < t07_count; ++n) {
            all_bits |= t07[n].bit;
            memset(&features, 0, sizeof(features));
            memcpy((unsigned char *)&features + t07[n].offset, &yes, sizeof(yes));
            d=(VkDevice)(uintptr_t)1;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);

            p->platform.supported_features = saved | t07[n].bit;
            vkGetPhysicalDeviceFeatures(p, &reported);
            for (unsigned other = 0; other < t07_count; ++other) {
                VkBool32 value;
                memcpy(&value, (unsigned char *)&reported + t07[other].offset,
                       sizeof(value));
                assert(value == (other == n ? VK_TRUE : VK_FALSE));
            }
            memset(&features, 0, sizeof(features));
            memcpy((unsigned char *)&features + t07[n].offset, &yes, sizeof(yes));
            d=(VkDevice)(uintptr_t)1;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
            assert(d->enabled_features == t07[n].bit);
            vkDestroyDevice(d, NULL);
            p->platform.supported_features = saved;
        }

        p->platform.supported_features = saved | all_bits;
        VkPhysicalDeviceFeatures2 all = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2
        };
        vkGetPhysicalDeviceFeatures2KHR(p, &all);
        assert(all.features.imageCubeArray && all.features.textureCompressionBC &&
               all.features.shaderImageGatherExtended &&
               all.features.occlusionQueryPrecise);
        VkDeviceCreateInfo chained = info;
        chained.pEnabledFeatures = NULL;
        chained.pNext = &all;
        d=(VkDevice)(uintptr_t)1;
        assert(vkCreateDevice(p,&chained,NULL,&d)==VK_SUCCESS && d);
        assert(d->enabled_features == (PS5VK_FEATURE_ROBUST_BUFFER_ACCESS | all_bits));
        vkDestroyDevice(d, NULL);
        p->platform.supported_features = saved;
        memset(&features, 0, sizeof(features));
        features.robustBufferAccess = VK_TRUE;
    }
    info.pEnabledFeatures = NULL; priority = NAN;
    assert(vkCreateDevice(p, &info, NULL, &d) != VK_SUCCESS);
    priority = 0.0f;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS &&
           d->queue.priority_class == 0);
    vkDestroyDevice(d, NULL);
    priority = 0.499f;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS &&
           d->queue.priority_class == 0);
    vkDestroyDevice(d, NULL);
    priority = 0.5f;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS &&
           d->queue.priority_class == 1);
    vkDestroyDevice(d, NULL);
    priority = 1.0f;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS &&
           d->queue.priority_class == 1);
    vkDestroyDevice(d, NULL);
    priority = -0.001f;
    assert(vkCreateDevice(p, &info, NULL, &d) != VK_SUCCESS && !d);
    priority = 1.001f;
    assert(vkCreateDevice(p, &info, NULL, &d) != VK_SUCCESS && !d);
    priority = 1.0f;
    before = opened;
    q.queueFamilyIndex = 1; assert(vkCreateDevice(p, &info, NULL, &d) != VK_SUCCESS);
    q.queueFamilyIndex = 0; assert(opened == before);
    open_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(vkCreateDevice(p, &info, NULL, &d) == open_result && !d); open_result = VK_SUCCESS;
    malformed = 2;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_INITIALIZATION_FAILED && !d);
    malformed = 0; assert(opened == closed && !i->devices);
    vkDestroyInstance(i, NULL);
}
static void narrow_storage_features(void)
{
    uint32_t count = 0;
    assert(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) == VK_SUCCESS && count == 4);
    VkExtensionProperties instance_properties[4] = {0};
    count = 0;
    assert(vkEnumerateInstanceExtensionProperties(NULL, &count, instance_properties) == VK_INCOMPLETE && count == 0);
    count = 4;
    assert(vkEnumerateInstanceExtensionProperties(NULL, &count, instance_properties) == VK_SUCCESS && count == 4);
    assert(!strcmp(instance_properties[0].extensionName,
                   VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME));
    assert(instance_properties[0].specVersion == VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION);
    assert(!strcmp(instance_properties[1].extensionName,
                   VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME));
    assert(instance_properties[1].specVersion == VK_KHR_DEVICE_GROUP_CREATION_SPEC_VERSION);
    assert(!strcmp(instance_properties[2].extensionName, VK_KHR_SURFACE_EXTENSION_NAME));
    assert(!strcmp(instance_properties[3].extensionName, VK_KHR_DISPLAY_EXTENSION_NAME));
    assert(vkEnumerateInstanceExtensionProperties("layer", &count, NULL) == VK_ERROR_LAYER_NOT_PRESENT);
    assert(vkEnumerateInstanceExtensionProperties(NULL, NULL, NULL) == VK_ERROR_UNKNOWN);

    VkInstance plain = instance();
    const char *gpdp2_commands[] = {
        "vkGetPhysicalDeviceFeatures2KHR",
        "vkGetPhysicalDeviceProperties2KHR",
        "vkGetPhysicalDeviceFormatProperties2KHR",
        "vkGetPhysicalDeviceImageFormatProperties2KHR",
        "vkGetPhysicalDeviceQueueFamilyProperties2KHR",
        "vkGetPhysicalDeviceMemoryProperties2KHR",
        "vkGetPhysicalDeviceSparseImageFormatProperties2KHR",
    };
    for (size_t n = 0; n < sizeof(gpdp2_commands) / sizeof(gpdp2_commands[0]); ++n) {
        assert(!vkGetInstanceProcAddr(plain, gpdp2_commands[n]));
        assert(!vkGetInstanceProcAddr(NULL, gpdp2_commands[n]));
    }
    vkDestroyInstance(plain, NULL);

    VkInstance i = features2_instance();
    VkPhysicalDevice p = physical(i);
    for (size_t n = 0; n < sizeof(gpdp2_commands) / sizeof(gpdp2_commands[0]); ++n)
        assert(vkGetInstanceProcAddr(i, gpdp2_commands[n]));
    assert(vkGetInstanceProcAddr(i, "vkGetPhysicalDeviceFeatures2KHR") ==
           (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures2KHR);
    p->platform.supported_features = PS5VK_FEATURE_STORAGE_BUFFER_8BIT |
                                     PS5VK_FEATURE_STORAGE_BUFFER_16BIT |
                                     PS5VK_FEATURE_ROBUST_BUFFER_ACCESS |
                                     PS5VK_FEATURE_SHADER_DRAW_PARAMETERS;
    p->platform.format_properties = ps5vk_graphics_format_properties;
    p->platform.image_properties = ps5vk_graphics_image_properties;

    VkBaseOutStructure query_unknown = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM};
    VkPhysicalDeviceProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &query_unknown};
    vkGetPhysicalDeviceProperties2KHR(p, &properties2);
    assert(properties2.properties.apiVersion == VK_API_VERSION_1_0);
    assert(query_unknown.sType == VK_STRUCTURE_TYPE_MAX_ENUM && !query_unknown.pNext);

    VkPhysicalDeviceProperties2 wrong_properties2;
    memset(&wrong_properties2, 0xa5, sizeof(wrong_properties2));
    wrong_properties2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    VkPhysicalDeviceProperties2 saved_wrong_properties2 = wrong_properties2;
    vkGetPhysicalDeviceProperties2KHR(p, &wrong_properties2);
    assert(!memcmp(&wrong_properties2, &saved_wrong_properties2,
                   sizeof(wrong_properties2)));

    VkPhysicalDeviceMemoryProperties2 memory2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
        .pNext = &query_unknown};
    vkGetPhysicalDeviceMemoryProperties2KHR(p, &memory2);
    assert(memory2.memoryProperties.memoryTypeCount == 1);
    assert(memory2.memoryProperties.memoryHeaps[0].flags ==
           VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
    assert(memory2.memoryProperties.memoryTypes[0].propertyFlags ==
           (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));
    VkPhysicalDeviceMemoryProperties2 wrong_memory2;
    memset(&wrong_memory2, 0xa5, sizeof(wrong_memory2));
    wrong_memory2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    VkPhysicalDeviceMemoryProperties2 saved_wrong_memory2 = wrong_memory2;
    vkGetPhysicalDeviceMemoryProperties2KHR(p, &wrong_memory2);
    assert(!memcmp(&wrong_memory2, &saved_wrong_memory2, sizeof(wrong_memory2)));

    VkFormatProperties2 format2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
                                   .pNext = &query_unknown};
    vkGetPhysicalDeviceFormatProperties2KHR(p, VK_FORMAT_R32_UINT, &format2);
    assert(format2.formatProperties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
    VkFormatProperties2 wrong_format2;
    memset(&wrong_format2, 0xa5, sizeof(wrong_format2));
    wrong_format2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    VkFormatProperties2 saved_wrong_format2 = wrong_format2;
    vkGetPhysicalDeviceFormatProperties2KHR(p, VK_FORMAT_R32_UINT, &wrong_format2);
    assert(!memcmp(&wrong_format2, &saved_wrong_format2, sizeof(wrong_format2)));

    VkQueueFamilyProperties2 queue2[2];
    memset(queue2, 0xa5, sizeof(queue2));
    queue2[0].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    queue2[0].pNext = &query_unknown;
    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2KHR(p, &count, queue2);
    assert(count == 0);
    for (size_t byte = offsetof(VkQueueFamilyProperties2, pNext) + sizeof(void *);
         byte < sizeof(queue2); ++byte)
        assert(((unsigned char *)queue2)[byte] == 0xa5);
    count = 2;
    vkGetPhysicalDeviceQueueFamilyProperties2KHR(p, &count, queue2);
    assert(count == 1 && queue2[0].queueFamilyProperties.queueCount == 1);
    assert(queue2[0].pNext == &query_unknown);
    for (size_t byte = sizeof(queue2[0]); byte < sizeof(queue2); ++byte)
        assert(((unsigned char *)queue2)[byte] == 0xa5);

    VkPhysicalDeviceImageFormatInfo2 image_info2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = VK_FORMAT_B8G8R8A8_UNORM, .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkImageFormatProperties2 image2 = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
                                       .pNext = &query_unknown};
    assert(vkGetPhysicalDeviceImageFormatProperties2KHR(p, &image_info2, &image2) == VK_SUCCESS);
    assert(image2.imageFormatProperties.maxExtent.width);
    VkImageFormatProperties2 wrong_image2;
    memset(&wrong_image2, 0xa5, sizeof(wrong_image2));
    wrong_image2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    VkImageFormatProperties2 saved_wrong_image2 = wrong_image2;
    assert(vkGetPhysicalDeviceImageFormatProperties2KHR(
        p, &image_info2, &wrong_image2) == VK_ERROR_UNKNOWN);
    assert(!memcmp(&wrong_image2, &saved_wrong_image2, sizeof(wrong_image2)));
    VkPhysicalDeviceImageFormatInfo2 supported_srgb_info2 = image_info2;
    supported_srgb_info2.format = VK_FORMAT_R8G8B8A8_SRGB;
    memset(&image2.imageFormatProperties, 0xa5,
           sizeof(image2.imageFormatProperties));
    supported_srgb_info2.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    assert(vkGetPhysicalDeviceImageFormatProperties2KHR(
        p, &supported_srgb_info2, &image2) == VK_SUCCESS);
    assert(image2.imageFormatProperties.maxExtent.width == PS5VK_MAX_IMAGE_2D);

    VkPhysicalDeviceSparseImageFormatInfo2 sparse_info2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2,
        .format = VK_FORMAT_B8G8R8A8_UNORM, .type = VK_IMAGE_TYPE_2D,
        .samples = VK_SAMPLE_COUNT_1_BIT, .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL};
    count = 99;
    vkGetPhysicalDeviceSparseImageFormatProperties2KHR(p, &sparse_info2, &count, NULL);
    assert(count == 0);

    VkExtensionProperties device_properties[4] = {0};
    count = 0;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS && count == 4);
    count = 2;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, device_properties) == VK_INCOMPLETE && count == 2);
    count = 4;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, device_properties) == VK_SUCCESS && count == 4);
    assert(!strcmp(device_properties[0].extensionName, VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME));
    assert(!strcmp(device_properties[1].extensionName, VK_KHR_8BIT_STORAGE_EXTENSION_NAME));
    assert(!strcmp(device_properties[2].extensionName, VK_KHR_16BIT_STORAGE_EXTENSION_NAME));
    assert(!strcmp(device_properties[3].extensionName, VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME));

    VkBaseOutStructure unknown = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM};
    VkPhysicalDeviceShaderDrawParametersFeatures shader_draw_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,
        .pNext = &unknown, .shaderDrawParameters = VK_TRUE};
    VkPhysicalDeviceProtectedMemoryFeatures protected_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,
        .pNext = &shader_draw_features, .protectedMemory = VK_TRUE};
    VkPhysicalDevice16BitStorageFeatures feature16 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
        .pNext = &protected_features, .storagePushConstant16 = VK_TRUE,
        .storageInputOutput16 = VK_TRUE};
    VkPhysicalDevice8BitStorageFeatures feature8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,
        .pNext = &feature16, .uniformAndStorageBuffer8BitAccess = VK_TRUE,
        .storagePushConstant8 = VK_TRUE};
    VkPhysicalDeviceFeatures2 feature_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &feature8};
    vkGetPhysicalDeviceFeatures2KHR(p, &feature_query);
    VkPhysicalDeviceFeatures expected = {0};
    expected.robustBufferAccess = VK_TRUE;
    assert(!memcmp(&feature_query.features, &expected, sizeof(expected)));
    assert(feature8.storageBuffer8BitAccess && !feature8.uniformAndStorageBuffer8BitAccess &&
           !feature8.storagePushConstant8);
    assert(feature16.storageBuffer16BitAccess && !feature16.uniformAndStorageBuffer16BitAccess &&
           !feature16.storagePushConstant16 && !feature16.storageInputOutput16);
    assert(!protected_features.protectedMemory && shader_draw_features.shaderDrawParameters);
    assert(unknown.sType == VK_STRUCTURE_TYPE_MAX_ENUM && !unknown.pNext);

    const char *extensions[] = {
        VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME,
        VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
        VK_KHR_16BIT_STORAGE_EXTENSION_NAME};
    VkDeviceQueueCreateInfo queue_info; float priority;
    VkDeviceCreateInfo info = device_info(&queue_info, &priority);
    info.enabledExtensionCount = 3; info.ppEnabledExtensionNames = extensions;
    feature8.pNext = &feature16;
    feature8.storageBuffer8BitAccess = VK_TRUE;
    protected_features.pNext = &shader_draw_features;
    shader_draw_features.pNext = NULL;
    /* The query above reported the feature the profile now supports; this
     * creation is the narrow-storage one and deliberately does not request it,
     * because the extension that exposes it is not enabled here. */
    shader_draw_features.shaderDrawParameters = VK_FALSE;
    feature16.pNext = &protected_features;
    feature16.storageBuffer16BitAccess = VK_TRUE;
    feature_query.pNext = &feature8;
    info.pNext = &feature_query;
    VkDevice device = VK_NULL_HANDLE;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_SUCCESS);
    assert(device->enabled_features == (PS5VK_FEATURE_STORAGE_BUFFER_8BIT |
                                        PS5VK_FEATURE_STORAGE_BUFFER_16BIT |
                                        PS5VK_FEATURE_ROBUST_BUFFER_ACCESS));
    vkDestroyDevice(device, NULL);

    protected_features.protectedMemory = VK_TRUE;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    protected_features.protectedMemory = 2;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_UNKNOWN && !device);
    protected_features.protectedMemory = VK_FALSE;
    shader_draw_features.shaderDrawParameters = VK_TRUE;
    /* The extension is what exposes the 1.1 feature on this Vulkan 1.0
     * profile, so a true request without it stays fail-closed. */
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    const char *draw_parameter_extensions[] = {
        VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME,
        VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
        VK_KHR_16BIT_STORAGE_EXTENSION_NAME,
        VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME};
    info.enabledExtensionCount = 4;
    info.ppEnabledExtensionNames = draw_parameter_extensions;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_SUCCESS);
    assert(device->enabled_features == (PS5VK_FEATURE_STORAGE_BUFFER_8BIT |
                                        PS5VK_FEATURE_STORAGE_BUFFER_16BIT |
                                        PS5VK_FEATURE_ROBUST_BUFFER_ACCESS |
                                        PS5VK_FEATURE_SHADER_DRAW_PARAMETERS));
    vkDestroyDevice(device, NULL);
    /* A second ShaderDrawParametersFeatures structure in the same chain is
     * rejected whatever the two carry. The duplicate rule is a property of the
     * chain, so it cannot depend on the requested value: FALSE+FALSE,
     * FALSE+TRUE and TRUE+FALSE are all invalid, exactly like TRUE+TRUE. */
    {
        VkPhysicalDeviceShaderDrawParametersFeatures duplicate = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,
            .pNext = NULL, .shaderDrawParameters = VK_TRUE};
        shader_draw_features.pNext = &duplicate;
        for (unsigned variant = 0; variant < 4; ++variant) {
            const VkBool32 first = (variant >= 2) ? VK_TRUE : VK_FALSE;
            const VkBool32 second = (variant % 2) ? VK_TRUE : VK_FALSE;
            shader_draw_features.shaderDrawParameters = first;
            duplicate.shaderDrawParameters = second;
            assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_UNKNOWN && !device);
        }
        /* The same request through one structure still succeeds, so the
         * rejection above is the duplicate and not the value. */
        shader_draw_features.pNext = NULL;
        shader_draw_features.shaderDrawParameters = VK_TRUE;
        assert(vkCreateDevice(p, &info, NULL, &device) == VK_SUCCESS);
        assert(device->enabled_features & PS5VK_FEATURE_SHADER_DRAW_PARAMETERS);
        vkDestroyDevice(device, NULL);
    }
    info.enabledExtensionCount = 3;
    info.ppEnabledExtensionNames = extensions;
    shader_draw_features.shaderDrawParameters = 2;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_UNKNOWN && !device);
    shader_draw_features.shaderDrawParameters = VK_FALSE;

    info.enabledExtensionCount = 2;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    info.enabledExtensionCount = 3;
    feature8.uniformAndStorageBuffer8BitAccess = VK_TRUE;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    feature8.uniformAndStorageBuffer8BitAccess = VK_FALSE;
    feature16.storageBuffer16BitAccess = 2;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_UNKNOWN && !device);
    feature16.storageBuffer16BitAccess = VK_TRUE;
    info.ppEnabledExtensionNames = NULL;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_UNKNOWN && !device);

    VkInstance no_gpdp2 = instance();
    VkPhysicalDevice no_gpdp2_physical = physical(no_gpdp2);
    no_gpdp2_physical->platform.supported_features = p->platform.supported_features;
    info.ppEnabledExtensionNames = extensions;
    assert(vkCreateDevice(no_gpdp2_physical, &info, NULL, &device) ==
           VK_ERROR_EXTENSION_NOT_PRESENT && !device);
    vkDestroyInstance(no_gpdp2, NULL);
    vkDestroyInstance(i, NULL);
}
struct allocation_counts { unsigned live, instance, device, object; int fail; };
static void *VKAPI_CALL host_allocate(void *ctx, size_t size, size_t align,
                                      VkSystemAllocationScope scope)
{
    struct allocation_counts *c = ctx;
    assert(align <= _Alignof(max_align_t));
    if (c->fail) return NULL;
    if (scope == VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE) ++c->instance;
    else if (scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE) ++c->device;
    else { assert(scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT); ++c->object; }
    void *p = malloc(size); assert(p); ++c->live; return p;
}
static void *VKAPI_CALL host_reallocate(void *ctx, void *p, size_t size, size_t align,
                                        VkSystemAllocationScope scope)
{ (void)ctx; (void)align; (void)scope; return realloc(p, size); }
static void VKAPI_CALL host_free(void *ctx, void *p)
{ struct allocation_counts *c = ctx; assert(c->live); --c->live; free(p); }
static void allocator_lifetimes(void)
{
    struct allocation_counts c = {0};
    VkAllocationCallbacks a = {.pUserData = &c, .pfnAllocation = host_allocate,
        .pfnReallocation = host_reallocate, .pfnFree = host_free};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; VkInstance i;
    query_result = VK_ERROR_INITIALIZATION_FAILED;
    assert(vkCreateInstance(&ii, &a, &i) == query_result && !c.live);
    query_result = VK_SUCCESS;
    assert(vkCreateInstance(&ii, &a, &i) == VK_SUCCESS && c.live == 1);
    VkPhysicalDevice p = physical(i);
    VkDeviceQueueCreateInfo q; float priority; VkDeviceCreateInfo info = device_info(&q, &priority);
    VkDevice d;
    open_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(vkCreateDevice(p, &info, NULL, &d) == open_result && c.live == 1);
    open_result = VK_SUCCESS; c.fail = 1;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_OUT_OF_HOST_MEMORY && c.live == 1);
    c.fail = 0;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS && c.live == 2);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 256, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer b; assert(vkCreateBuffer(d, &bi, NULL, &b) == VK_SUCCESS && c.live == 3);
    vkDestroyBuffer(d, b, NULL); vkDestroyDevice(d, NULL); vkDestroyInstance(i, &a);
    assert(!c.live && c.instance == 2 && c.device == 2 && c.object == 1);
}
static void stale_consumer_formats(VkFormat format, VkFormatProperties *out)
{
    ps5vk_graphics_format_properties(format, out);
    if (format == VK_FORMAT_R8G8B8A8_UNORM)
        out->bufferFeatures &= ~VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}
static void consumer_physical_queries(void)
{
    VkInstance i = features2_instance();
    VkPhysicalDevice p = physical(i);
    ps5vk_device_profile_init(&p->platform.properties,
                             &p->platform.memory_properties, 1, 1,
                             p->platform.supported_features);
    p->platform.queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    p->platform.max_allocation = UINT64_C(268435456);
    p->platform.format_properties = ps5vk_graphics_format_properties;
    p->platform.image_properties = ps5vk_graphics_image_properties;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    report_physical_device_contract(i, p, &props);

    /* A lost advertised role must fail the exact native consumer contract,
     * not silently regenerate its oracle from the implementation. */
    p->platform.format_properties = stale_consumer_formats;
    expect_consumer_rejection = 1;
    consumer_failed_contract = NULL;
    if (setjmp(consumer_rejection) == 0) {
        report_physical_device_contract(i, p, &props);
        assert(!"consumer accepted a stale format report");
    }
    assert(consumer_failed_contract &&
           !strcmp(consumer_failed_contract, "exact format-property matrix"));
    expect_consumer_rejection = 0;
    vkDestroyInstance(i, NULL);
}
static void tessellation_feature_negotiation(void)
{
    VkInstance i = features2_instance();
    VkPhysicalDevice p = physical(i);
    const uint32_t baseline = p->platform.supported_features;
    assert(!(baseline & PS5VK_FEATURE_TESSELLATION_SHADER));
    VkDeviceQueueCreateInfo q;
    float priority;
    VkDeviceCreateInfo info = device_info(&q, &priority);
    for (unsigned supported = 0; supported != 2; ++supported) {
        p->platform.supported_features = baseline |
            (supported ? PS5VK_FEATURE_TESSELLATION_SHADER : 0);
        VkPhysicalDeviceFeatures reported;
        vkGetPhysicalDeviceFeatures(p, &reported);
        assert(reported.tessellationShader == supported);
        VkPhysicalDeviceFeatures2 reported2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        vkGetPhysicalDeviceFeatures2KHR(p, &reported2);
        assert(!memcmp(&reported, &reported2.features, sizeof(reported)));
        for (unsigned chained = 0; chained != 2; ++chained) {
            for (VkBool32 requested = 0; requested != 3; ++requested) {
                VkPhysicalDeviceFeatures2 features = {
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                    .features.tessellationShader = requested};
                info.pEnabledFeatures = chained ? NULL : &features.features;
                info.pNext = chained ? &features : NULL;
                const unsigned before = opened;
                VkDevice d = (VkDevice)(uintptr_t)1;
                VkResult expected = requested == 2 ? VK_ERROR_UNKNOWN :
                    (requested && !supported ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS);
                assert(vkCreateDevice(p, &info, NULL, &d) == expected);
                if (expected == VK_SUCCESS) {
                    assert(d && d->enabled_features ==
                        (requested ? PS5VK_FEATURE_TESSELLATION_SHADER : 0));
                    vkDestroyDevice(d, NULL);
                } else {
                    assert(!d && opened == before && !i->devices);
                }
            }
        }
    }
    p->platform.supported_features = baseline;
    vkDestroyInstance(i, NULL);
}

static void shader_int16_core_route(void)
{
    VkInstance instance_with_features2 = features2_instance();
    VkPhysicalDevice p = physical(instance_with_features2);
    assert(!(p->platform.supported_features & PS5VK_FEATURE_SHADER_INT16));
    VkPhysicalDeviceFeatures reported;
    vkGetPhysicalDeviceFeatures(p, &reported);
    assert(!reported.shaderInt16);
    VkDeviceQueueCreateInfo queue; float priority;
    VkDeviceCreateInfo info = device_info(&queue, &priority);
    VkPhysicalDeviceFeatures requested = {.shaderInt16 = VK_TRUE};
    info.pEnabledFeatures = &requested;
    const unsigned before = opened;
    VkDevice device = VK_NULL_HANDLE;
    assert(vkCreateDevice(p, &info, NULL, &device) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !device && opened == before);

    /* Host-only capable-platform simulation; the shipping platform mask is
     * unchanged until native and original CTS evidence justify promotion. */
    p->platform.supported_features |= PS5VK_FEATURE_SHADER_INT16;
    vkGetPhysicalDeviceFeatures(p, &reported);
    assert(reported.shaderInt16);
    VkPhysicalDeviceFeatures2 reported2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2KHR(p, &reported2);
    assert(reported2.features.shaderInt16);
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_SUCCESS);
    assert(device->enabled_features & PS5VK_FEATURE_SHADER_INT16);
    vkDestroyDevice(device, NULL);

    VkPhysicalDeviceFeatures2 chained = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .features.shaderInt16 = VK_TRUE};
    info.pEnabledFeatures = NULL;
    info.pNext = &chained;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_SUCCESS);
    assert(device->enabled_features & PS5VK_FEATURE_SHADER_INT16);
    vkDestroyDevice(device, NULL);
    chained.features.shaderInt16 = VK_FALSE;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_SUCCESS);
    assert(!(device->enabled_features & PS5VK_FEATURE_SHADER_INT16));
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance_with_features2, NULL);
}

static void unadvertised_subgroup_properties(void)
{
    VkInstance i = features2_instance();
    VkPhysicalDevice p = physical(i);
    assert(VK_API_VERSION_MAJOR(p->platform.properties.apiVersion) == 1);
    assert(VK_API_VERSION_MINOR(p->platform.properties.apiVersion) == 0);
    VkPhysicalDeviceSubgroupProperties subgroup = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
        .subgroupSize = 99u,
        .supportedStages = VK_SHADER_STAGE_ALL,
        .supportedOperations = VK_SUBGROUP_FEATURE_BALLOT_BIT,
        .quadOperationsInAllStages = VK_TRUE};
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &subgroup};
    vkGetPhysicalDeviceProperties2KHR(p, &properties);
    assert(!subgroup.subgroupSize && !subgroup.supportedStages &&
           !subgroup.supportedOperations && !subgroup.quadOperationsInAllStages);
    assert(properties.properties.apiVersion == VK_API_VERSION_1_0);
    vkDestroyInstance(i, NULL);
}

static void memory_model_feature_negotiation(void)
{
    VkInstance i = features2_instance();
    VkPhysicalDevice p = physical(i);
    const uint32_t baseline = p->platform.supported_features;
    const char *extension = VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME;
    VkPhysicalDeviceVulkanMemoryModelFeaturesKHR reported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &reported};
    vkGetPhysicalDeviceFeatures2KHR(p, &features2);
    assert(!reported.vulkanMemoryModel && !reported.vulkanMemoryModelDeviceScope &&
           !reported.vulkanMemoryModelAvailabilityVisibilityChains);

    VkDeviceQueueCreateInfo q;
    float priority;
    VkDeviceCreateInfo info = device_info(&q, &priority);
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = &extension;
    VkDevice d = VK_NULL_HANDLE;
    const unsigned before = opened;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT);
    assert(!d && opened == before);

    p->platform.supported_features = baseline | PS5VK_FEATURE_VULKAN_MEMORY_MODEL;
    vkGetPhysicalDeviceFeatures2KHR(p, &features2);
    assert(reported.vulkanMemoryModel && !reported.vulkanMemoryModelDeviceScope &&
           !reported.vulkanMemoryModelAvailabilityVisibilityChains);
    uint32_t count = 0;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS);
    VkExtensionProperties properties[8];
    memset(properties, 0, sizeof(properties));
    assert(count <= 8);
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, properties) == VK_SUCCESS);
    int found = 0;
    for (uint32_t n = 0; n < count; ++n)
        found |= !strcmp(properties[n].extensionName, extension);
    assert(found);

    VkPhysicalDeviceVulkanMemoryModelFeaturesKHR requested = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES_KHR};
    info.pNext = &requested;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
    assert(d && !(d->enabled_features & PS5VK_FEATURE_VULKAN_MEMORY_MODEL));
    vkDestroyDevice(d, NULL);
    requested.vulkanMemoryModel = VK_TRUE;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
    assert(d && (d->enabled_features & PS5VK_FEATURE_VULKAN_MEMORY_MODEL) &&
           !(d->enabled_features & PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE));
    vkDestroyDevice(d, NULL);
    requested.vulkanMemoryModelDeviceScope = VK_TRUE;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    p->platform.supported_features |= PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE;
    vkGetPhysicalDeviceFeatures2KHR(p, &features2);
    assert(reported.vulkanMemoryModel && reported.vulkanMemoryModelDeviceScope);
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
    assert(d && (d->enabled_features & PS5VK_FEATURE_VULKAN_MEMORY_MODEL) &&
           (d->enabled_features & PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE));
    vkDestroyDevice(d, NULL);

    requested.vulkanMemoryModel = VK_FALSE;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    requested.vulkanMemoryModel = VK_TRUE;
    requested.vulkanMemoryModelDeviceScope = VK_FALSE;
    requested.vulkanMemoryModelAvailabilityVisibilityChains = VK_TRUE;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    requested.vulkanMemoryModelAvailabilityVisibilityChains = 2;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_UNKNOWN && !d);
    requested.vulkanMemoryModelAvailabilityVisibilityChains = VK_FALSE;
    requested.pNext = &reported;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_UNKNOWN && !d);
    requested.pNext = NULL;
    info.enabledExtensionCount = 0;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    p->platform.supported_features = baseline;
    vkDestroyInstance(i, NULL);

    i = instance();
    p = physical(i);
    p->platform.supported_features |= PS5VK_FEATURE_VULKAN_MEMORY_MODEL;
    info = device_info(&q, &priority);
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = &extension;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);
}

static void single_device_group_creation(void)
{
    const char *extension = VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME;
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
    VkInstance i = VK_NULL_HANDLE;
    assert(vkCreateInstance(&info, NULL, &i) == VK_SUCCESS);
    assert(i->device_group_creation_enabled && !i->features2_extension_enabled);
    assert(vkGetInstanceProcAddr(i, "vkEnumeratePhysicalDeviceGroupsKHR"));
    assert(!vkGetInstanceProcAddr(NULL, "vkEnumeratePhysicalDeviceGroupsKHR"));
    uint32_t count = 0;
    assert(vkEnumeratePhysicalDeviceGroupsKHR(i, &count, NULL) == VK_SUCCESS && count == 1);
    VkPhysicalDeviceGroupProperties groups[2] = {
        {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES},
        {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES}};
    count = 0;
    assert(vkEnumeratePhysicalDeviceGroupsKHR(i, &count, groups) == VK_INCOMPLETE && !count);
    count = 2;
    assert(vkEnumeratePhysicalDeviceGroupsKHR(i, &count, groups) == VK_SUCCESS && count == 1);
    assert(groups[0].physicalDeviceCount == 1 &&
           groups[0].physicalDevices[0] == physical(i) &&
           !groups[0].subsetAllocation);
    assert(groups[1].physicalDeviceCount == 0);

    VkDeviceQueueCreateInfo q; float priority;
    VkDeviceCreateInfo device = device_info(&q, &priority);
    VkPhysicalDevice member = physical(i);
    VkDeviceGroupDeviceCreateInfo group = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO,
        .physicalDeviceCount = 1, .pPhysicalDevices = &member};
    device.pNext = &group;
    VkDevice d = VK_NULL_HANDLE;
    assert(vkCreateDevice(member, &device, NULL, &d) == VK_SUCCESS);
    vkDestroyDevice(d, NULL);
    group.physicalDeviceCount = 2;
    assert(vkCreateDevice(member, &device, NULL, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    group.physicalDeviceCount = 1;
    VkPhysicalDevice wrong = (VkPhysicalDevice)(uintptr_t)1;
    group.pPhysicalDevices = &wrong;
    assert(vkCreateDevice(member, &device, NULL, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    group.pPhysicalDevices = &member;
    VkDeviceGroupDeviceCreateInfo duplicate = group;
    group.pNext = &duplicate;
    assert(vkCreateDevice(member, &device, NULL, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);

    i = instance();
    assert(!vkGetInstanceProcAddr(i, "vkEnumeratePhysicalDeviceGroupsKHR"));
    count = 0;
    assert(vkEnumeratePhysicalDeviceGroupsKHR(i, &count, NULL) == VK_ERROR_UNKNOWN);
    vkDestroyInstance(i, NULL);
}

/* A Vulkan 1.1 instance accepts every requested apiVersion (DXVK 2.6.2 asks
 * for 1.3) and resolves the core 1.1 instance- and physical-device-level names.
 * The physical device keeps reporting Vulkan 1.0 and the device-level core 1.1
 * names stay absent. */
static void vulkan11_instance_version(void)
{
    uint32_t version = 0;
    PFN_vkEnumerateInstanceVersion enumerate_version = (PFN_vkEnumerateInstanceVersion)
        vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    assert(enumerate_version == vkEnumerateInstanceVersion);
    assert(enumerate_version(&version) == VK_SUCCESS && version == VK_API_VERSION_1_1);
    assert(enumerate_version(NULL) == VK_ERROR_UNKNOWN);
    assert(!vkGetInstanceProcAddr(NULL, "vkEnumeratePhysicalDeviceGroups"));

    const uint32_t requests[] = {
        VK_API_VERSION_1_1, VK_API_VERSION_1_2, VK_API_VERSION_1_3,
        VK_MAKE_API_VERSION(0, 1, 3, 204), VK_MAKE_API_VERSION(0, 2, 0, 0)};
    for (size_t n = 0; n < sizeof(requests) / sizeof(requests[0]); ++n) {
        VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .apiVersion = requests[n]};
        VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app};
        VkInstance i = VK_NULL_HANDLE;
        assert(vkCreateInstance(&info, NULL, &i) == VK_SUCCESS && i);
        assert(i->api_version == VK_API_VERSION_1_1);
        VkPhysicalDevice p = physical(i);
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(p, &properties);
        assert(properties.apiVersion == VK_API_VERSION_1_0);
        assert(vkGetInstanceProcAddr(i, "vkEnumeratePhysicalDeviceGroups") ==
               (PFN_vkVoidFunction)vkEnumeratePhysicalDeviceGroups);
        assert(!vkGetInstanceProcAddr(i, "vkEnumeratePhysicalDeviceGroupsKHR"));
        assert(!vkGetInstanceProcAddr(i, "vkGetPhysicalDeviceFeatures2KHR"));
        assert(vkGetInstanceProcAddr(i, "vkGetPhysicalDeviceFeatures2") ==
               (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures2);
        assert(vkGetInstanceProcAddr(i, "vkGetPhysicalDeviceExternalSemaphoreProperties") ==
               (PFN_vkVoidFunction)vkGetPhysicalDeviceExternalSemaphoreProperties);
        /* Device-level core 1.1 names stay absent from both lookups. */
        assert(!vkGetInstanceProcAddr(i, "vkTrimCommandPool"));
        assert(!vkGetInstanceProcAddr(i, "vkGetDeviceQueue2"));
        assert(!vkGetInstanceProcAddr(i, "vkBindBufferMemory2"));
        /* The core query names answer exactly as the KHR route does. */
        VkPhysicalDeviceFeatures2 core = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceFeatures2 khr = core;
        vkGetPhysicalDeviceFeatures2(p, &core);
        vkGetPhysicalDeviceFeatures2KHR(p, &khr);
        assert(!memcmp(&core.features, &khr.features, sizeof(core.features)));
        VkPhysicalDeviceProperties2 properties2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        vkGetPhysicalDeviceProperties2(p, &properties2);
        assert(properties2.properties.apiVersion == VK_API_VERSION_1_0);
        VkQueueFamilyProperties2 family = {.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2};
        uint32_t families = 1;
        vkGetPhysicalDeviceQueueFamilyProperties2(p, &families, &family);
        assert(families == 1 && family.queueFamilyProperties.queueCount == 1);
        VkPhysicalDeviceExternalBufferInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        VkExternalBufferProperties buffer_external = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
            .externalMemoryProperties = {1, 1, 1}};
        vkGetPhysicalDeviceExternalBufferProperties(p, &buffer_info, &buffer_external);
        assert(!buffer_external.externalMemoryProperties.externalMemoryFeatures &&
               !buffer_external.externalMemoryProperties.exportFromImportedHandleTypes &&
               !buffer_external.externalMemoryProperties.compatibleHandleTypes);
        VkPhysicalDeviceExternalFenceInfo fence_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
            .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        VkExternalFenceProperties fence_external = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES,
            .exportFromImportedHandleTypes = 1, .compatibleHandleTypes = 1,
            .externalFenceFeatures = 1};
        vkGetPhysicalDeviceExternalFenceProperties(p, &fence_info, &fence_external);
        assert(!fence_external.exportFromImportedHandleTypes &&
               !fence_external.compatibleHandleTypes && !fence_external.externalFenceFeatures);
        VkPhysicalDeviceExternalSemaphoreInfo semaphore_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        VkExternalSemaphoreProperties semaphore_external = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
            .exportFromImportedHandleTypes = 1, .compatibleHandleTypes = 1,
            .externalSemaphoreFeatures = 1};
        vkGetPhysicalDeviceExternalSemaphoreProperties(p, &semaphore_info, &semaphore_external);
        assert(!semaphore_external.exportFromImportedHandleTypes &&
               !semaphore_external.compatibleHandleTypes &&
               !semaphore_external.externalSemaphoreFeatures);
        uint32_t count = 0;
        assert(vkEnumeratePhysicalDeviceGroups(i, &count, NULL) == VK_SUCCESS && count == 1);
        VkPhysicalDeviceGroupProperties group = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES};
        assert(vkEnumeratePhysicalDeviceGroups(i, &count, &group) == VK_SUCCESS &&
               count == 1 && group.physicalDeviceCount == 1 &&
               group.physicalDevices[0] == p && !group.subsetAllocation);
        assert(vkEnumeratePhysicalDeviceGroupsKHR(i, &count, NULL) == VK_ERROR_UNKNOWN);
        vkDestroyInstance(i, NULL);
    }

    const uint32_t legacy[] = {0, VK_API_VERSION_1_0, VK_MAKE_API_VERSION(0, 1, 0, 300),
                               VK_MAKE_API_VERSION(0, 0, 9, 0)};
    for (size_t n = 0; n < sizeof(legacy) / sizeof(legacy[0]); ++n) {
        VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .apiVersion = legacy[n]};
        VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app};
        VkInstance i = VK_NULL_HANDLE;
        assert(vkCreateInstance(&info, NULL, &i) == VK_SUCCESS);
        assert(i->api_version == VK_API_VERSION_1_0);
        assert(!vkGetInstanceProcAddr(i, "vkEnumeratePhysicalDeviceGroups"));
        assert(!vkGetInstanceProcAddr(i, "vkGetPhysicalDeviceFeatures2"));
        assert(!vkGetInstanceProcAddr(i, "vkGetPhysicalDeviceExternalFenceProperties"));
        uint32_t count = 0;
        assert(vkEnumeratePhysicalDeviceGroups(i, &count, NULL) == VK_ERROR_UNKNOWN);
        vkDestroyInstance(i, NULL);
    }
    /* A non-zero variant is incompatible with this Vulkan implementation. */
    const uint32_t variants[] = {VK_MAKE_API_VERSION(1, 1, 0, 0), VK_MAKE_API_VERSION(7, 1, 3, 0),
                                 VK_MAKE_API_VERSION(1, 1, 0, 0) & ~0x1fffffffu};
    for (size_t n = 0; n < sizeof(variants) / sizeof(variants[0]); ++n) {
        VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .apiVersion = variants[n]};
        VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app};
        VkInstance i = (VkInstance)(uintptr_t)1;
        assert(vkCreateInstance(&info, NULL, &i) == VK_ERROR_INCOMPATIBLE_DRIVER && !i);
    }
    VkInstance i = instance();
    assert(i->api_version == VK_API_VERSION_1_0);
    vkDestroyInstance(i, NULL);
}

static void buffer_address_command_gate(void)
{
    struct VkDevice_T d = {0};
    const char *commands[] = {
        "vkGetBufferDeviceAddressKHR",
        "vkGetBufferOpaqueCaptureAddressKHR",
        "vkGetDeviceMemoryOpaqueCaptureAddressKHR",
    };
    for (size_t n = 0; n < sizeof(commands) / sizeof(commands[0]); ++n)
        assert(!vkGetDeviceProcAddr(&d, commands[n]));
    d.enabled_features = PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS;
    for (size_t n = 0; n < sizeof(commands) / sizeof(commands[0]); ++n)
        assert(vkGetDeviceProcAddr(&d, commands[n]));
    /* This device still reports Vulkan 1.0: no core-1.2 command alias. */
    assert(!vkGetDeviceProcAddr(&d, "vkGetBufferDeviceAddress"));
}

static void device_group_dispatch_command_gate(void)
{
    struct VkDevice_T d = {0};
    assert(!vkGetDeviceProcAddr(&d, "vkCmdDispatchBaseKHR"));
    assert(!vkGetDeviceProcAddr(&d, "vkCmdSetDeviceMaskKHR"));
    assert(!vkGetDeviceProcAddr(&d, "vkGetDeviceGroupPeerMemoryFeaturesKHR"));
    d.device_group_extension_enabled = VK_TRUE;
    assert(vkGetDeviceProcAddr(&d, "vkCmdDispatchBaseKHR") ==
           (PFN_vkVoidFunction)vkCmdDispatchBaseKHR);
    assert(vkGetDeviceProcAddr(&d, "vkCmdSetDeviceMaskKHR") ==
           (PFN_vkVoidFunction)vkCmdSetDeviceMaskKHR);
    assert(vkGetDeviceProcAddr(&d, "vkGetDeviceGroupPeerMemoryFeaturesKHR") ==
           (PFN_vkVoidFunction)vkGetDeviceGroupPeerMemoryFeaturesKHR);
    VkPeerMemoryFeatureFlags peer = ~0u;
    vkGetDeviceGroupPeerMemoryFeaturesKHR(&d, 0, 0, 0, &peer);
    assert(!peer);
    /* This device still reports Vulkan 1.0: no core-1.1 command alias. */
    assert(!vkGetDeviceProcAddr(&d, "vkCmdDispatchBase"));
    assert(!vkGetDeviceProcAddr(&d, "vkCmdSetDeviceMask"));
}

static void create_renderpass2_command_gate(void)
{
    static const char *const commands[] = {
        "vkCreateRenderPass2KHR", "vkCmdBeginRenderPass2KHR",
        "vkCmdNextSubpass2KHR", "vkCmdEndRenderPass2KHR"};
    const PFN_vkVoidFunction functions[] = {
        (PFN_vkVoidFunction)vkCreateRenderPass2KHR,
        (PFN_vkVoidFunction)vkCmdBeginRenderPass2KHR,
        (PFN_vkVoidFunction)vkCmdNextSubpass2KHR,
        (PFN_vkVoidFunction)vkCmdEndRenderPass2KHR};
    struct VkDevice_T d = {0};
    for (size_t n = 0; n < 4; ++n) assert(!vkGetDeviceProcAddr(&d, commands[n]));
    d.create_renderpass2_extension_enabled = VK_TRUE;
    for (size_t n = 0; n < 4; ++n)
        assert(vkGetDeviceProcAddr(&d, commands[n]) == functions[n]);
    /* This device still reports Vulkan 1.0: no core-1.2 command alias. */
    assert(!vkGetDeviceProcAddr(&d, "vkCreateRenderPass2"));
    assert(!vkGetDeviceProcAddr(&d, "vkCmdBeginRenderPass2"));
    assert(!vkGetDeviceProcAddr(&d, "vkCmdNextSubpass2"));
    assert(!vkGetDeviceProcAddr(&d, "vkCmdEndRenderPass2"));
}

static void descriptor_update_template_command_gate(void)
{
    static const char *const commands[] = {
        "vkCreateDescriptorUpdateTemplateKHR", "vkDestroyDescriptorUpdateTemplateKHR",
        "vkUpdateDescriptorSetWithTemplateKHR"};
    const PFN_vkVoidFunction functions[] = {
        (PFN_vkVoidFunction)vkCreateDescriptorUpdateTemplateKHR,
        (PFN_vkVoidFunction)vkDestroyDescriptorUpdateTemplateKHR,
        (PFN_vkVoidFunction)vkUpdateDescriptorSetWithTemplateKHR};
    struct VkDevice_T d = {0};
    for (size_t n = 0; n < 3; ++n) assert(!vkGetDeviceProcAddr(&d, commands[n]));
    d.descriptor_update_template_extension_enabled = VK_TRUE;
    for (size_t n = 0; n < 3; ++n)
        assert(vkGetDeviceProcAddr(&d, commands[n]) == functions[n]);
    /* This device still reports Vulkan 1.0: no core-1.1 command alias, and
     * no push-descriptor template command. */
    assert(!vkGetDeviceProcAddr(&d, "vkCreateDescriptorUpdateTemplate"));
    assert(!vkGetDeviceProcAddr(&d, "vkDestroyDescriptorUpdateTemplate"));
    assert(!vkGetDeviceProcAddr(&d, "vkUpdateDescriptorSetWithTemplate"));
    assert(!vkGetDeviceProcAddr(&d, "vkCmdPushDescriptorSetWithTemplateKHR"));
}

static void buffer_address_khr_device_route(void)
{
    const char *instance_names[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = instance_names};
    VkInstance i = VK_NULL_HANDLE;
    assert(vkCreateInstance(&instance_info, NULL, &i) == VK_SUCCESS);
    VkPhysicalDevice p = physical(i);
    const uint32_t baseline = p->platform.supported_features;
    VkPhysicalDeviceBufferDeviceAddressFeaturesKHR reported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &reported};
    vkGetPhysicalDeviceFeatures2KHR(p, &query);
    assert(!reported.bufferDeviceAddress && !reported.bufferDeviceAddressCaptureReplay &&
           !reported.bufferDeviceAddressMultiDevice);

    const char *device_names[] = {
        VK_KHR_DEVICE_GROUP_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    };
    VkPhysicalDeviceBufferDeviceAddressFeaturesKHR requested = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR,
        .bufferDeviceAddress = VK_TRUE};
    VkDeviceGroupDeviceCreateInfo group = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO,
        .pNext = &requested, .physicalDeviceCount = 1,
        .pPhysicalDevices = &p};
    VkDeviceQueueCreateInfo q;
    float priority;
    VkDeviceCreateInfo info = device_info(&q, &priority);
    info.pNext = &group;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = device_names;
    VkDevice d = VK_NULL_HANDLE;
    const unsigned before = opened;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT);
    assert(!d && opened == before);

    p->platform.supported_features |= PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS;
    vkGetPhysicalDeviceFeatures2KHR(p, &query);
    assert(reported.bufferDeviceAddress && !reported.bufferDeviceAddressCaptureReplay &&
           !reported.bufferDeviceAddressMultiDevice);
    uint32_t count = 0;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS);
    assert(count <= 16);
    VkExtensionProperties properties[16] = {0};
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, properties) == VK_SUCCESS);
    int found_group = 0, found_bda = 0;
    for (uint32_t n = 0; n < count; ++n) {
        found_group |= !strcmp(properties[n].extensionName, device_names[0]);
        found_bda |= !strcmp(properties[n].extensionName, device_names[1]);
    }
    assert(found_group && found_bda);
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
    assert(d && d->device_group_extension_enabled &&
           (d->enabled_features & PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS));
    vkDestroyDevice(d, NULL);

    requested.bufferDeviceAddress = VK_FALSE;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
    assert(d && d->device_group_extension_enabled &&
           !(d->enabled_features & PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS));
    vkDestroyDevice(d, NULL);
    requested.bufferDeviceAddress = VK_TRUE;
    info.enabledExtensionCount = 1;
    requested.bufferDeviceAddress = VK_FALSE;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
    assert(d && d->device_group_extension_enabled &&
           !(d->enabled_features & PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS));
    vkDestroyDevice(d, NULL);
    requested.bufferDeviceAddress = VK_TRUE;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = &device_names[1];
    info.enabledExtensionCount = 1;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    info.ppEnabledExtensionNames = device_names;
    info.enabledExtensionCount = 2;
    requested.bufferDeviceAddressCaptureReplay = VK_TRUE;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    requested.bufferDeviceAddressCaptureReplay = VK_FALSE;
    requested.bufferDeviceAddressMultiDevice = VK_TRUE;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    requested.bufferDeviceAddressMultiDevice = 2;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_UNKNOWN && !d);
    requested.bufferDeviceAddressMultiDevice = VK_FALSE;
    requested.bufferDeviceAddress = 2;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_UNKNOWN && !d);
    requested.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceBufferDeviceAddressFeaturesKHR duplicate = requested;
    requested.pNext = &duplicate;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_UNKNOWN && !d);
    requested.pNext = NULL;
    p->platform.supported_features = baseline;
    vkDestroyInstance(i, NULL);

    instance_info.enabledExtensionCount = 1;
    assert(vkCreateInstance(&instance_info, NULL, &i) == VK_SUCCESS);
    p = physical(i);
    p->platform.supported_features |= PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS;
    group.pPhysicalDevices = &p;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);

    instance_info.ppEnabledExtensionNames = &instance_names[1];
    assert(vkCreateInstance(&instance_info, NULL, &i) == VK_SUCCESS);
    p = physical(i);
    p->platform.supported_features |= PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS;
    group.pPhysicalDevices = &p;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);
}
static void uniform_buffer_standard_layout_route(void)
{
    VkInstance instance_with_features2 = features2_instance();
    VkPhysicalDevice p = physical(instance_with_features2);
    VkPhysicalDeviceUniformBufferStandardLayoutFeatures layout_feature = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES};
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &layout_feature};
    vkGetPhysicalDeviceFeatures2KHR(p, &features);
    assert(!layout_feature.uniformBufferStandardLayout);
    VkExtensionProperties extensions[8]; uint32_t count = 8;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, extensions) == VK_SUCCESS);
    for (uint32_t n = 0; n < count; ++n)
        assert(strcmp(extensions[n].extensionName,
                      VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME));

    /* Simulate a capable platform to test the Vulkan 1.0 KHR negotiation.
     * The shipping platform never sets this bit until native acceptance. */
    p->platform.supported_features |= PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT;
    layout_feature.uniformBufferStandardLayout = VK_FALSE;
    vkGetPhysicalDeviceFeatures2KHR(p, &features);
    assert(layout_feature.uniformBufferStandardLayout == VK_TRUE);
    count = 8;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, extensions) == VK_SUCCESS);
    int found = 0;
    for (uint32_t n = 0; n < count; ++n)
        found |= !strcmp(extensions[n].extensionName,
                         VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME);
    assert(found);

    const char *name = VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME;
    VkDeviceQueueCreateInfo queue; float priority;
    VkDeviceCreateInfo info = device_info(&queue, &priority);
    info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = &name;
    VkDevice device = VK_NULL_HANDLE;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_SUCCESS);
    assert(!(device->enabled_features & PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT));
    vkDestroyDevice(device, NULL);
    layout_feature.uniformBufferStandardLayout = VK_TRUE;
    info.pNext = &layout_feature;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_SUCCESS);
    assert(device->enabled_features & PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT);
    vkDestroyDevice(device, NULL);

    info.enabledExtensionCount = 0; info.ppEnabledExtensionNames = NULL;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = &name;
    VkPhysicalDeviceUniformBufferStandardLayoutFeatures duplicate = layout_feature;
    layout_feature.pNext = &duplicate;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_UNKNOWN && !device);
    layout_feature.pNext = NULL;
    layout_feature.uniformBufferStandardLayout = 2;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_UNKNOWN && !device);
    layout_feature.uniformBufferStandardLayout = VK_TRUE;
    p->platform.supported_features &= ~PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_EXTENSION_NOT_PRESENT && !device);
    vkDestroyInstance(instance_with_features2, NULL);

    VkInstance plain_instance = instance();
    VkPhysicalDevice plain = physical(plain_instance);
    plain->platform.supported_features |= PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT;
    info.pNext = NULL;
    assert(vkCreateDevice(plain, &info, NULL, &device) == VK_ERROR_EXTENSION_NOT_PRESENT && !device);
    vkDestroyInstance(plain_instance, NULL);
}

/* VK_EXT_robustness2 (DXVK262-T13): unreported by the host platform; with the
 * two platform bits the extension, the feature and property structures and
 * device creation follow the pinned DXVK request exactly. */
static int has_robustness2(VkPhysicalDevice p)
{
    VkExtensionProperties extensions[24]; uint32_t count = 24;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, extensions) == VK_SUCCESS);
    int found = 0;
    for (uint32_t n = 0; n < count; ++n)
        found |= !strcmp(extensions[n].extensionName, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    return found;
}
static void robustness2_route(void)
{
    VkInstance instance_with_features2 = features2_instance();
    VkPhysicalDevice p = physical(instance_with_features2);
    VkPhysicalDeviceRobustness2FeaturesEXT reported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .robustBufferAccess2 = 7, .robustImageAccess2 = 7, .nullDescriptor = 7};
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &reported};
    vkGetPhysicalDeviceFeatures2KHR(p, &features);
    assert(!reported.robustBufferAccess2 && !reported.robustImageAccess2 &&
           !reported.nullDescriptor && !has_robustness2(p));
    VkPhysicalDeviceRobustness2PropertiesEXT limits = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT,
        .robustStorageBufferAccessSizeAlignment = 99,
        .robustUniformBufferAccessSizeAlignment = 99};
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &limits};
    vkGetPhysicalDeviceProperties2KHR(p, &properties);
    assert(!limits.robustStorageBufferAccessSizeAlignment &&
           !limits.robustUniformBufferAccessSizeAlignment);
    const char *name = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
    VkDeviceQueueCreateInfo queue; float priority;
    VkDeviceCreateInfo info = device_info(&queue, &priority);
    info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = &name;
    VkDevice device = VK_NULL_HANDLE;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_EXTENSION_NOT_PRESENT && !device);

    /* A capable platform: both implemented features, never robustImageAccess2. */
    p->platform.supported_features_t09 |= PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2 |
        PS5VK_T09_FEATURE_NULL_DESCRIPTOR;
    vkGetPhysicalDeviceFeatures2KHR(p, &features);
    assert(reported.robustBufferAccess2 == VK_TRUE && reported.nullDescriptor == VK_TRUE &&
           reported.robustImageAccess2 == VK_FALSE && has_robustness2(p));
    vkGetPhysicalDeviceProperties2KHR(p, &properties);
    assert(limits.robustStorageBufferAccessSizeAlignment == 4 &&
           limits.robustUniformBufferAccessSizeAlignment == 4);

    /* The pinned DXVK request: core robustBufferAccess in Features2 plus
     * robustBufferAccess2 and nullDescriptor in the extension structure. */
    VkPhysicalDeviceRobustness2FeaturesEXT requested = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .robustBufferAccess2 = VK_TRUE, .nullDescriptor = VK_TRUE};
    VkPhysicalDeviceFeatures2 enabled = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &requested, .features = {.robustBufferAccess = VK_TRUE}};
    info.pNext = &enabled;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_SUCCESS);
    assert((device->enabled_features_t09 & (PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2 |
            PS5VK_T09_FEATURE_NULL_DESCRIPTOR)) ==
           (PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2 | PS5VK_T09_FEATURE_NULL_DESCRIPTOR));
    vkDestroyDevice(device, NULL);
    /* The extension alone enables neither feature. */
    info.pNext = NULL;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_SUCCESS);
    assert(!(device->enabled_features_t09 & (PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2 |
             PS5VK_T09_FEATURE_NULL_DESCRIPTOR)));
    vkDestroyDevice(device, NULL);
    info.pNext = &enabled;
    /* VUID-...-robustBufferAccess2-04000: robustBufferAccess must be enabled. */
    enabled.features.robustBufferAccess = VK_FALSE;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_UNKNOWN && !device);
    /* nullDescriptor alone does not need robustBufferAccess. */
    requested.robustBufferAccess2 = VK_FALSE;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_SUCCESS);
    assert((device->enabled_features_t09 & (PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2 |
            PS5VK_T09_FEATURE_NULL_DESCRIPTOR)) == PS5VK_T09_FEATURE_NULL_DESCRIPTOR);
    vkDestroyDevice(device, NULL);
    enabled.features.robustBufferAccess = VK_TRUE; requested.robustBufferAccess2 = VK_TRUE;
    /* robustImageAccess2 is not implemented; non-boolean values are invalid. */
    requested.robustImageAccess2 = VK_TRUE;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    requested.robustImageAccess2 = 2;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_UNKNOWN && !device);
    requested.robustImageAccess2 = VK_FALSE;
    /* Duplicate structure, and the structure without the extension. */
    VkPhysicalDeviceRobustness2FeaturesEXT duplicate = requested;
    requested.pNext = &duplicate;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    requested.pNext = NULL;
    info.enabledExtensionCount = 0;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    info.enabledExtensionCount = 1;
    /* Each feature follows its own platform bit. */
    p->platform.supported_features_t09 &= ~(uint32_t)PS5VK_T09_FEATURE_NULL_DESCRIPTOR;
    vkGetPhysicalDeviceFeatures2KHR(p, &features);
    assert(reported.robustBufferAccess2 && !reported.nullDescriptor && has_robustness2(p));
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    /* robustBufferAccess2 relies on offset alignments that are multiples of four. */
    p->platform.supported_features_t09 |= PS5VK_T09_FEATURE_NULL_DESCRIPTOR;
    const VkDeviceSize saved_alignment =
        p->platform.properties.limits.minUniformBufferOffsetAlignment;
    p->platform.properties.limits.minUniformBufferOffsetAlignment = 2;
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
    p->platform.properties.limits.minUniformBufferOffsetAlignment = saved_alignment;
    vkDestroyInstance(instance_with_features2, NULL);

    /* Registry dependency: the Features2 instance extension on a 1.0 device. */
    VkInstance plain_instance = instance();
    VkPhysicalDevice plain = physical(plain_instance);
    plain->platform.supported_features_t09 |= PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2 |
        PS5VK_T09_FEATURE_NULL_DESCRIPTOR;
    info.pNext = NULL;
    assert(vkCreateDevice(plain, &info, NULL, &device) == VK_ERROR_EXTENSION_NOT_PRESENT && !device);
    vkDestroyInstance(plain_instance, NULL);
}

static void wsi_display_surface_contract(void)
{
    const char *names[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = names};
    VkInstance i = VK_NULL_HANDLE;
    names[0] = VK_KHR_DISPLAY_EXTENSION_NAME;
    info.enabledExtensionCount = 1;
    assert(vkCreateInstance(&info, NULL, &i) == VK_ERROR_EXTENSION_NOT_PRESENT && !i);
    names[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    info.enabledExtensionCount = 2;
    assert(vkCreateInstance(&info, NULL, &i) == VK_SUCCESS);
    VkPhysicalDevice p = physical(i);
    p->platform.queue_flags |= VK_QUEUE_GRAPHICS_BIT;
    assert(vkGetInstanceProcAddr(i, "vkGetPhysicalDeviceDisplayPropertiesKHR") ==
           (PFN_vkVoidFunction)vkGetPhysicalDeviceDisplayPropertiesKHR);
    assert(vkGetInstanceProcAddr(i, "vkCreateDisplayPlaneSurfaceKHR") ==
           (PFN_vkVoidFunction)vkCreateDisplayPlaneSurfaceKHR);
    assert(vkGetInstanceProcAddr(i, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") ==
           (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    assert(!vkGetInstanceProcAddr(NULL, "vkCreateDisplayPlaneSurfaceKHR"));
    uint32_t count = 0;
    assert(vkGetPhysicalDeviceDisplayPropertiesKHR(p, &count, NULL) == VK_SUCCESS && count == 1);
    VkDisplayPropertiesKHR display = {0};
    count = 0;
    assert(vkGetPhysicalDeviceDisplayPropertiesKHR(p, &count, &display) == VK_INCOMPLETE);
    count = 1;
    assert(vkGetPhysicalDeviceDisplayPropertiesKHR(p, &count, &display) == VK_SUCCESS);
    assert(display.physicalResolution.width == 1920 && display.physicalResolution.height == 1080);
    assert(display.supportedTransforms == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
    VkDisplayModePropertiesKHR mode = {0};
    count = 1;
    assert(vkGetDisplayModePropertiesKHR(p, display.display, &count, &mode) == VK_SUCCESS);
    assert(mode.parameters.visibleRegion.width == 1920 &&
           mode.parameters.visibleRegion.height == 1080 &&
           mode.parameters.refreshRate == 60000);
    VkDisplayModeCreateInfoKHR mode_info = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_MODE_CREATE_INFO_KHR,
        .parameters = mode.parameters};
    VkDisplayModeKHR made = VK_NULL_HANDLE;
    assert(vkCreateDisplayModeKHR(p, display.display, &mode_info, NULL, &made) == VK_SUCCESS);
    assert(made == mode.displayMode);
    mode_info.parameters.refreshRate = 50000;
    assert(vkCreateDisplayModeKHR(p, display.display, &mode_info, NULL, &made) ==
           VK_ERROR_INITIALIZATION_FAILED && !made);
    VkDisplayPlanePropertiesKHR plane = {0};
    count = 1;
    assert(vkGetPhysicalDeviceDisplayPlanePropertiesKHR(p, &count, &plane) == VK_SUCCESS);
    assert(plane.currentDisplay == display.display && plane.currentStackIndex == 0);
    VkDisplayKHR supported = VK_NULL_HANDLE;
    count = 1;
    assert(vkGetDisplayPlaneSupportedDisplaysKHR(p, 0, &count, &supported) == VK_SUCCESS);
    assert(supported == display.display);
    VkDisplayPlaneCapabilitiesKHR plane_caps = {0};
    assert(vkGetDisplayPlaneCapabilitiesKHR(p, mode.displayMode, 0, &plane_caps) == VK_SUCCESS);
    assert(plane_caps.supportedAlpha == VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR);
    VkDisplaySurfaceCreateInfoKHR create = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = mode.displayMode, .planeIndex = 0, .planeStackIndex = 0,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = {1920, 1080}};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    create.imageExtent.width = 1280;
    assert(vkCreateDisplayPlaneSurfaceKHR(i, &create, NULL, &surface) ==
           VK_ERROR_INITIALIZATION_FAILED && !surface);
    create.imageExtent.width = 1920;
    assert(vkCreateDisplayPlaneSurfaceKHR(i, &create, NULL, &surface) == VK_SUCCESS && surface);
    VkBool32 present = VK_FALSE;
    assert(vkGetPhysicalDeviceSurfaceSupportKHR(p, 0, surface, &present) == VK_SUCCESS && !present);
    VkSurfaceCapabilitiesKHR caps = {0};
    assert(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p, surface, &caps) == VK_SUCCESS);
    assert(caps.minImageCount == 2 && caps.maxImageCount == 2 && caps.maxImageArrayLayers == 1);
    assert(caps.currentExtent.width == 1920 && caps.currentExtent.height == 1080);
    assert(caps.supportedUsageFlags == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    VkSurfaceFormatKHR format = {0};
    count = 1;
    assert(vkGetPhysicalDeviceSurfaceFormatsKHR(p, surface, &count, &format) == VK_SUCCESS);
    assert(count == 1 && format.format == VK_FORMAT_B8G8R8A8_UNORM &&
           format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    count = 1;
    assert(vkGetPhysicalDeviceSurfacePresentModesKHR(p, surface, &count, &present_mode) == VK_SUCCESS);
    assert(count == 1 && present_mode == VK_PRESENT_MODE_FIFO_KHR);
    vkDestroyInstance(i, NULL);
    assert(i->lifetime_errors == 1);
    vkDestroySurfaceKHR(i, surface, NULL);
    assert(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p, surface, &caps) == VK_ERROR_SURFACE_LOST_KHR);
    vkDestroyInstance(i, NULL);

    VkInstance plain = instance();
    assert(!vkGetInstanceProcAddr(plain, "vkGetPhysicalDeviceDisplayPropertiesKHR"));
    assert(!vkGetInstanceProcAddr(plain, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
    vkDestroyInstance(plain, NULL);
}

static VkResult mock_wsi_image_requirements(VkDevice d,
    const VkImageCreateInfo *info, VkMemoryRequirements *out)
{
    (void)d; (void)info;
    *out = (VkMemoryRequirements){.size = 64u * 1024u * 1024u,
        .alignment = 65536, .memoryTypeBits = 1};
    return VK_SUCCESS;
}
static void mock_wsi_configure(VkDevice d)
{
    d->graphics_enabled = VK_TRUE;
    d->graphics_submit_enabled = VK_TRUE;
    d->image_requirements = mock_wsi_image_requirements;
}
static void mock_wsi_configure_incomplete(VkDevice d)
{
    d->graphics_enabled = VK_TRUE;
}
static VkBool32 has_swapchain_extension(VkPhysicalDevice p)
{
    VkExtensionProperties properties[32] = {0};
    uint32_t count = 32;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, properties) == VK_SUCCESS);
    for (uint32_t n = 0; n < count; ++n)
        if (!strcmp(properties[n].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            return VK_TRUE;
    return VK_FALSE;
}
static void wsi_swapchain_negotiation_contract(void)
{
    const char *instance_names[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                    VK_KHR_DISPLAY_EXTENSION_NAME};
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = instance_names};
    VkInstance i = VK_NULL_HANDLE;
    assert(vkCreateInstance(&instance_info, NULL, &i) == VK_SUCCESS);
    VkPhysicalDevice p = physical(i);
    p->platform.queue_flags |= VK_QUEUE_GRAPHICS_BIT;
    p->platform.format_properties = ps5vk_graphics_format_properties;
    p->platform.image_properties = ps5vk_graphics_image_properties;
    p->platform.max_allocation = UINT64_C(128) * 1024 * 1024;
    p->platform.configure = mock_wsi_configure;
    VkDisplayPropertiesKHR display = {0};
    uint32_t count = 1;
    assert(vkGetPhysicalDeviceDisplayPropertiesKHR(p, &count, &display) == VK_SUCCESS);
    VkDisplayModePropertiesKHR mode = {0};
    count = 1;
    assert(vkGetDisplayModePropertiesKHR(p, display.display, &count, &mode) == VK_SUCCESS);
    VkDisplaySurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = mode.displayMode, .planeIndex = 0, .planeStackIndex = 0,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = {1920, 1080}};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    assert(vkCreateDisplayPlaneSurfaceKHR(i, &surface_info, NULL, &surface) == VK_SUCCESS);
    float priority;
    VkDeviceQueueCreateInfo queue;
    VkDeviceCreateInfo device_info_value = device_info(&queue, &priority);
    const char *device_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    device_info_value.enabledExtensionCount = 1;
    device_info_value.ppEnabledExtensionNames = &device_extension;
    VkDevice d = VK_NULL_HANDLE;
    VkBool32 present = VK_TRUE;
    VkSurfaceCapabilitiesKHR caps = {0};

    assert(!has_swapchain_extension(p));
    assert(vkGetPhysicalDeviceSurfaceSupportKHR(p, 0, surface, &present) == VK_SUCCESS && !present);
    assert(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p, surface, &caps) == VK_SUCCESS &&
           caps.supportedUsageFlags == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    assert(vkCreateDevice(p, &device_info_value, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);

    mock_wsi_available = VK_TRUE;
    assert(has_swapchain_extension(p));
    assert(vkGetPhysicalDeviceSurfaceSupportKHR(p, 0, surface, &present) == VK_SUCCESS && present);
    assert(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p, surface, &caps) == VK_SUCCESS &&
           caps.supportedUsageFlags == (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    p->platform.max_allocation = UINT64_C(64) * 1024 * 1024;
    assert(!has_swapchain_extension(p));
    p->platform.max_allocation = UINT64_C(128) * 1024 * 1024;
    p->platform.queue_flags &= ~VK_QUEUE_GRAPHICS_BIT;
    assert(!has_swapchain_extension(p));
    p->platform.queue_flags |= VK_QUEUE_GRAPHICS_BIT;
    p->platform.image_properties = NULL;
    assert(!has_swapchain_extension(p));
    p->platform.image_properties = ps5vk_graphics_image_properties;
    p->platform.configure = mock_wsi_configure_incomplete;
    unsigned before_open = opened, before_close = closed;
    assert(vkCreateDevice(p, &device_info_value, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    assert(opened == before_open + 1 && closed == before_close + 1);
    p->platform.configure = mock_wsi_configure;
    assert(vkCreateDevice(p, &device_info_value, NULL, &d) == VK_SUCCESS && d);
    assert(d->swapchain_extension_enabled);
    const char *commands[] = {"vkCreateSwapchainKHR", "vkDestroySwapchainKHR",
        "vkGetSwapchainImagesKHR", "vkAcquireNextImageKHR", "vkQueuePresentKHR"};
    for (size_t n = 0; n < sizeof(commands) / sizeof(commands[0]); ++n)
        assert(vkGetDeviceProcAddr(d, commands[n]));
    d->swapchains = (VkSwapchainKHR)(uintptr_t)1;
    vkDestroyDevice(d, NULL);
    assert(d->lifetime_errors == 1 && i->devices == 1);
    d->swapchains = VK_NULL_HANDLE;
    vkDestroyDevice(d, NULL);

    device_info_value.enabledExtensionCount = 0;
    device_info_value.ppEnabledExtensionNames = NULL;
    assert(vkCreateDevice(p, &device_info_value, NULL, &d) == VK_SUCCESS && d);
    for (size_t n = 0; n < sizeof(commands) / sizeof(commands[0]); ++n)
        assert(!vkGetDeviceProcAddr(d, commands[n]));
    vkDestroyDevice(d, NULL);
    vkDestroySurfaceKHR(i, surface, NULL);
    vkDestroyInstance(i, NULL);
    mock_wsi_available = VK_FALSE;

    instance_info.enabledExtensionCount = 0;
    assert(vkCreateInstance(&instance_info, NULL, &i) == VK_SUCCESS);
    p = physical(i);
    p->platform.queue_flags |= VK_QUEUE_GRAPHICS_BIT;
    p->platform.format_properties = ps5vk_graphics_format_properties;
    p->platform.image_properties = ps5vk_graphics_image_properties;
    p->platform.max_allocation = UINT64_C(128) * 1024 * 1024;
    p->platform.configure = mock_wsi_configure;
    mock_wsi_available = VK_TRUE;
    assert(!has_swapchain_extension(p));
    vkDestroyInstance(i, NULL);
    mock_wsi_available = VK_FALSE;
}

int main(void)
{
    lifecycle(); negative(); narrow_storage_features(); allocator_lifetimes();
    wsi_display_surface_contract();
    wsi_swapchain_negotiation_contract();
    consumer_physical_queries();
    tessellation_feature_negotiation();
    shader_int16_core_route();
    unadvertised_subgroup_properties();
    memory_model_feature_negotiation();
    single_device_group_creation();
    vulkan11_instance_version();
    buffer_address_command_gate();
    device_group_dispatch_command_gate();
    create_renderpass2_command_gate(); descriptor_update_template_command_gate();
    buffer_address_khr_device_route();
    uniform_buffer_standard_layout_route();
    robustness2_route();
    puts("Vulkan device lifecycle: pass (host backend only)");
}
