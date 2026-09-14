#ifndef PS5VK_DRAW_PREPARE_PS5_H
#define PS5VK_DRAW_PREPARE_PS5_H
#include "draw_emit_ps5.h"
#include "graphics_program.h"
struct ps5vk_prepared_draw {
    struct ps5vk_memory_backend memory;
    struct ps5vk_draw_state *state;
    void *backing;
    size_t bytes;
    const uint32_t *vertex_table;
    const uint32_t *texture_table;
    const uint32_t *descriptor_tables[PS5VK_RUNTIME_DESCRIPTOR_SETS];
    uint32_t descriptor_bytes[PS5VK_RUNTIME_DESCRIPTOR_SETS];
    const void *vertex_bounce;
    size_t vertex_bounce_bytes;
};
/* Prepare only: translate the recorded Vulkan objects into owned, flushed
 * indirect register storage. Submission must pin source resources separately.
 * Release only before launch or after exact completion. */
VkResult ps5vk_native_prepare_draw(VkDevice, const struct ps5vk_operation *, const VkRect2D *,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT], struct ps5vk_prepared_draw *);
void ps5vk_native_release_draw(struct ps5vk_prepared_draw *);
VkResult ps5vk_native_prepare_vertex_draw(VkDevice,const struct ps5vk_operation *,const VkRect2D *,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],const struct ps5vk_graphics_key *,
    uint64_t shader_address,struct ps5vk_prepared_draw *);
/* Production bridge: usage_mask comes from the compiled vertex shader and
 * preserves Vulkan binding indices when compacting the indirect SRD table. */
VkResult ps5vk_native_prepare_vertex_draw_masked(VkDevice,const struct ps5vk_operation *,const VkRect2D *,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],const struct ps5vk_graphics_key *,
    uint64_t shader_address,uint32_t usage_mask,struct ps5vk_prepared_draw *);
/* Runtime shaders without vertex inputs can still use descriptor tables. */
VkResult ps5vk_native_prepare_resource_draw(VkDevice,const struct ps5vk_operation *,const VkRect2D *,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],uint64_t shader_address,
    struct ps5vk_prepared_draw *);
#endif
