/*
 * Dump what ps5vk reports through the public Vulkan 1.0 query paths, as JSON.
 *
 * This is a host tool: it installs a platform whose properties come from the
 * same initializer the native console platform uses
 * (src/device_profile_report.h), then asks the driver through the real entry
 * points - vkGetPhysicalDeviceProperties, vkGetPhysicalDeviceFeatures,
 * vkGetPhysicalDeviceMemoryProperties, vkGetPhysicalDeviceFormatProperties and
 * vkGetPhysicalDeviceImageFormatProperties - rather than reading C constants.
 * It also emits the authoritative capability ledger (implemented versus
 * witnessed per format) so the audit can prove that every advertised bit has a
 * backend and that nothing is advertised without a console witness.
 * The spec -> reported matrix in tools/check_reporting_matrix.py consumes this
 * output, so a changed report changes the matrix.
 */
#include "vk_internal.h"
#include "device_profile_report.h"
#include "graphics_formats.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Model the default native runtime-graphics profile using its actual overlay,
 * not a second copy of the eight tessellation limits. This host executable
 * reports the profile; it does not provide GPU execution evidence. */
#define PS5VK_GRAPHICS_API 1
#define PS5VK_GRAPHICS_DRAW 1
#define PS5VK_RUNTIME_COMPILER 1
#define PS5VK_RUNTIME_GRAPHICS 1
#include "../native/tess_profile.h"

static int graphics_objects = 1;
static int graphics_submit = 1;
static VkPhysicalDevice dump_physical;

static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx;
    *address = calloc(1, (size_t)size);
    *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult cache_sync(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, cache_sync, cache_sync};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }

VkResult ps5vk_platform_query(struct ps5vk_platform *platform)
{
    memset(platform, 0, sizeof(*platform));
    platform->open = open_backend;
    platform->close = close_backend;
    platform->queue_flags = graphics_submit ?
        (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) : VK_QUEUE_COMPUTE_BIT;
    if (graphics_submit) {
        platform->format_properties = ps5vk_graphics_format_properties;
        platform->image_properties = ps5vk_graphics_image_properties;
    }
    /* Mirror native/platform_ps5.c: the narrow-storage features are negotiated
     * only by builds that link the runtime compiler
     * (tools/build_native.py: use_runtime_compiler = compute and ...). */
    platform->supported_features = PS5VK_FEATURE_ROBUST_BUFFER_ACCESS |
                                   PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT |
                                   PS5VK_FEATURE_VULKAN_MEMORY_MODEL |
                                   PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE |
                                   PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS;
    if (!graphics_objects)
        platform->supported_features |= PS5VK_FEATURE_STORAGE_BUFFER_8BIT |
                                        PS5VK_FEATURE_STORAGE_BUFFER_16BIT;
    if (graphics_submit)
        platform->supported_features |= PS5VK_FEATURE_MULTIVIEW |
                                        PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE |
                                        PS5VK_FEATURE_MULTI_DRAW_INDIRECT |
                                        PS5VK_FEATURE_FULL_DRAW_INDEX_UINT32 |
                                        /* Mirrors native/platform_ps5.c: the
                                         * distance features belong to the
                                         * graphics path, which is the only one
                                         * that can export or read them. */
                                        PS5VK_FEATURE_SHADER_CLIP_DISTANCE |
                                        PS5VK_FEATURE_SHADER_CULL_DISTANCE |
                                        /* The optional geometry stage belongs to
                                         * the graphics submit path too, and to
                                         * the same measured build the witness
                                         * ran on; this dump mirrors the console
                                         * platform's initializer so the
                                         * published matrix is the console's. */
                                        PS5VK_FEATURE_GEOMETRY_SHADER |
                                        /* Fragment storage side effects are
                                         * now part of the shipping graphics
                                         * profile after the native readback
                                         * and focused upstream oracles both
                                        * passed on the integrated build. */
                                        PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS |
                                        /* dual-source blending joined the
                                         * shipping graphics profile on
                                         * 2026-09-21: the native witness and
                                         * all 98 applicable upstream
                                         * dual_source leaves passed on the
                                         * promoted candidate. */
                                        PS5VK_FEATURE_DUAL_SRC_BLEND |
                                        /* DXVK262-T06 independentBlend, promoted
                                         * on 2026-09-22: the platform reports
                                         * the bit, the profile advertises two
                                         * colour attachments, and both upstream
                                         * leaves that require the feature pass.
                                         * This dump mirrors the console
                                         * initializer. */
                                        PS5VK_FEATURE_INDEPENDENT_BLEND |
                                        /* DXVK262-T05, promoted on
                                         * physical-console evidence: the four
                                         * rasterization and viewport features
                                         * the console platform now advertises.
                                         * This dump mirrors that initializer,
                                         * so the published matrix is the
                                         * console's. */
                                        PS5VK_FEATURE_DEPTH_BIAS_CLAMP |
                                        PS5VK_FEATURE_DEPTH_CLAMP |
                                        PS5VK_FEATURE_FILL_MODE_NON_SOLID |
                                        PS5VK_FEATURE_MULTI_VIEWPORT |
                                        /* DXVK262-T06 sampleRateShading,
                                         * promoted on 2026-09-23: the raster
                                         * stage publishes the sample positions
                                         * and interpolates the position at the
                                         * iterated sample, the colour-to-texture
                                         * barrier waits for a confirmed
                                         * writeback, and the feature's own oracle
                                         * passes at both served counts. This dump
                                         * mirrors the console initializer, so the
                                         * published matrix is the console's. */
                                        PS5VK_FEATURE_SAMPLE_RATE_SHADING |
                                        PS5VK_FEATURE_IMAGE_CUBE_ARRAY |
                                        PS5VK_FEATURE_TEXTURE_COMPRESSION_BC |
                                        PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE |
                                        PS5VK_FEATURE_SHADER_IMAGE_GATHER_EXTENDED;

    platform->supported_features_t09 = PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE;
    if (graphics_submit) {
        platform->supported_features_t09 |= PS5VK_T09_FEATURE_HOST_QUERY_RESET;
        platform->supported_features_t09 |= PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE;
    }

    /* DXVK262-T09, mirroring native/platform_ps5.c: timeline semaphores on
     * every profile, and separateDepthStencilLayouts with its
     * maintenance2/create_renderpass2 route on the graphics submit path. */
    if (graphics_submit)
        platform->supported_features_t09 |= PS5VK_T09_FEATURE_SEPARATE_DEPTH_STENCIL_LAYOUTS |
                                            PS5VK_T09_FEATURE_MAINTENANCE2 |
                                            PS5VK_T09_FEATURE_CREATE_RENDERPASS2;
    /* DXVK262-T11, mirroring native/platform_ps5.c: the demote and terminate
     * extension routes on the graphics submit path. */
    if (graphics_submit)
        platform->supported_features_t09 |=
            PS5VK_T09_FEATURE_SHADER_DEMOTE_TO_HELPER_INVOCATION |
            PS5VK_T09_FEATURE_SHADER_TERMINATE_INVOCATION;
    /* Mirroring native/platform_ps5.c: synchronization2 and the format routes
     * (VkFormatProperties3, image format list) on the graphics submit path. */
    if (graphics_submit)
        platform->supported_features_t09 |= PS5VK_T09_FEATURE_SYNCHRONIZATION2 |
            PS5VK_T09_FEATURE_FORMAT_FEATURE_FLAGS2 | PS5VK_T09_FEATURE_IMAGE_FORMAT_LIST;
    /* DXVK262-T14, mirroring native/platform_ps5.c: transform feedback on the
     * graphics submit path. */
    if (graphics_submit)
        platform->supported_features_t09 |= PS5VK_T09_FEATURE_TRANSFORM_FEEDBACK;

    platform->max_allocation = ps5vk_device_profile_max_allocation(graphics_objects);
    ps5vk_device_profile_init(&platform->properties, &platform->memory_properties,
        graphics_objects, graphics_submit, platform->supported_features);
    if (graphics_submit)
        ps5vk_native_tess_profile(platform);
    return VK_SUCCESS;
}

