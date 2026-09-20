#ifndef PS5VK_NATIVE_TESS_PROFILE_H
#define PS5VK_NATIVE_TESS_PROFILE_H

#include "vk_internal.h"

/* Only the runtime graphics queue implements the validated linked LS/HS,
 * TES and TES/GS path. Compute-only and offline-library builds do not. */
static inline void ps5vk_native_tess_profile(struct ps5vk_platform *platform)
{
#if defined(PS5VK_GRAPHICS_API) && PS5VK_GRAPHICS_API && \
    defined(PS5VK_GRAPHICS_DRAW) && PS5VK_GRAPHICS_DRAW && \
    defined(PS5VK_RUNTIME_COMPILER) && PS5VK_RUNTIME_COMPILER && \
    defined(PS5VK_RUNTIME_GRAPHICS) && PS5VK_RUNTIME_GRAPHICS
    platform->supported_features |= PS5VK_FEATURE_TESSELLATION_SHADER;
    VkPhysicalDeviceLimits *limits = &platform->properties.limits;
    limits->maxTessellationGenerationLevel = 64;
    limits->maxTessellationPatchSize = 32;
    limits->maxTessellationControlPerVertexInputComponents = 128;
    limits->maxTessellationControlPerVertexOutputComponents = 128;
    limits->maxTessellationControlPerPatchOutputComponents = 120;
    limits->maxTessellationControlTotalOutputComponents = 4096;
    limits->maxTessellationEvaluationInputComponents = 128;
    limits->maxTessellationEvaluationOutputComponents = 128;
#else
    (void)platform;
#endif
}
#endif
