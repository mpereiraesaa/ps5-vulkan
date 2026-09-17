#ifndef PS5VK_DRAW_STATE_PS5_H
#define PS5VK_DRAW_STATE_PS5_H
#include "vk_pipeline.h"
#include "graphics_pipeline_ps5.h"
#include "targets_ps5.h"
#include "ps5_pipeline.h"
#include "runtime_draw_abi.h"
enum { PS5VK_DRAW_CX_CAPACITY = PS5_PIPELINE_CX_REGISTERS + 13 + PS5_DEPTH_REGISTER_COUNT + 8 };
struct ps5vk_draw_state {
    ps5_agc_register cx[PS5VK_DRAW_CX_CAPACITY], sh[16], uc[4];
    uint32_t cx_count;
    /* The UC registers this draw programs. The linked three are the whole UC
     * block the AGC link exposes; a merged vertex+geometry program additionally
     * needs the GE's PC-line allocation (R_030980), which neither the compiler
     * metadata nor the linked block carries, so it stays out until a source for
     * it exists. */
    uint32_t uc_count;
    uint32_t sh_count; /* Zero preserves the legacy 12-register LLPC path. */
    uint64_t modifier;
    uint32_t push_constant_low;
    struct ps5vk_runtime_draw_abi runtime;
};
/* Caller must keep the resulting register block in published GPU-visible
 * storage through retirement. No init-context, clear, cache or draw emitted. */
VkResult ps5vk_native_draw_state(VkPipeline, const VkViewport *, const VkRect2D *,
    const struct ps5vk_target_registers *color, const struct ps5vk_target_registers *depth,
    const VkRect2D *area,
    uint32_t width, uint32_t height, struct ps5vk_draw_state *);
#endif