static void json_float(FILE *out, float value)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9g", (double)value);
    fputs(buffer, out);
}

static void json_bool(FILE *out, VkBool32 value) { fputs(value ? "true" : "false", out); }

/* Usage bits by name, so a selection gate can compare the witnessed request
 * with the contract it declares without duplicating the enum in another
 * language. */
static void print_usage_names(FILE *out, VkImageUsageFlags usage)
{
    const struct { VkImageUsageFlags bit; const char *name; } bits[] = {
        {VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "VK_IMAGE_USAGE_TRANSFER_SRC_BIT"},
        {VK_IMAGE_USAGE_TRANSFER_DST_BIT, "VK_IMAGE_USAGE_TRANSFER_DST_BIT"},
        {VK_IMAGE_USAGE_SAMPLED_BIT, "VK_IMAGE_USAGE_SAMPLED_BIT"},
        {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT"},
        {VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
         "VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT"},
        {VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, "VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT"},
    };
    fputs("[", out);
    int first = 1;
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
        if (!(usage & bits[i].bit)) continue;
        fprintf(out, "%s\"%s\"", first ? "" : ", ", bits[i].name);
        first = 0;
    }
    fputs("]", out);
}

/* The resource-footprint contract the pinned upstream multiview helper builds:
 * a 2D RGBA8 array image, optimal tiling, one mip, one sample, six array layers
 * (the deepest extent the selected legacy families render) and usage
 * COLOR_ATTACHMENT | TRANSFER_SRC | INPUT_ATTACHMENT | TRANSFER_DST. The
 * public query and a real vkCreateImage of exactly this request are both
 * recorded for that one shape, so a selection gate can require every field and
 * require the two paths to agree instead of reading C text. */
/* One shape, both public paths: the query's answer for it and the result of
 * really creating that image. A selection gate needs both, because a contract
 * is only supported when the query covers the request and creation succeeds,
 * and because the two disagreeing is itself a finding. */
static void print_shape_probe(FILE *out, const char *indent, VkImageUsageFlags usage,
                              uint32_t array_layers)
{
    const VkImageCreateInfo image_info = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, // sType
        NULL,                                // pNext
        (VkImageCreateFlags)0,               // flags
        VK_IMAGE_TYPE_2D,                    // imageType
        VK_FORMAT_R8G8B8A8_UNORM,            // format
        {64u, 64u, 1u},                      // extent
        1u,                                  // mipLevels
        array_layers,                        // arrayLayers
        VK_SAMPLE_COUNT_1_BIT,               // samples
        VK_IMAGE_TILING_OPTIMAL,             // tiling
        usage,                               // usage
        VK_SHARING_MODE_EXCLUSIVE,           // sharingMode
        0u,                                  // queueFamilyIndexCount
        NULL,                                // pQueueFamilyIndices
        VK_IMAGE_LAYOUT_UNDEFINED,           // initialLayout
    };
    VkImageFormatProperties properties;
    memset(&properties, 0, sizeof(properties));
    const VkResult query_result = vkGetPhysicalDeviceImageFormatProperties(dump_physical,
        image_info.format, image_info.imageType, image_info.tiling, image_info.usage,
        image_info.flags, &properties);
    VkDevice device = VK_NULL_HANDLE;
    float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0u, 0u, 1u, &priority};
    const VkDeviceCreateInfo device_info = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0u, 1u, &queue_info,
        0u, NULL, 0u, NULL, NULL};
    const VkResult device_result = vkCreateDevice(dump_physical, &device_info, NULL, &device);
    VkResult create_result = VK_ERROR_INITIALIZATION_FAILED;
    if (device_result == VK_SUCCESS) {
        /* The native platform installs these during configure; this host
         * fixture installs the same image entry point the console path uses. */
        device->graphics_enabled = VK_TRUE;
        device->image_requirements = ps5vk_native_image_requirements;
        VkImage image = VK_NULL_HANDLE;
        create_result = vkCreateImage(device, &image_info, NULL, &image);
        if (image) vkDestroyImage(device, image, NULL);
        vkDestroyDevice(device, NULL);
    }
    const VkBool32 query_covers = query_result == VK_SUCCESS &&
        properties.maxArrayLayers >= image_info.arrayLayers &&
        properties.maxMipLevels >= image_info.mipLevels &&
        (properties.sampleCounts & image_info.samples) &&
        properties.maxExtent.width >= image_info.extent.width &&
        properties.maxExtent.height >= image_info.extent.height;
    const VkBool32 supported = query_covers && create_result == VK_SUCCESS;

    fprintf(out, "%s\"format\": %u, \"formatName\": \"VK_FORMAT_R8G8B8A8_UNORM\",\n", indent,
            (unsigned)image_info.format);
    fprintf(out, "%s\"imageType\": %u, \"imageTypeName\": \"VK_IMAGE_TYPE_2D\",\n", indent,
            (unsigned)image_info.imageType);
    fprintf(out, "%s\"tiling\": %u, \"tilingName\": \"VK_IMAGE_TILING_OPTIMAL\",\n", indent,
            (unsigned)image_info.tiling);
    fprintf(out, "%s\"mipLevels\": %u, \"samples\": %u, \"arrayLayers\": %u,\n", indent,
            image_info.mipLevels, (unsigned)image_info.samples, image_info.arrayLayers);
    fprintf(out, "%s\"extent\": [%u, %u, %u],\n", indent, image_info.extent.width,
            image_info.extent.height, image_info.extent.depth);
    fprintf(out, "%s\"usage\": %u, \"usageNames\": ", indent, usage);
    print_usage_names(out, image_info.usage);
    fprintf(out, ",\n%s\"deviceResult\": %d, \"queryResult\": %d,\n", indent,
            (int)device_result, (int)query_result);
    fprintf(out, "%s\"queryMaxExtent\": [%u, %u, %u], \"queryMaxMipLevels\": %u, ", indent,
            properties.maxExtent.width, properties.maxExtent.height,
            properties.maxExtent.depth, properties.maxMipLevels);
    fprintf(out, "\"queryMaxArrayLayers\": %u, \"querySampleCounts\": %u,\n",
            properties.maxArrayLayers, properties.sampleCounts);
    fprintf(out, "%s\"createResult\": %d,\n", indent, (int)create_result);
    fprintf(out, "%s\"queryCovers\": %s, \"createSucceeded\": %s, \"supported\": %s",
            indent, query_covers ? "true" : "false",
            create_result == VK_SUCCESS ? "true" : "false", supported ? "true" : "false");
}

