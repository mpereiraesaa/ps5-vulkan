#ifndef PS5VK_DRAW_STATE_PS5_H
#define PS5VK_DRAW_STATE_PS5_H
#include "vk_pipeline.h"
#include "graphics_pipeline_ps5.h"
#include "targets_ps5.h"
#include "ps5_pipeline.h"
enum { PS5VK_DRAW_CX_CAPACITY = PS5_PIPELINE_CX_REGISTERS + PS5_DEPTH_REGISTER_COUNT + 8 };
struct ps5vk_draw_state {
    ps5_agc_register cx[PS5VK_DRAW_CX_CAPACITY], sh[12], uc[3];
    uint32_t cx_count;
    uint64_t modifier;
};
/* Caller must keep the resulting register block in published GPU-visible
 * storage through retirement. No init-context, clear, cache or draw emitted. */
VkResult ps5vk_native_draw_state(VkPipeline, const struct ps5vk_target_registers *color,
    const struct ps5vk_target_registers *depth, const VkRect2D *area,
    uint32_t width, uint32_t height, struct ps5vk_draw_state *);
#endif
