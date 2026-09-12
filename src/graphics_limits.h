#ifndef PS5VK_GRAPHICS_LIMITS_H
#define PS5VK_GRAPHICS_LIMITS_H
#include <vulkan/vulkan_core.h>
/* Execution envelope derived from native viewport/target/layout/fetch code.
 * Not Vulkan minimum-limit compliance or validation at maximum dimensions.
 * Texture precision/inter-stage limits are not inferred from GPU branding. */
enum {
    PS5VK_MAX_IMAGE_2D = 16384,
    PS5VK_MAX_COLOR_DIMENSION = 16383,
    PS5VK_MAX_VERTEX_STRIDE = 16380,
    PS5VK_MAX_VERTEX_ATTRIBUTE_OFFSET = 16376,
    /* PA_SU_VTX_CNTL QUANT_MODE=5 selects 1/256 framebuffer coordinates.
     * Compiler, pair preparation and draw-state gates require word 0x2d. */
    PS5VK_SUBPIXEL_BITS = 8,
    /* Software object budget, not a measured hardware sampler limit. */
    PS5VK_MAX_SAMPLERS = 4096
};
static inline void ps5vk_graphics_limits(VkPhysicalDeviceLimits *limits)
{
    /* Common guarantee across supported 2D formats. Per-format queries may
     * expose the larger RGBA/depth ceiling, but BGRA stops at 16383. */
    limits->maxImageDimension2D=PS5VK_MAX_COLOR_DIMENSION;
    limits->maxImageArrayLayers=1;
    limits->maxColorAttachments=1;
    limits->maxFragmentOutputAttachments=1;
    limits->maxFragmentCombinedOutputResources=1;
    /* The native sampled draw consumes one combined image/sampler at set 0,
     * binding 0. These count against both sampler and sampled-image limits.
     * Preserve compute storage-buffer limits and the shared resource ceiling. */
    limits->maxPerStageDescriptorSamplers=1;
    limits->maxPerStageDescriptorSampledImages=1;
    limits->maxDescriptorSetSamplers=1;
    limits->maxDescriptorSetSampledImages=1;
    limits->maxSamplerAllocationCount=PS5VK_MAX_SAMPLERS;
    limits->maxFramebufferWidth=PS5VK_MAX_COLOR_DIMENSION;
    limits->maxFramebufferHeight=PS5VK_MAX_COLOR_DIMENSION;
    limits->maxFramebufferLayers=1;
    limits->framebufferColorSampleCounts=VK_SAMPLE_COUNT_1_BIT;
    limits->framebufferDepthSampleCounts=VK_SAMPLE_COUNT_1_BIT;
    limits->sampledImageColorSampleCounts=VK_SAMPLE_COUNT_1_BIT;
    limits->maxViewports=1;
    limits->maxViewportDimensions[0]=limits->maxViewportDimensions[1]=PS5VK_MAX_IMAGE_2D;
    limits->viewportBoundsRange[0]=-32768.0f;
    limits->viewportBoundsRange[1]=32767.0f;
    limits->maxVertexInputBindings=1;
    limits->maxVertexInputAttributes=32;
    limits->maxVertexInputBindingStride=PS5VK_MAX_VERTEX_STRIDE;
    limits->maxVertexInputAttributeOffset=PS5VK_MAX_VERTEX_ATTRIBUTE_OFFSET;
    limits->maxDrawIndexedIndexValue=UINT32_MAX;
    limits->subPixelPrecisionBits=PS5VK_SUBPIXEL_BITS;
    limits->maxSamplerLodBias=0.0f;
    limits->maxSamplerAnisotropy=1.0f;
}
#endif