static void print_resource_contract_witness(FILE *out)
{
    const VkImageUsageFlags contract_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                             VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    /* The deepest extent any selected legacy family renders is six layers, so
     * that is the ceiling this witness has to cover. */
    fputs("    \"multiview-attachment-image\": {\n", out);
    print_shape_probe(out, "      ", contract_usage, 6u);
    /* The same shape without the input-attachment role: what the driver can do
     * today, so the layer ceiling can be read independently of the usage gap. */
    fputs(",\n      \"withoutInputAttachment\": {\n", out);
    print_shape_probe(out, "        ",
                      contract_usage & ~(VkImageUsageFlags)VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
                      6u);
    fputs("\n      }\n", out);
    fputs("    }\n", out);
}

/* Print every field of the Vulkan 1.0 physical-device limit block by name, so
 * the matrix tool never has to guess which numbers were reported. */
static void print_limits(FILE *out, const VkPhysicalDeviceLimits *l)
{
    fputs("{\n", out);
    fprintf(out, "      \"maxImageDimension1D\": %u,\n", l->maxImageDimension1D);
    fprintf(out, "      \"maxImageDimension2D\": %u,\n", l->maxImageDimension2D);
    fprintf(out, "      \"maxImageDimension3D\": %u,\n", l->maxImageDimension3D);
    fprintf(out, "      \"maxImageDimensionCube\": %u,\n", l->maxImageDimensionCube);
    fprintf(out, "      \"maxImageArrayLayers\": %u,\n", l->maxImageArrayLayers);
    fprintf(out, "      \"maxTexelBufferElements\": %u,\n", l->maxTexelBufferElements);
    fprintf(out, "      \"maxUniformBufferRange\": %u,\n", l->maxUniformBufferRange);
    fprintf(out, "      \"maxStorageBufferRange\": %u,\n", l->maxStorageBufferRange);
    fprintf(out, "      \"maxPushConstantsSize\": %u,\n", l->maxPushConstantsSize);
    fprintf(out, "      \"maxMemoryAllocationCount\": %u,\n", l->maxMemoryAllocationCount);
    fprintf(out, "      \"maxSamplerAllocationCount\": %u,\n", l->maxSamplerAllocationCount);
    fprintf(out, "      \"bufferImageGranularity\": %llu,\n", (unsigned long long)l->bufferImageGranularity);
    fprintf(out, "      \"sparseAddressSpaceSize\": %llu,\n", (unsigned long long)l->sparseAddressSpaceSize);
    fprintf(out, "      \"maxBoundDescriptorSets\": %u,\n", l->maxBoundDescriptorSets);
    fprintf(out, "      \"maxPerStageDescriptorSamplers\": %u,\n", l->maxPerStageDescriptorSamplers);
    fprintf(out, "      \"maxPerStageDescriptorUniformBuffers\": %u,\n", l->maxPerStageDescriptorUniformBuffers);
    fprintf(out, "      \"maxPerStageDescriptorStorageBuffers\": %u,\n", l->maxPerStageDescriptorStorageBuffers);
    fprintf(out, "      \"maxPerStageDescriptorSampledImages\": %u,\n", l->maxPerStageDescriptorSampledImages);
    fprintf(out, "      \"maxPerStageDescriptorStorageImages\": %u,\n", l->maxPerStageDescriptorStorageImages);
    fprintf(out, "      \"maxPerStageDescriptorInputAttachments\": %u,\n", l->maxPerStageDescriptorInputAttachments);
    fprintf(out, "      \"maxPerStageResources\": %u,\n", l->maxPerStageResources);
    fprintf(out, "      \"maxDescriptorSetSamplers\": %u,\n", l->maxDescriptorSetSamplers);
    fprintf(out, "      \"maxDescriptorSetUniformBuffers\": %u,\n", l->maxDescriptorSetUniformBuffers);
    fprintf(out, "      \"maxDescriptorSetUniformBuffersDynamic\": %u,\n", l->maxDescriptorSetUniformBuffersDynamic);
    fprintf(out, "      \"maxDescriptorSetStorageBuffers\": %u,\n", l->maxDescriptorSetStorageBuffers);
    fprintf(out, "      \"maxDescriptorSetStorageBuffersDynamic\": %u,\n", l->maxDescriptorSetStorageBuffersDynamic);
    fprintf(out, "      \"maxDescriptorSetSampledImages\": %u,\n", l->maxDescriptorSetSampledImages);
    fprintf(out, "      \"maxDescriptorSetStorageImages\": %u,\n", l->maxDescriptorSetStorageImages);
    fprintf(out, "      \"maxDescriptorSetInputAttachments\": %u,\n", l->maxDescriptorSetInputAttachments);
    fprintf(out, "      \"maxVertexInputAttributes\": %u,\n", l->maxVertexInputAttributes);
    fprintf(out, "      \"maxVertexInputBindings\": %u,\n", l->maxVertexInputBindings);
    fprintf(out, "      \"maxVertexInputAttributeOffset\": %u,\n", l->maxVertexInputAttributeOffset);
    fprintf(out, "      \"maxVertexInputBindingStride\": %u,\n", l->maxVertexInputBindingStride);
    fprintf(out, "      \"maxVertexOutputComponents\": %u,\n", l->maxVertexOutputComponents);
    fprintf(out, "      \"maxTessellationGenerationLevel\": %u,\n", l->maxTessellationGenerationLevel);
    fprintf(out, "      \"maxTessellationPatchSize\": %u,\n", l->maxTessellationPatchSize);
    fprintf(out, "      \"maxTessellationControlPerVertexInputComponents\": %u,\n", l->maxTessellationControlPerVertexInputComponents);
    fprintf(out, "      \"maxTessellationControlPerVertexOutputComponents\": %u,\n", l->maxTessellationControlPerVertexOutputComponents);
    fprintf(out, "      \"maxTessellationControlPerPatchOutputComponents\": %u,\n", l->maxTessellationControlPerPatchOutputComponents);
    fprintf(out, "      \"maxTessellationControlTotalOutputComponents\": %u,\n", l->maxTessellationControlTotalOutputComponents);
    fprintf(out, "      \"maxTessellationEvaluationInputComponents\": %u,\n", l->maxTessellationEvaluationInputComponents);
    fprintf(out, "      \"maxTessellationEvaluationOutputComponents\": %u,\n", l->maxTessellationEvaluationOutputComponents);
    fprintf(out, "      \"maxGeometryShaderInvocations\": %u,\n", l->maxGeometryShaderInvocations);
    fprintf(out, "      \"maxGeometryInputComponents\": %u,\n", l->maxGeometryInputComponents);
    fprintf(out, "      \"maxGeometryOutputComponents\": %u,\n", l->maxGeometryOutputComponents);
    fprintf(out, "      \"maxGeometryOutputVertices\": %u,\n", l->maxGeometryOutputVertices);
    fprintf(out, "      \"maxGeometryTotalOutputComponents\": %u,\n", l->maxGeometryTotalOutputComponents);
    fprintf(out, "      \"maxFragmentInputComponents\": %u,\n", l->maxFragmentInputComponents);
    fprintf(out, "      \"maxFragmentOutputAttachments\": %u,\n", l->maxFragmentOutputAttachments);
    fprintf(out, "      \"maxFragmentCombinedOutputResources\": %u,\n", l->maxFragmentCombinedOutputResources);
    fprintf(out, "      \"maxFragmentDualSrcAttachments\": %u,\n", l->maxFragmentDualSrcAttachments);
    fprintf(out, "      \"maxComputeSharedMemorySize\": %u,\n", l->maxComputeSharedMemorySize);
    fprintf(out, "      \"maxComputeWorkGroupCount\": [%u, %u, %u],\n",
        l->maxComputeWorkGroupCount[0], l->maxComputeWorkGroupCount[1], l->maxComputeWorkGroupCount[2]);
    fprintf(out, "      \"maxComputeWorkGroupInvocations\": %u,\n", l->maxComputeWorkGroupInvocations);
    fprintf(out, "      \"maxComputeWorkGroupSize\": [%u, %u, %u],\n",
        l->maxComputeWorkGroupSize[0], l->maxComputeWorkGroupSize[1], l->maxComputeWorkGroupSize[2]);
    fprintf(out, "      \"subPixelPrecisionBits\": %u,\n", l->subPixelPrecisionBits);
    fprintf(out, "      \"subTexelPrecisionBits\": %u,\n", l->subTexelPrecisionBits);
    fprintf(out, "      \"mipmapPrecisionBits\": %u,\n", l->mipmapPrecisionBits);
    fprintf(out, "      \"maxDrawIndexedIndexValue\": %u,\n", l->maxDrawIndexedIndexValue);
    fprintf(out, "      \"maxDrawIndirectCount\": %u,\n", l->maxDrawIndirectCount);
    fputs("      \"maxSamplerLodBias\": ", out); json_float(out, l->maxSamplerLodBias); fputs(",\n", out);
    fputs("      \"maxSamplerAnisotropy\": ", out); json_float(out, l->maxSamplerAnisotropy); fputs(",\n", out);
    fprintf(out, "      \"maxViewports\": %u,\n", l->maxViewports);
    fprintf(out, "      \"maxViewportDimensions\": [%u, %u],\n",
        l->maxViewportDimensions[0], l->maxViewportDimensions[1]);
    fputs("      \"viewportBoundsRange\": [", out);
    json_float(out, l->viewportBoundsRange[0]); fputs(", ", out);
    json_float(out, l->viewportBoundsRange[1]); fputs("],\n", out);
    fprintf(out, "      \"viewportSubPixelBits\": %u,\n", l->viewportSubPixelBits);
    fprintf(out, "      \"minMemoryMapAlignment\": %llu,\n", (unsigned long long)l->minMemoryMapAlignment);
    fprintf(out, "      \"minTexelBufferOffsetAlignment\": %llu,\n", (unsigned long long)l->minTexelBufferOffsetAlignment);
    fprintf(out, "      \"minUniformBufferOffsetAlignment\": %llu,\n", (unsigned long long)l->minUniformBufferOffsetAlignment);
    fprintf(out, "      \"minStorageBufferOffsetAlignment\": %llu,\n", (unsigned long long)l->minStorageBufferOffsetAlignment);
    fprintf(out, "      \"minTexelOffset\": %d,\n", l->minTexelOffset);
    fprintf(out, "      \"maxTexelOffset\": %u,\n", l->maxTexelOffset);
    fprintf(out, "      \"minTexelGatherOffset\": %d,\n", l->minTexelGatherOffset);
    fprintf(out, "      \"maxTexelGatherOffset\": %u,\n", l->maxTexelGatherOffset);
    fputs("      \"minInterpolationOffset\": ", out); json_float(out, l->minInterpolationOffset); fputs(",\n", out);
    fputs("      \"maxInterpolationOffset\": ", out); json_float(out, l->maxInterpolationOffset); fputs(",\n", out);
    fprintf(out, "      \"subPixelInterpolationOffsetBits\": %u,\n", l->subPixelInterpolationOffsetBits);
    fprintf(out, "      \"maxFramebufferWidth\": %u,\n", l->maxFramebufferWidth);
    fprintf(out, "      \"maxFramebufferHeight\": %u,\n", l->maxFramebufferHeight);
    fprintf(out, "      \"maxFramebufferLayers\": %u,\n", l->maxFramebufferLayers);
    fprintf(out, "      \"framebufferColorSampleCounts\": %u,\n", l->framebufferColorSampleCounts);
    fprintf(out, "      \"framebufferDepthSampleCounts\": %u,\n", l->framebufferDepthSampleCounts);
    fprintf(out, "      \"framebufferStencilSampleCounts\": %u,\n", l->framebufferStencilSampleCounts);
    fprintf(out, "      \"framebufferNoAttachmentsSampleCounts\": %u,\n", l->framebufferNoAttachmentsSampleCounts);
    fprintf(out, "      \"maxColorAttachments\": %u,\n", l->maxColorAttachments);
    fprintf(out, "      \"sampledImageColorSampleCounts\": %u,\n", l->sampledImageColorSampleCounts);
    fprintf(out, "      \"sampledImageIntegerSampleCounts\": %u,\n", l->sampledImageIntegerSampleCounts);
    fprintf(out, "      \"sampledImageDepthSampleCounts\": %u,\n", l->sampledImageDepthSampleCounts);
    fprintf(out, "      \"sampledImageStencilSampleCounts\": %u,\n", l->sampledImageStencilSampleCounts);
    fprintf(out, "      \"storageImageSampleCounts\": %u,\n", l->storageImageSampleCounts);
    fprintf(out, "      \"maxSampleMaskWords\": %u,\n", l->maxSampleMaskWords);
    fputs("      \"timestampComputeAndGraphics\": ", out); json_bool(out, l->timestampComputeAndGraphics); fputs(",\n", out);
    fputs("      \"timestampPeriod\": ", out); json_float(out, l->timestampPeriod); fputs(",\n", out);
    fprintf(out, "      \"maxClipDistances\": %u,\n", l->maxClipDistances);
    fprintf(out, "      \"maxCullDistances\": %u,\n", l->maxCullDistances);
    fprintf(out, "      \"maxCombinedClipAndCullDistances\": %u,\n", l->maxCombinedClipAndCullDistances);
    fprintf(out, "      \"discreteQueuePriorities\": %u,\n", l->discreteQueuePriorities);
    fputs("      \"pointSizeRange\": [", out);
    json_float(out, l->pointSizeRange[0]); fputs(", ", out);
    json_float(out, l->pointSizeRange[1]); fputs("],\n", out);
    fputs("      \"lineWidthRange\": [", out);
    json_float(out, l->lineWidthRange[0]); fputs(", ", out);
    json_float(out, l->lineWidthRange[1]); fputs("],\n", out);
    fputs("      \"pointSizeGranularity\": ", out); json_float(out, l->pointSizeGranularity); fputs(",\n", out);
    fputs("      \"lineWidthGranularity\": ", out); json_float(out, l->lineWidthGranularity); fputs(",\n", out);
    fputs("      \"strictLines\": ", out); json_bool(out, l->strictLines); fputs(",\n", out);
    fputs("      \"standardSampleLocations\": ", out); json_bool(out, l->standardSampleLocations); fputs(",\n", out);
    fprintf(out, "      \"optimalBufferCopyOffsetAlignment\": %llu,\n", (unsigned long long)l->optimalBufferCopyOffsetAlignment);
    fprintf(out, "      \"optimalBufferCopyRowPitchAlignment\": %llu,\n", (unsigned long long)l->optimalBufferCopyRowPitchAlignment);
    fprintf(out, "      \"nonCoherentAtomSize\": %llu\n", (unsigned long long)l->nonCoherentAtomSize);
    fputs("    }", out);
}

