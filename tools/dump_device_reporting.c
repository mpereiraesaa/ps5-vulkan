/*
 * Dump what ps5vk reports through the public Vulkan 1.0 query paths, as JSON.
 *
 * This is a host tool: it installs a platform whose properties come from the
 * same initializer the native console platform uses
 * (src/device_profile_report.h), then asks the driver through the real entry
 * points - vkGetPhysicalDeviceProperties, vkGetPhysicalDeviceFeatures,
 * vkGetPhysicalDeviceMemoryProperties, vkGetPhysicalDeviceFormatProperties and
 * vkGetPhysicalDeviceImageFormatProperties - rather than reading C constants.
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
    platform->supported_features = PS5VK_FEATURE_ROBUST_BUFFER_ACCESS;
    if (!graphics_objects)
        platform->supported_features |= PS5VK_FEATURE_STORAGE_BUFFER_8BIT |
                                        PS5VK_FEATURE_STORAGE_BUFFER_16BIT;
    platform->max_allocation = ps5vk_device_profile_heap_bytes(graphics_objects);
    ps5vk_device_profile_init(&platform->properties, &platform->memory_properties,
        graphics_objects, graphics_submit);
    return VK_SUCCESS;
}

static void json_float(FILE *out, float value)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9g", (double)value);
    fputs(buffer, out);
}

static void json_bool(FILE *out, VkBool32 value) { fputs(value ? "true" : "false", out); }

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
    VK_FORMAT_A8B8G8R8_SINT_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
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
    VK_FORMAT_D16_UNORM, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT,
    VK_FORMAT_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
    VK_FORMAT_D32_SFLOAT_S8_UINT,
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
    VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                           .pNext = &storage8};
    vkGetPhysicalDeviceFeatures2KHR(dump_physical, &features2);
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
                    "    \"shaderDrawParameters\": %s\n"
                    "  },\n",
        storage8.storageBuffer8BitAccess ? "true" : "false",
        storage8.uniformAndStorageBuffer8BitAccess ? "true" : "false",
        storage8.storagePushConstant8 ? "true" : "false",
        storage16.storageBuffer16BitAccess ? "true" : "false",
        storage16.uniformAndStorageBuffer16BitAccess ? "true" : "false",
        storage16.storagePushConstant16 ? "true" : "false",
        storage16.storageInputOutput16 ? "true" : "false",
        protected_memory.protectedMemory ? "true" : "false",
        draw_parameters.shaderDrawParameters ? "true" : "false");
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
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
        VkImageFormatProperties properties_out;
        VkResult result = vkGetPhysicalDeviceImageFormatProperties(dump_physical, queries[i].format,
            queries[i].type, queries[i].tiling, queries[i].usage, 0, &properties_out);
        fprintf(stdout, "    {\"format\": %u, \"type\": %u, \"tiling\": %u, \"usage\": %u, \"result\": %d",
            (unsigned)queries[i].format, (unsigned)queries[i].type, (unsigned)queries[i].tiling,
            queries[i].usage, (int)result);
        if (result == VK_SUCCESS)
            fprintf(stdout, ", \"maxExtent\": [%u, %u, %u], \"maxMipLevels\": %u, \"maxArrayLayers\": %u, \"sampleCounts\": %u, \"maxResourceSize\": %llu",
                properties_out.maxExtent.width, properties_out.maxExtent.height, properties_out.maxExtent.depth,
                properties_out.maxMipLevels, properties_out.maxArrayLayers, properties_out.sampleCounts,
                (unsigned long long)properties_out.maxResourceSize);
        fprintf(stdout, "}%s\n", i + 1 == sizeof(queries) / sizeof(queries[0]) ? "" : ",");
    }
    fputs("  ]\n}\n", stdout);

    vkDestroyInstance(instance, NULL);
    return 0;
}
