#ifndef PS5VK_DRAW_STATE_PS5_H
#define PS5VK_DRAW_STATE_PS5_H
#include "vk_pipeline.h"
#include "graphics_pipeline_ps5.h"
#include "targets_ps5.h"
#include "ps5_pipeline.h"
#include "runtime_draw_abi.h"
/* Registers the draw state writes after the pipeline's 84-register base: the
 * two runtime stage blocks (13), the optional depth target, the eight fixed
 * raster/clip/scissor words, and the six polygon-offset words
 * (PA_SU_POLY_OFFSET_DB_FMT_CNTL .. BACK_OFFSET, 0x2de..0x2e3). */
enum { PS5VK_DRAW_RASTER_REGISTERS = 8 + 6 };
enum { PS5VK_DRAW_CX_CAPACITY = PS5_PIPELINE_CX_REGISTERS + 13 + PS5_DEPTH_REGISTER_COUNT +
    PS5VK_DRAW_RASTER_REGISTERS };
struct ps5vk_draw_state {
    ps5_agc_register cx[PS5VK_DRAW_CX_CAPACITY], sh[16], uc[3];
    uint32_t cx_count;
    uint32_t sh_count; /* Zero preserves the legacy 12-register LLPC path. */
    uint64_t modifier;
    uint32_t push_constant_low;
    struct ps5vk_runtime_draw_abi runtime;
};
/* Caller must keep the resulting register block in published GPU-visible
 * storage through retirement. No init-context, clear, cache or draw emitted.
 * `raster` is the draw's resolved rasterization snapshot (vk_pipeline.h); the
 * polygon-offset registers are ALWAYS written from it, so a draw with the bias
 * disabled programs the disable bits and zero factors rather than inheriting
 * whatever the previous draw left in the context. */
VkResult ps5vk_native_draw_state(VkPipeline, const VkViewport *, const VkRect2D *,
    const struct ps5vk_raster_state *raster,
    const struct ps5vk_target_registers *color, const struct ps5vk_target_registers *depth,
    const VkRect2D *area,
    uint32_t width, uint32_t height, struct ps5vk_draw_state *);
#endif
