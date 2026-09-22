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
/* The attachments ONE SUBPASS renders into, as framebuffer attachment indices.
 *
 * A framebuffer's own role lists are subpass 0's roles, which is the whole
 * story for the single-subpass profile and nothing like it for the pinned
 * multisample oracle: there subpass 0 renders the multisampled colour and
 * resolves it, and every following subpass renders a single-sample target of
 * its own. Deriving the target set from the subpass a draw belongs to is what
 * makes those subpasses addressable at all; passing NULL keeps the framebuffer
 * roles, which is exactly the meaning every earlier caller had. */
struct ps5vk_target_set {
    uint32_t color[PS5VK_MAX_COLOR_ATTACHMENTS];
    uint32_t color_count;
    uint32_t depth;
};
/* Fill a target set from one subpass's own references. The pass and the
 * framebuffer must already be the pair the draw executes against; a subpass
 * whose references leave the framebuffer is refused rather than clamped. */
VkResult ps5vk_target_set_from_subpass(VkRenderPass, VkFramebuffer, uint32_t subpass,
    struct ps5vk_target_set *out);
/* Prepare only: translate the recorded Vulkan objects into owned, flushed
 * indirect register storage. Submission must pin source resources separately.
 * Release only before launch or after exact completion. */
VkResult ps5vk_native_prepare_draw(VkDevice, const struct ps5vk_operation *, const VkRect2D *,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT], struct ps5vk_prepared_draw *);
void ps5vk_native_release_draw(struct ps5vk_prepared_draw *);
VkResult ps5vk_native_prepare_vertex_draw(VkDevice,const struct ps5vk_operation *,const VkRect2D *,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],const struct ps5vk_graphics_key *,
    uint64_t shader_address,const struct ps5vk_target_set *,struct ps5vk_prepared_draw *);
/* Production bridge: usage_mask comes from the compiled vertex shader and
 * preserves Vulkan binding indices when compacting the indirect SRD table. */
VkResult ps5vk_native_prepare_vertex_draw_masked(VkDevice,const struct ps5vk_operation *,const VkRect2D *,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],const struct ps5vk_graphics_key *,
    uint64_t shader_address,uint32_t usage_mask,const struct ps5vk_target_set *,
    struct ps5vk_prepared_draw *);
/* Runtime shaders without vertex inputs can still use descriptor tables. */
VkResult ps5vk_native_prepare_resource_draw(VkDevice,const struct ps5vk_operation *,const VkRect2D *,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],uint64_t shader_address,
    const struct ps5vk_target_set *,struct ps5vk_prepared_draw *);
#endif