/* Every VkPhysicalDeviceFeatures member, all of which are reported as zeroed
 * for Vulkan 1.0 core. Print them explicitly so the matrix can prove that no
 * bit is silently true. */
static const struct { const char *name; size_t offset; } feature_members[] = {
#define FEATURE(name) { #name, offsetof(VkPhysicalDeviceFeatures, name) }
    FEATURE(robustBufferAccess), FEATURE(fullDrawIndexUint32), FEATURE(imageCubeArray),
    FEATURE(independentBlend), FEATURE(geometryShader), FEATURE(tessellationShader),
    FEATURE(sampleRateShading), FEATURE(dualSrcBlend), FEATURE(logicOp),
    FEATURE(multiDrawIndirect), FEATURE(drawIndirectFirstInstance), FEATURE(depthClamp),
    FEATURE(depthBiasClamp), FEATURE(fillModeNonSolid), FEATURE(depthBounds),
    FEATURE(wideLines), FEATURE(largePoints), FEATURE(alphaToOne),
    FEATURE(multiViewport), FEATURE(samplerAnisotropy), FEATURE(textureCompressionETC2),
    FEATURE(textureCompressionASTC_LDR), FEATURE(textureCompressionBC),
    FEATURE(occlusionQueryPrecise), FEATURE(pipelineStatisticsQuery),
    FEATURE(vertexPipelineStoresAndAtomics), FEATURE(fragmentStoresAndAtomics),
    FEATURE(shaderTessellationAndGeometryPointSize), FEATURE(shaderImageGatherExtended),
    FEATURE(shaderStorageImageExtendedFormats), FEATURE(shaderStorageImageMultisample),
    FEATURE(shaderStorageImageReadWithoutFormat), FEATURE(shaderStorageImageWriteWithoutFormat),
    FEATURE(shaderUniformBufferArrayDynamicIndexing), FEATURE(shaderSampledImageArrayDynamicIndexing),
    FEATURE(shaderStorageBufferArrayDynamicIndexing), FEATURE(shaderStorageImageArrayDynamicIndexing),
    FEATURE(shaderClipDistance), FEATURE(shaderCullDistance), FEATURE(shaderFloat64),
    FEATURE(shaderInt64), FEATURE(shaderInt16), FEATURE(shaderResourceResidency),
    FEATURE(shaderResourceMinLod), FEATURE(sparseBinding), FEATURE(sparseResidencyBuffer),
    FEATURE(sparseResidencyImage2D), FEATURE(sparseResidencyImage3D),
    FEATURE(sparseResidency2Samples), FEATURE(sparseResidency4Samples),
    FEATURE(sparseResidency8Samples), FEATURE(sparseResidency16Samples),
    FEATURE(sparseResidencyAliased), FEATURE(variableMultisampleRate), FEATURE(inheritedQueries)
#undef FEATURE
};

