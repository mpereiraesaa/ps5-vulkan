#ifndef PS5VK_COMPILER_H
#define PS5VK_COMPILER_H

#include "vk_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compile a SPIR-V compute shader into a ps5vk_compiled_program using PSBC/ACO.
 *
 * spirv: pointer to SPIR-V words
 * spirv_words: number of 32-bit words
 * entry_name: compute entry point (e.g. "main")
 * layout: pipeline layout with descriptor set signatures
 * out_program: populated compiled program
 * out_code: receives pointer to newly allocated machine code buffer (caller owns and frees)
 */
VkResult ps5vk_runtime_compile_compute(
    const uint32_t *spirv,
    size_t spirv_words,
    const char *entry_name,
    VkPipelineLayout layout,
    const VkSpecializationInfo *specialization,
    struct ps5vk_compiled_program *out_program,
    uint32_t **out_code
);

VkResult ps5vk_runtime_compile_compute_features(
    const uint32_t *spirv,
    size_t spirv_words,
    const char *entry_name,
    VkPipelineLayout layout,
    const VkSpecializationInfo *specialization,
    uint32_t feature_mask,
    struct ps5vk_compiled_program *out_program,
    uint32_t **out_code
);

/* Adapter matching struct ps5vk_compiler.compile signature */
VkResult ps5vk_compiler_adapter_compile(
    void *context,
    const uint32_t *spirv,
    size_t spirv_words,
    const char *entry_name,
    VkPipelineLayout layout,
    const VkSpecializationInfo *specialization,
    uint32_t feature_mask,
    struct ps5vk_compiled_program *out_program,
    uint32_t **out_code
);

#ifdef __cplusplus
}
#endif

#endif /* PS5VK_COMPILER_H */
