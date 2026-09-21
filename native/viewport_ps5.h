#ifndef PS5VK_VIEWPORT_PS5_H
#define PS5VK_VIEWPORT_PS5_H
#include <vulkan/vulkan_core.h>
#include "ps5_agc.h"
/* One viewport bank: PA_CL_VPORT_{X,Y,Z}{SCALE,OFFSET} (6), PA_SC_VPORT_ZMIN/ZMAX
 * (2) and PA_SC_VPORT_SCISSOR_TL/BR (2). */
enum { PS5VK_VIEWPORT_REGISTERS = 10 };
/* The bank count the gfx10 context register file has: viewport i lives at
 * PA_CL_VPORT_XSCALE_0 + 6*i (0x10f..0x169), PA_SC_VPORT_ZMIN_0 + 2*i
 * (0x0b4..0x0d3) and PA_SC_VPORT_SCISSOR_0_TL + 2*i (0x094..0x0b3), for
 * i in 0..15 - the pinned Mesa gfx10 register schema and RADV's
 * radv_emit_viewport_state/radv_emit_scissor_state stride. */
enum { PS5VK_VIEWPORT_BANKS = 16 };
/* Compact context offsets, gfx1013. No register publication/submission here.
 * Scissor is intersected with render area; zero extent clips every fragment.
 * Bank zero: the viewport a draw without a ViewportIndex output uses. */
VkResult ps5vk_native_viewport(const VkViewport *, const VkRect2D *scissor,
    const VkRect2D *render_area, ps5_agc_register out[PS5VK_VIEWPORT_REGISTERS]);
/* Bank `index` (0..PS5VK_VIEWPORT_BANKS-1): the same ten registers at that
 * bank's offsets. Index zero is exactly ps5vk_native_viewport. */
VkResult ps5vk_native_viewport_bank(uint32_t index, const VkViewport *,
    const VkRect2D *scissor, const VkRect2D *render_area,
    ps5_agc_register out[PS5VK_VIEWPORT_REGISTERS]);
#endif
