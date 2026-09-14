#ifndef PS5VK_GRAPHICS_LIMITS_H
#define PS5VK_GRAPHICS_LIMITS_H
#include <vulkan/vulkan_core.h>
/* Execution envelope derived from native viewport/target/layout/fetch code.
 * Not Vulkan minimum-limit compliance or validation at maximum dimensions.
 * Texture precision/inter-stage limits are not inferred from GPU branding. */
enum {
    PS5VK_MAX_IMAGE_2D = 16384,
    /* Vulkan 1.0 floors. The GFX10 descriptor fields cover these dimensions;
     * allocation-size and format queries still bound each concrete image. */
    PS5VK_MAX_IMAGE_1D = 4096,
    PS5VK_MAX_IMAGE_3D = 512,
    PS5VK_MAX_IMAGE_CUBE = 4096,
    PS5VK_MAX_IMAGE_ARRAY_LAYERS = 256,
    PS5VK_MAX_COLOR_DIMENSION = 16383,
    PS5VK_MAX_VERTEX_STRIDE = 16380,
    PS5VK_MAX_VERTEX_ATTRIBUTE_OFFSET = 16376,
    /* Vulkan 1.0 mandatory floor. The signed 8.8 GFX1013 sampler field has a
     * wider range; expose only the hardware-qualified core interval. */
    PS5VK_MAX_SAMPLER_LOD_BIAS = 2,
    /* PA_SU_VTX_CNTL QUANT_MODE=5 selects 1/256 framebuffer coordinates.
     * Compiler, pair preparation and draw-state gates require word 0x2d. */
    PS5VK_SUBPIXEL_BITS = 8,
    /* Software object budget, not a measured hardware sampler limit. */
    PS5VK_MAX_SAMPLERS = 4096,
    /* Sampled-descriptor minima this frontend actually reaches. They are the
     * single source for the four reported values below and for the matching
     * profile validation, so the report cannot drift from the contract it was
     * qualified against. Evidence: VALIDATION.md records 96 combined image
     * samplers inside one set, and 96 across four sets from one stage. The
     * implemented capacity of one set and of one stage is PS5VK_MAX_DESCRIPTORS
     * records (vk_descriptor.h), which is what the validation bounds. */
    PS5VK_QUALIFIED_STAGE_SAMPLED_DESCRIPTORS = 16,
    PS5VK_QUALIFIED_SET_SAMPLED_DESCRIPTORS = 96
};
/* Diagnostic consumers ask whether their workload fits, not whether a device
 * still reports the historical ceiling. Hardware-limit policy lives below. */
static inline int ps5vk_graphics_vertex_bindings_available(
    const VkPhysicalDeviceLimits *limits,uint32_t required)
{
    return limits && required<=limits->maxVertexInputBindings;
}
static inline void ps5vk_graphics_limits(VkPhysicalDeviceLimits *limits)
{
    /* Common guarantees across supported sampled-image types. Per-format 2D
     * queries may expose the larger RGBA/depth ceiling, but BGRA stops at
     * 16383. */
    limits->maxImageDimension1D=PS5VK_MAX_IMAGE_1D;
    limits->maxImageDimension2D=PS5VK_MAX_COLOR_DIMENSION;
    limits->maxImageDimension3D=PS5VK_MAX_IMAGE_3D;
    limits->maxImageDimensionCube=PS5VK_MAX_IMAGE_CUBE;
    limits->maxImageArrayLayers=PS5VK_MAX_IMAGE_ARRAY_LAYERS;
    limits->maxColorAttachments=1;
    limits->maxFragmentOutputAttachments=1;
    limits->maxFragmentCombinedOutputResources=1;
    /* The sampled descriptor limits are the qualified minima from the shared
     * constants above, not independent report literals: one set carries the
     * whole descriptor table the runtime draw ABI addresses, and the stage
     * reads as many records as the table layout reserves. Samplers and sampled
     * images share the table, so both limit pairs carry the same values.
     * Compute storage-buffer limits and the shared resource ceiling stay. */
    limits->maxPerStageDescriptorSamplers=PS5VK_QUALIFIED_STAGE_SAMPLED_DESCRIPTORS;
    limits->maxPerStageDescriptorSampledImages=PS5VK_QUALIFIED_STAGE_SAMPLED_DESCRIPTORS;
    limits->maxDescriptorSetSamplers=PS5VK_QUALIFIED_SET_SAMPLED_DESCRIPTORS;
    limits->maxDescriptorSetSampledImages=PS5VK_QUALIFIED_SET_SAMPLED_DESCRIPTORS;
    limits->maxSamplerAllocationCount=PS5VK_MAX_SAMPLERS;
    limits->maxFramebufferWidth=PS5VK_MAX_COLOR_DIMENSION;
    limits->maxFramebufferHeight=PS5VK_MAX_COLOR_DIMENSION;
    limits->maxFramebufferLayers=1;
    limits->framebufferColorSampleCounts=VK_SAMPLE_COUNT_1_BIT;
    limits->framebufferDepthSampleCounts=VK_SAMPLE_COUNT_1_BIT;
    limits->sampledImageColorSampleCounts=VK_SAMPLE_COUNT_1_BIT;
    limits->sampledImageIntegerSampleCounts=VK_SAMPLE_COUNT_1_BIT;
    limits->maxViewports=1;
    limits->maxViewportDimensions[0]=limits->maxViewportDimensions[1]=PS5VK_MAX_IMAGE_2D;
    limits->viewportBoundsRange[0]=-32768.0f;
    limits->viewportBoundsRange[1]=32767.0f;
    /* Optimized PSBC mask drives dense native SRDs; see VERTEX_INPUT.md. */
    limits->maxVertexInputBindings=16;
    limits->maxVertexInputAttributes=32;
    limits->maxVertexInputBindingStride=PS5VK_MAX_VERTEX_STRIDE;
    limits->maxVertexInputAttributeOffset=PS5VK_MAX_VERTEX_ATTRIBUTE_OFFSET;
    limits->maxDrawIndexedIndexValue=UINT32_MAX;
    limits->subPixelPrecisionBits=PS5VK_SUBPIXEL_BITS;
    limits->maxSamplerLodBias=(float)PS5VK_MAX_SAMPLER_LOD_BIAS;
    limits->maxSamplerAnisotropy=1.0f;
}
#endif
