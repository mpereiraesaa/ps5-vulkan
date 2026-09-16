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
    assert(gl.maxFragmentOutputAttachments==1 && gl.maxFragmentCombinedOutputResources==1);
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
    assert(gl.maxFramebufferLayers==1 && gl.maxColorAttachments==1);
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
        ps5vk_device_profile_init(&profile, &profile_memory, VK_TRUE, VK_TRUE);
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
        ps5vk_device_profile_init(&compute_profile, &compute_memory, VK_FALSE, VK_FALSE);
        assert(compute_profile.limits.subTexelPrecisionBits==PS5VK_REQUIRED_SUBTEXEL_BITS);
        assert(compute_profile.limits.pointSizeRange[1]==PS5VK_REQUIRED_POINT_SIZE);
        assert(compute_profile.limits.sampledImageIntegerSampleCounts==VK_SAMPLE_COUNT_1_BIT);
        assert(strcmp(compute_profile.deviceName, PS5VK_PROFILE_COMPUTE_NAME)==0);
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
            (usage==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
        /* The depth target is an attachment, the destination of the whole-
         * subresource vkCmdClearDepthStencilImage, or both. Every form is the
         * tiled depth surface; no other combination exists. */
        assert(!!ps5vk_graphics_image_usage(VK_FORMAT_D32_SFLOAT,usage)==
            (usage==VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT ||
             usage==VK_IMAGE_USAGE_TRANSFER_DST_BIT ||
             usage==(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|
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
            assert(result==VK_SUCCESS && ip.maxExtent.width==(f==0?16383u:16384u));
            assert(ip.maxExtent.height==ip.maxExtent.width && ip.maxExtent.depth==1);
            /* Every D32 role is the tiled depth surface, including the target
             * created only as the destination of a whole-subresource depth
             * clear, so all of them are attachment-shaped: one mip, one layer. */
            const VkBool32 attachment=(usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))!=0 ||
                image_formats[f]==VK_FORMAT_D32_SFLOAT;
            const VkBool32 transfer_only=image_formats[f]==VK_FORMAT_R8G8B8A8_UNORM && usage &&
                !(usage&~(VkImageUsageFlags)(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT));
            const uint32_t expected_mips=!attachment &&
                (usage&VK_IMAGE_USAGE_SAMPLED_BIT)?15u:1u;
            assert(ip.maxMipLevels==expected_mips &&
                ip.maxArrayLayers==(attachment||transfer_only?1u:PS5VK_MAX_IMAGE_ARRAY_LAYERS) &&
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
        ip.maxExtent.depth==1 && ip.maxArrayLayers==6);
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
            optimal_bits=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        else if(formats[n]==VK_FORMAT_R8G8B8A8_UNORM)
            optimal_bits|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        else if(formats[n]==VK_FORMAT_D32_SFLOAT)
            /* TRANSFER_DST is the whole-subresource depth clear, which is the
             * only transfer role 64KB_Z_X has; there is no TRANSFER_SRC. */
            optimal_bits=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        /* One format publishes a linear-tiling role: RGBA8 carries the transfer
         * destination of the pinned host-readback staging image. */
        const VkFormatFeatureFlags linear_bits = formats[n]==VK_FORMAT_R8G8B8A8_UNORM ?
            (VkFormatFeatureFlags)VK_FORMAT_FEATURE_TRANSFER_DST_BIT : 0;
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
            (VkFormatFeatureFlags)VK_FORMAT_FEATURE_TRANSFER_DST_BIT : 0));
        VkFormatFeatureFlags expected_optimal=0;
        if(sampled && (sampled->witnessed & PS5VK_FORMAT_CAP_SAMPLED_IMAGE)) {
            expected_optimal=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if(sampled->witnessed & PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR)
                expected_optimal|=VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        }
        if(vertex_formats[n]==VK_FORMAT_R8G8B8A8_UNORM)
            expected_optimal|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        else if(vertex_formats[n]==VK_FORMAT_B8G8R8A8_UNORM)
            expected_optimal=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
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
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    ii.pApplicationInfo = &ai;
    assert(vkCreateInstance(&ii, NULL, &i) == VK_ERROR_INCOMPATIBLE_DRIVER); ii.pApplicationInfo = NULL;
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
    assert(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) == VK_SUCCESS && count == 1);
    VkExtensionProperties instance_properties[2] = {0};
    count = 0;
    assert(vkEnumerateInstanceExtensionProperties(NULL, &count, instance_properties) == VK_INCOMPLETE && count == 0);
    count = 2;
    assert(vkEnumerateInstanceExtensionProperties(NULL, &count, instance_properties) == VK_SUCCESS && count == 1);
    assert(!strcmp(instance_properties[0].extensionName,
                   VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME));
    assert(instance_properties[0].specVersion == VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION);
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
                             &p->platform.memory_properties, 1, 1);
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
int main(void)
{
    lifecycle(); negative(); narrow_storage_features(); allocator_lifetimes();
    consumer_physical_queries();
    puts("Vulkan device lifecycle: pass (host backend only)");
}