static const VkFormat dump_formats[] = {
    VK_FORMAT_R4G4_UNORM_PACK8, VK_FORMAT_R4G4B4A4_UNORM_PACK16,
    VK_FORMAT_B4G4R4A4_UNORM_PACK16, VK_FORMAT_R5G6B5_UNORM_PACK16,
    VK_FORMAT_B5G6R5_UNORM_PACK16, VK_FORMAT_R5G5B5A1_UNORM_PACK16,
    VK_FORMAT_B5G5R5A1_UNORM_PACK16, VK_FORMAT_A1R5G5B5_UNORM_PACK16,
    VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED,
    VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM,
    VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_UNORM,
    VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT,
    VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_B8G8R8_SNORM, VK_FORMAT_B8G8R8_UINT,
    VK_FORMAT_B8G8R8_SINT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
    VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_R8G8B8A8_UINT,
    VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SNORM,
    VK_FORMAT_B8G8R8A8_UINT, VK_FORMAT_B8G8R8A8_SINT, VK_FORMAT_A8B8G8R8_UNORM_PACK32,
    VK_FORMAT_A8B8G8R8_SNORM_PACK32, VK_FORMAT_A8B8G8R8_UINT_PACK32,
    VK_FORMAT_A8B8G8R8_SINT_PACK32, VK_FORMAT_A8B8G8R8_SRGB_PACK32,
    VK_FORMAT_A2B10G10R10_UNORM_PACK32,
    VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM,
    VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_UNORM,
    VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_SFLOAT,
    VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT,
    VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UINT,
    VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32_SFLOAT,
    VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_SFLOAT,
    VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_SFLOAT,
    VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_SFLOAT,
    VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R16G16_SNORM,
    VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
    VK_FORMAT_B10G11R11_UFLOAT_PACK32,
    VK_FORMAT_D16_UNORM, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT,
    VK_FORMAT_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
    VK_FORMAT_D32_SFLOAT_S8_UINT,
    VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK,
    VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
    VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK,
    VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK,
    VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK,
    VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK,
    VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC6H_SFLOAT_BLOCK,
    VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK,
};

