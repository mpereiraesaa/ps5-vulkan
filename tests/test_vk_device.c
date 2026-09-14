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
    assert(gl.maxPerStageDescriptorSamplers==1 && gl.maxPerStageDescriptorSampledImages==1);
    assert(gl.maxDescriptorSetSamplers==1 && gl.maxDescriptorSetSampledImages==1);
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
    assert(gl.maxVertexInputBindings==1 && gl.maxVertexInputAttributes==32);
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
        assert(ps5vk_physical_profile_valid(&profile, &profile_memory, max_allocation,
            queue_flags, 1, 1));
    }
    /* Enumerate all combinations of known core image role bits, not only the
     * three happy paths. Mixed executable/non-executable roles must fail. */
    for(unsigned usage=0;usage<256;++usage) {
        assert(!!ps5vk_graphics_image_usage(VK_FORMAT_B8G8R8A8_UNORM,usage)==
            (usage==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
        assert(!!ps5vk_graphics_image_usage(VK_FORMAT_D32_SFLOAT,usage)==
            (usage==VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
    assert(!!ps5vk_graphics_image_usage(VK_FORMAT_R8G8B8A8_UNORM,usage)==
            (usage==VK_IMAGE_USAGE_SAMPLED_BIT || usage==VK_IMAGE_USAGE_TRANSFER_DST_BIT ||
             usage==(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
             usage==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ||
             usage==(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
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
            const VkBool32 attachment=(usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))!=0;
            const VkBool32 transfer_only=image_formats[f]==VK_FORMAT_R8G8B8A8_UNORM && usage &&
                !(usage&~(VkImageUsageFlags)(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT));
            assert(ip.maxMipLevels==1 &&
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
    const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
        VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_R8_SNORM,VK_FORMAT_R8G8_SNORM,VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        VK_FORMAT_R16_UNORM,VK_FORMAT_R16_SNORM,VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16_SNORM,VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_R16G16B16A16_UNORM,VK_FORMAT_R16G16B16A16_SNORM,
        VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT};
    for (unsigned n=0; n<sizeof(formats)/sizeof(formats[0]); ++n) {
        memset(&fp, 0xff, sizeof(fp));
        vkGetPhysicalDeviceFormatProperties(p, formats[n], &fp);
        VkFormatFeatureFlags buffer_bits=ps5vk_vertex_format_size(formats[n])?
            VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT:0;
        if(formats[n]==VK_FORMAT_R32_SFLOAT)
            buffer_bits|=VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
        VkFormatFeatureFlags optimal_bits=0;
        const struct ps5vk_texture_format *sampled=
            ps5vk_texture_format_lookup(formats[n]);
        if(sampled && sampled->validated) {
            optimal_bits=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if(sampled->linear_filter_validated)
                optimal_bits|=VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        }
        if(formats[n]==VK_FORMAT_B8G8R8A8_UNORM)
            optimal_bits=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        else if(formats[n]==VK_FORMAT_R8G8B8A8_UNORM)
            optimal_bits|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        else if(formats[n]==VK_FORMAT_D32_SFLOAT)
            optimal_bits=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        assert(!fp.linearTilingFeatures && fp.bufferFeatures==buffer_bits &&
            fp.optimalTilingFeatures==optimal_bits);
    }
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
        VkFormatFeatureFlags expected=VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
            ((vertex_formats[n]==VK_FORMAT_R32_SFLOAT ||
              vertex_formats[n]==VK_FORMAT_R32_SINT ||
              vertex_formats[n]==VK_FORMAT_R32_UINT)?VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT:0);
        assert(fp.bufferFeatures==expected);
        assert(!fp.linearTilingFeatures);
        VkFormatFeatureFlags expected_optimal=0;
        const struct ps5vk_texture_format *sampled=
            ps5vk_texture_format_lookup(vertex_formats[n]);
        if(sampled && sampled->validated) {
            expected_optimal=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if(sampled->linear_filter_validated)
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
        if(sampled && sampled->validated)
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
                                     PS5VK_FEATURE_ROBUST_BUFFER_ACCESS;
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
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS && count == 3);
    count = 2;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, device_properties) == VK_INCOMPLETE && count == 2);
    count = 4;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, device_properties) == VK_SUCCESS && count == 3);
    assert(!strcmp(device_properties[0].extensionName, VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME));
    assert(!strcmp(device_properties[1].extensionName, VK_KHR_8BIT_STORAGE_EXTENSION_NAME));
    assert(!strcmp(device_properties[2].extensionName, VK_KHR_16BIT_STORAGE_EXTENSION_NAME));

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
    assert(!protected_features.protectedMemory && !shader_draw_features.shaderDrawParameters);
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
    assert(vkCreateDevice(p, &info, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT && !device);
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
int main(void)
{
    lifecycle(); negative(); narrow_storage_features(); allocator_lifetimes();
    puts("Vulkan device lifecycle: pass (host backend only)");
}
