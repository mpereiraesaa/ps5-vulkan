#ifndef PS5VK_DRAW_STATE_PS5_H
#define PS5VK_DRAW_STATE_PS5_H
#include "vk_pipeline.h"
#include "graphics_pipeline_ps5.h"
#include "targets_ps5.h"
#include "ps5_pipeline.h"
#include "runtime_draw_abi.h"
/* The extra two slots are the primitive-restart enable and reset index this path
 * programs from the pipeline's flag and the draw's index width; the tessellation
 * pair adds its hull launch state, the tessellator configuration and the ring
 * set, so the context capacity grows with the hull's own registers. The shader
 * bank carries both hull programs' register blocks behind the runtime pair's. */
enum {
    PS5VK_DRAW_CX_CAPACITY = PS5_PIPELINE_CX_REGISTERS + 15 + PS5_DEPTH_REGISTER_COUNT + 8 + 16,
    PS5VK_DRAW_SH_CAPACITY = 32,
    PS5VK_DRAW_UC_CAPACITY = 12
};
struct ps5vk_draw_state {
    ps5_agc_register cx[PS5VK_DRAW_CX_CAPACITY], sh[PS5VK_DRAW_SH_CAPACITY],
        uc[PS5VK_DRAW_UC_CAPACITY];
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
    uint32_t width, uint32_t height, unsigned index_width,
    struct ps5vk_draw_state *);
#endif