static void print_format_properties(FILE *out)
{
    fputs("  \"formats\": {\n", out);
    for (size_t i = 0; i < sizeof(dump_formats) / sizeof(dump_formats[0]); ++i) {
        VkFormat format = dump_formats[i];
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(dump_physical, format, &properties);
        fprintf(out, "    \"%u\": {\"linearTilingFeatures\": %u, \"optimalTilingFeatures\": %u, "
                     "\"bufferFeatures\": %u}%s\n",
            (unsigned)format, properties.linearTilingFeatures, properties.optimalTilingFeatures,
            properties.bufferFeatures,
            i + 1 == sizeof(dump_formats) / sizeof(dump_formats[0]) ? "" : ",");
    }
    fputs("  },\n", out);
}

/* The capability ledger, read through the authoritative table accessors: for
 * every format the driver knows, which operations are implemented and which of
 * them have an on-console witness. The published feature bits must equal the
 * witnessed column, so a capability cannot be re-advertised without a backend
 * or silently advertised without evidence. */
static void print_format_capabilities(FILE *out)
{
    fputs("  \"formatCapabilities\": [\n", out);
    const unsigned count = ps5vk_texture_format_count();
    for (unsigned i = 0; i < count; ++i) {
        const struct ps5vk_texture_format *entry = ps5vk_texture_format_at(i);
        VkFormatProperties properties;
        ps5vk_texture_format_properties(entry->format, &properties);
        fprintf(out,
            "    {\"format\": %u, \"bytesPerTexel\": %u, \"blockWidth\": %u, "
            "\"blockHeight\": %u, \"bytesPerBlock\": %u, \"descriptorFormatWord\": %u, "
            "\"selectors\": [%u, %u, %u, %u], \"capabilities\": %u, \"witnessed\": %u, "
            "\"provenance\": %u, \"optimalTilingFeatures\": %u, \"bufferFeatures\": %u}%s\n",
            (unsigned)entry->format, entry->bytes_per_texel, entry->block_width,
            entry->block_height, entry->bytes_per_block, entry->descriptor_format_word,
            entry->selectors[0], entry->selectors[1], entry->selectors[2],
            entry->selectors[3], entry->capabilities, entry->witnessed,
            (unsigned)entry->provenance, properties.optimalTilingFeatures,
            properties.bufferFeatures,
            i + 1 == count ? "" : ",");
    }
    fputs("  ],\n", out);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--compute") == 0) {
        graphics_objects = 0;
        graphics_submit = 0;
    }
    VkInstance instance;
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (vkCreateInstance(&instance_info, NULL, &instance) != VK_SUCCESS) return 1;
    uint32_t count = 1;
    if (vkEnumeratePhysicalDevices(instance, &count, &dump_physical) != VK_SUCCESS) return 1;

    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceProperties(dump_physical, &properties);
    vkGetPhysicalDeviceMemoryProperties(dump_physical, &memory);
    vkGetPhysicalDeviceFeatures(dump_physical, &features);

    fputs("{\n", stdout);
    fprintf(stdout, "  \"profile\": \"%s\",\n", graphics_submit ? "graphics" : "compute");
    fprintf(stdout, "  \"apiVersion\": %u,\n", properties.apiVersion);
    fprintf(stdout, "  \"driverVersion\": %u,\n", properties.driverVersion);
    fprintf(stdout, "  \"vendorID\": %u,\n", properties.vendorID);
    fprintf(stdout, "  \"deviceID\": %u,\n", properties.deviceID);
    fprintf(stdout, "  \"deviceType\": %u,\n", properties.deviceType);
    fprintf(stdout, "  \"deviceName\": \"%s\",\n", properties.deviceName);
    fputs("  \"limits\": ", stdout);
    print_limits(stdout, &properties.limits);
    fputs(",\n", stdout);

    fputs("  \"features\": {\n", stdout);
    for (size_t i = 0; i < sizeof(feature_members) / sizeof(feature_members[0]); ++i) {
        VkBool32 value = *(const VkBool32 *)((const char *)&features + feature_members[i].offset);
        fprintf(stdout, "    \"%s\": %s%s\n", feature_members[i].name, value ? "true" : "false",
            i + 1 == sizeof(feature_members) / sizeof(feature_members[0]) ? "" : ",");
    }
    fputs("  },\n", stdout);

    /* Extension feature structs reachable through the advertised
     * VK_KHR_get_physical_device_properties2 chain. */
    VkPhysicalDevice8BitStorageFeatures storage8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
    VkPhysicalDevice16BitStorageFeatures storage16 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceProtectedMemoryFeatures protected_memory = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES};
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    VkPhysicalDeviceMultiviewFeatures multiview = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceUniformBufferStandardLayoutFeatures standard_ubo = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES};
    VkPhysicalDeviceVulkanMemoryModelFeaturesKHR memory_model = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES_KHR};
    VkPhysicalDeviceBufferDeviceAddressFeaturesKHR device_address = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR};

    VkPhysicalDeviceHostQueryResetFeaturesEXT host_query_reset = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT};

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures separate_layouts = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures demote = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT transform_feedback = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceShaderTerminateInvocationFeatures terminate = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES};
    VkPhysicalDeviceSynchronization2Features synchronization2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};

    VkPhysicalDeviceMultiviewProperties multiview_properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES};
    VkPhysicalDeviceTimelineSemaphoreProperties timeline_properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES};
    uint32_t extensions = 0;
    (void)vkEnumerateDeviceExtensionProperties(dump_physical, NULL, &extensions, NULL);
    char **extension_names = extensions ? calloc(extensions, sizeof(*extension_names)) : NULL;
    VkExtensionProperties *extension_properties = extensions ? calloc(extensions, sizeof(*extension_properties)) : NULL;
    if (extension_names && extension_properties &&
        vkEnumerateDeviceExtensionProperties(dump_physical, NULL, &extensions, extension_properties) == VK_SUCCESS) {
        for (uint32_t i = 0; i < extensions; ++i) extension_names[i] = extension_properties[i].extensionName;
    }
    storage8.pNext = &storage16;
    storage16.pNext = &protected_memory;
    protected_memory.pNext = &draw_parameters;
    draw_parameters.pNext = &multiview;
    multiview.pNext = &standard_ubo;
    standard_ubo.pNext = &memory_model;
    memory_model.pNext = &device_address;

    device_address.pNext = &host_query_reset;
    host_query_reset.pNext = &timeline;
    timeline.pNext = &separate_layouts;
    separate_layouts.pNext = &demote;
    demote.pNext = &terminate;
    terminate.pNext = &synchronization2;
    synchronization2.pNext = &transform_feedback;
    multiview_properties.pNext = &timeline_properties;

    VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                           .pNext = &storage8};
    vkGetPhysicalDeviceFeatures2KHR(dump_physical, &features2);
    VkPhysicalDeviceProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &multiview_properties};
    vkGetPhysicalDeviceProperties2KHR(dump_physical, &properties2);
    fprintf(stdout, "  \"multiviewQuery\": {\"route\": \"VK_KHR_multiview\", "
        "\"multiview\": %s, \"maxMultiviewViewCount\": %u, "
        "\"maxMultiviewInstanceIndex\": %u},\n",
        multiview.multiview ? "true" : "false", multiview_properties.maxMultiviewViewCount,
        multiview_properties.maxMultiviewInstanceIndex);
    fprintf(stdout, "  \"standardUBOQuery\": {\"route\": "
                    "\"VK_KHR_uniform_buffer_standard_layout\", "
                    "\"uniformBufferStandardLayout\": %s},\n",
        standard_ubo.uniformBufferStandardLayout ? "true" : "false");
    fprintf(stdout, "  \"memoryModelQuery\": {\"route\": "
                    "\"VK_KHR_vulkan_memory_model\", "
                    "\"vulkanMemoryModel\": %s, "
                    "\"vulkanMemoryModelDeviceScope\": %s},\n",
        memory_model.vulkanMemoryModel ? "true" : "false",
        memory_model.vulkanMemoryModelDeviceScope ? "true" : "false");
    fprintf(stdout, "  \"bufferDeviceAddressQuery\": {\"route\": "
                    "\"VK_KHR_buffer_device_address\", "
                    "\"bufferDeviceAddress\": %s, "
                    "\"bufferDeviceAddressCaptureReplay\": %s, "
                    "\"bufferDeviceAddressMultiDevice\": %s},\n",
        device_address.bufferDeviceAddress ? "true" : "false",
        device_address.bufferDeviceAddressCaptureReplay ? "true" : "false",
        device_address.bufferDeviceAddressMultiDevice ? "true" : "false");

    fprintf(stdout, "  \"hostQueryResetQuery\": {\"route\": "
                    "\"VK_EXT_host_query_reset\", \"hostQueryReset\": %s},\n",
        host_query_reset.hostQueryReset ? "true" : "false");

    fprintf(stdout, "  \"timelineSemaphoreQuery\": {\"route\": "
                    "\"VK_KHR_timeline_semaphore\", "
                    "\"timelineSemaphore\": %s, "
                    "\"maxTimelineSemaphoreValueDifference\": %llu},\n",
        timeline.timelineSemaphore ? "true" : "false",
        (unsigned long long)timeline_properties.maxTimelineSemaphoreValueDifference);
    fprintf(stdout, "  \"separateDepthStencilLayoutsQuery\": {\"route\": "
                    "\"VK_KHR_separate_depth_stencil_layouts\", "
                    "\"separateDepthStencilLayouts\": %s},\n",
        separate_layouts.separateDepthStencilLayouts ? "true" : "false");
    /* One entry per single-feature extension route: the extension that
     * carries the promoted Vulkan 1.2/1.3 feature on this 1.0 device and the
     * value its own feature structure reports. */
    fprintf(stdout, "  \"extensionRouteQueries\": {\n"
                    "    \"VK_EXT_shader_demote_to_helper_invocation\": "
                    "{\"shaderDemoteToHelperInvocation\": %s},\n"
                    "    \"VK_KHR_shader_terminate_invocation\": "
                    "{\"shaderTerminateInvocation\": %s},\n"
                    "    \"VK_KHR_synchronization2\": "
                    "{\"synchronization2\": %s},\n"
                    "    \"VK_EXT_transform_feedback\": "
                    "{\"transformFeedback\": %s, \"geometryStreams\": %s}\n"
                    "  },\n",
        demote.shaderDemoteToHelperInvocation ? "true" : "false",
        terminate.shaderTerminateInvocation ? "true" : "false",
        synchronization2.synchronization2 ? "true" : "false",
        transform_feedback.transformFeedback ? "true" : "false",
        transform_feedback.geometryStreams ? "true" : "false");

    fprintf(stdout, "  \"extensionCount\": %u,\n", extensions);
    fputs("  \"extensions\": [", stdout);
    for (uint32_t i = 0; i < extensions; ++i)
        fprintf(stdout, "%s\"%s\"", i ? ", " : "", extension_names[i] ? extension_names[i] : "");
    fputs("],\n", stdout);
    fprintf(stdout, "  \"extensionFeatures\": {\n"
                    "    \"storageBuffer8BitAccess\": %s,\n"
                    "    \"uniformAndStorageBuffer8BitAccess\": %s,\n"
                    "    \"storagePushConstant8\": %s,\n"
                    "    \"storageBuffer16BitAccess\": %s,\n"
                    "    \"uniformAndStorageBuffer16BitAccess\": %s,\n"
                    "    \"storagePushConstant16\": %s,\n"
                    "    \"storageInputOutput16\": %s,\n"
                    "    \"protectedMemory\": %s,\n"
                    "    \"shaderDrawParameters\": %s,\n"
                    "    \"uniformBufferStandardLayout\": %s,\n"
                    "    \"vulkanMemoryModel\": %s,\n"
                    "    \"vulkanMemoryModelDeviceScope\": %s,\n"
                    "    \"bufferDeviceAddress\": %s,\n"

                    "    \"hostQueryReset\": %s,\n"
                    "    \"timelineSemaphore\": %s,\n"
                    "    \"separateDepthStencilLayouts\": %s,\n"
                    "    \"shaderDemoteToHelperInvocation\": %s,\n"
                    "    \"shaderTerminateInvocation\": %s,\n"
                    "    \"synchronization2\": %s,\n"
                    "    \"transformFeedback\": %s,\n"
                    "    \"geometryStreams\": %s\n"

                    "  },\n",
        storage8.storageBuffer8BitAccess ? "true" : "false",
        storage8.uniformAndStorageBuffer8BitAccess ? "true" : "false",
        storage8.storagePushConstant8 ? "true" : "false",
        storage16.storageBuffer16BitAccess ? "true" : "false",
        storage16.uniformAndStorageBuffer16BitAccess ? "true" : "false",
        storage16.storagePushConstant16 ? "true" : "false",
        storage16.storageInputOutput16 ? "true" : "false",
        protected_memory.protectedMemory ? "true" : "false",
        draw_parameters.shaderDrawParameters ? "true" : "false",
        standard_ubo.uniformBufferStandardLayout ? "true" : "false",
        memory_model.vulkanMemoryModel ? "true" : "false",
        memory_model.vulkanMemoryModelDeviceScope ? "true" : "false",
        device_address.bufferDeviceAddress ? "true" : "false",

        host_query_reset.hostQueryReset ? "true" : "false",
        timeline.timelineSemaphore ? "true" : "false",
        separate_layouts.separateDepthStencilLayouts ? "true" : "false",
        demote.shaderDemoteToHelperInvocation ? "true" : "false",
        terminate.shaderTerminateInvocation ? "true" : "false",
        synchronization2.synchronization2 ? "true" : "false",
        transform_feedback.transformFeedback ? "true" : "false",
        transform_feedback.geometryStreams ? "true" : "false");

    free(extension_names);
    free(extension_properties);

    fprintf(stdout, "  \"memoryTypeCount\": %u,\n", memory.memoryTypeCount);
    fprintf(stdout, "  \"memoryHeapCount\": %u,\n", memory.memoryHeapCount);
    fprintf(stdout, "  \"memoryTypes\": [");
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        fprintf(stdout, "%s{\"propertyFlags\": %u, \"heapIndex\": %u}",
            i ? ", " : "", memory.memoryTypes[i].propertyFlags, memory.memoryTypes[i].heapIndex);
    fputs("],\n", stdout);
    fprintf(stdout, "  \"memoryHeaps\": [");
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
        fprintf(stdout, "%s{\"size\": %llu, \"flags\": %u}",
            i ? ", " : "", (unsigned long long)memory.memoryHeaps[i].size, memory.memoryHeaps[i].flags);
    fputs("],\n", stdout);

    print_format_properties(stdout);
    print_format_capabilities(stdout);

    /* Image-format queries: the combinations the advertised matrix claims,
     * plus the mandatory 2D/optimal/buffer scopes, so the matrix tool can show
     * exactly which scope answered VK_SUCCESS and with what ceilings. */
    fputs("  \"imageFormatProperties\": [\n", stdout);
    const struct { VkFormat format; VkImageType type; VkImageTiling tiling; VkImageUsageFlags usage; } queries[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_SAMPLED_BIT},
        {VK_FORMAT_R8G8B8A8_UINT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT},
        {VK_FORMAT_R5G6B5_UNORM_PACK16, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT},
        {VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT},
        {VK_FORMAT_S8_UINT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT},
    };
    /* Cross-check matrix: every advertised format against the usages that have
     * a VkFormatFeatureFlagBits counterpart, in both tiling scopes. The audit
     * tool requires the two query paths to agree. */
    const VkFormat advertised[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
                                   VK_FORMAT_D32_SFLOAT};
    const VkImageUsageFlags usages[] = {VK_IMAGE_USAGE_SAMPLED_BIT,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                        VK_IMAGE_USAGE_STORAGE_BIT};
    const VkImageTiling tilings[] = {VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_TILING_LINEAR};
    size_t query_count = sizeof(queries) / sizeof(queries[0]);
    size_t matrix_count = sizeof(advertised) / sizeof(advertised[0]) *
                          sizeof(usages) / sizeof(usages[0]) *
                          sizeof(tilings) / sizeof(tilings[0]);
    for (size_t i = 0; i < query_count + matrix_count; ++i) {
        VkFormat format;
        VkImageType type = VK_IMAGE_TYPE_2D;
        VkImageTiling tiling;
        VkImageUsageFlags usage;
        if (i < query_count) {
            format = queries[i].format; type = queries[i].type;
            tiling = queries[i].tiling; usage = queries[i].usage;
        } else {
            size_t index = i - query_count;
            format = advertised[index / (sizeof(usages) / sizeof(usages[0]) *
                                          sizeof(tilings) / sizeof(tilings[0]))];
            usage = usages[(index / (sizeof(tilings) / sizeof(tilings[0]))) %
                           (sizeof(usages) / sizeof(usages[0]))];
            tiling = tilings[index % (sizeof(tilings) / sizeof(tilings[0]))];
        }
        VkImageFormatProperties properties_out;
        VkResult result = vkGetPhysicalDeviceImageFormatProperties(dump_physical, format,
            type, tiling, usage, 0, &properties_out);
        fprintf(stdout, "    {\"format\": %u, \"type\": %u, \"tiling\": %u, \"usage\": %u, \"result\": %d",
            (unsigned)format, (unsigned)type, (unsigned)tiling, usage, (int)result);
        if (result == VK_SUCCESS)
            fprintf(stdout, ", \"maxExtent\": [%u, %u, %u], \"maxMipLevels\": %u, \"maxArrayLayers\": %u, \"sampleCounts\": %u, \"maxResourceSize\": %llu",
                properties_out.maxExtent.width, properties_out.maxExtent.height, properties_out.maxExtent.depth,
                properties_out.maxMipLevels, properties_out.maxArrayLayers, properties_out.sampleCounts,
                (unsigned long long)properties_out.maxResourceSize);
        fprintf(stdout, "}%s\n", i + 1 == query_count + matrix_count ? "" : ",");
    }
    fputs("  ],\n  \"resourceContractWitness\": {\n", stdout);
    print_resource_contract_witness(stdout);
    fputs("  }\n}\n", stdout);

    vkDestroyInstance(instance, NULL);
    return 0;
}
