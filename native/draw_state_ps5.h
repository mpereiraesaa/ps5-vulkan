#ifndef PS5VK_DRAW_STATE_PS5_H
#define PS5VK_DRAW_STATE_PS5_H
#include "vk_pipeline.h"
#include "graphics_pipeline_ps5.h"
#include "targets_ps5.h"
#include "ps5_pipeline.h"
#include "runtime_draw_abi.h"
#include "viewport_ps5.h"
/* Registers the draw state writes after the pipeline's 84-register base: the
 * two runtime stage blocks and the two primitive-restart slots (15), the
 * optional depth target, the eight fixed raster/clip/scissor words, the six
 * polygon-offset words (PA_SU_POLY_OFFSET_DB_FMT_CNTL .. BACK_OFFSET,
 * 0x2de..0x2e3), the four point/line words (PA_SU_POINT_SIZE,
 * PA_SU_POINT_MINMAX, PA_SU_LINE_CNTL, PA_SC_LINE_CNTL) the non-solid polygon
 * modes rasterize with, and one ten-register bank for every viewport index
 * above zero (bank zero replaces the base's own viewport words in place).
 * The tessellation pair additionally contributes its hull launch state,
 * tessellator configuration and ring set; both hull register blocks share the
 * expanded shader bank. */
enum { PS5VK_DRAW_RASTER_REGISTERS = 8 + 6 + 4 +
    (PS5VK_MAX_VIEWPORTS - 1) * PS5VK_VIEWPORT_REGISTERS };
enum {
    PS5VK_DRAW_CX_CAPACITY = PS5_PIPELINE_CX_REGISTERS + 15 +
        PS5_DEPTH_REGISTER_COUNT + PS5VK_DRAW_RASTER_REGISTERS + 24 + 9 +
        3 /* stencil control and the two reference/mask words */,
    PS5VK_DRAW_SH_CAPACITY = 32,
    PS5VK_DRAW_UC_CAPACITY = 12
};
_Static_assert((int)PS5VK_MAX_VIEWPORTS <= (int)PS5VK_VIEWPORT_BANKS,
    "the pipeline's viewport arrays cannot exceed the hardware bank count");
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
    struct ps5vk_runtime_draw_abi hull_runtime;
};
/* Caller must keep the resulting register block in published GPU-visible
 * storage through retirement. No init-context, clear, cache or draw emitted.
 * `viewports`/`scissors` are the draw's resolved arrays, `viewport_count` of
 * each (1..PS5VK_MAX_VIEWPORTS): index zero is programmed into the base's own
 * viewport words, every further index into its own hardware bank, so a
 * ViewportIndex the pre-raster stage exports selects among exactly the
 * viewports this draw was recorded with. `raster` is the draw's resolved
 * rasterization snapshot (vk_pipeline.h); the polygon-offset registers are
 * ALWAYS written from it, so a draw with the bias disabled programs the disable
 * bits and zero factors rather than inheriting whatever the previous draw left
 * in the context. */
VkResult ps5vk_native_draw_state(VkPipeline, const VkViewport *viewports,
    const VkRect2D *scissors, uint32_t viewport_count,
    const struct ps5vk_raster_state *raster,
    const struct ps5vk_target_registers *colors, uint32_t color_count,
    const struct ps5vk_target_registers *depth,
    const VkRect2D *area,
    uint32_t width, uint32_t height, unsigned index_width,
    struct ps5vk_draw_state *);
/* The three stencil words of a draw with the stencil test enabled:
 * DB_STENCIL_CONTROL, DB_STENCILREFMASK and DB_STENCILREFMASK_BF. */
int ps5vk_stencil_registers(const VkStencilOpState *front, const VkStencilOpState *back,
    ps5_agc_register out[3]);
#endif
