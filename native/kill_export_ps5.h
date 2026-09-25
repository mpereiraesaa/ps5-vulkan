#ifndef PS5VK_KILL_EXPORT_PS5_H
#define PS5VK_KILL_EXPORT_PS5_H
#include <stdint.h>
#include "ps5_agc.h"

/* Export memory for a pixel program that can remove its own pixels.
 *
 * The pinned RADV (radv_pipeline_graphics.c, radv_needs_null_export_workaround)
 * records a hardware rule: "The hardware ignores the EXEC mask if no export
 * memory is allocated, so KILL and alpha test do not work correctly without
 * this." A pixel program that can discard (OpKill, OpTerminateInvocation or
 * OpDemoteToHelperInvocation all set DB_SHADER_CONTROL.KILL_ENABLE) and that
 * exports no colour, depth, stencil or sample mask therefore needs
 * SPI_SHADER_COL_FORMAT = SPI_SHADER_32_R for MRT0, so its NULL export carries
 * the valid mask the depth block reads. CB_SHADER_MASK stays zero: the export
 * memory is not a colour write, and no colour target receives anything.
 *
 * The standalone compiler publishes the pixel stage's own format (zero for a
 * depth-only discard program) and has no pipeline pass that adds the
 * allocation. Measured on hardware with the T11 pixel-removal witness (OpKill,
 * OpTerminateInvocation and OpDemoteToHelperInvocation on D32_SFLOAT_S8_UINT):
 * without the allocation every removed pixel still wrote depth and stencil,
 * with it none did. The runtime draw applies the rule to every pixel package.
 *
 * Context-register indices ((address - 0x28000) / 4, GFX10.3 register data):
 * SPI_SHADER_Z_FORMAT 0x28710, SPI_SHADER_COL_FORMAT 0x28714,
 * DB_SHADER_CONTROL 0x2880c with KILL_ENABLE at bit 6. */
enum {
    PS5VK_CX_SPI_SHADER_Z_FORMAT = 0x1c4u,
    PS5VK_CX_SPI_SHADER_COL_FORMAT = 0x1c5u,
    PS5VK_CX_DB_SHADER_CONTROL = 0x203u,
    PS5VK_DB_SHADER_CONTROL_KILL_ENABLE = 1u << 6,
    PS5VK_SPI_SHADER_32_R = 1u,
};

/* The index of the LAST write of `offset` in an ordered bank, or -1. A stream
 * that names a register twice leaves the last write in force, so that is the
 * value the hardware sees. */
static inline int ps5vk_kill_export_last(const ps5_agc_register *bank,
                                         unsigned count, uint32_t offset)
{
    for (unsigned i = count; i > 0; --i)
        if (bank[i - 1].offset == offset) return (int)(i - 1);
    return -1;
}

/* 1 when the bank describes a pixel program that can kill while allocating no
 * export memory, 0 when it does not, and -1 when one of the three registers
 * the decision needs is absent: the rule is never applied to a guess. */
static inline int ps5vk_kill_needs_export_memory(const ps5_agc_register *bank,
                                                 unsigned count)
{
    if (!bank) return -1;
    const int control = ps5vk_kill_export_last(bank, count, PS5VK_CX_DB_SHADER_CONTROL);
    const int depth = ps5vk_kill_export_last(bank, count, PS5VK_CX_SPI_SHADER_Z_FORMAT);
    const int color = ps5vk_kill_export_last(bank, count, PS5VK_CX_SPI_SHADER_COL_FORMAT);
    if (control < 0 || depth < 0 || color < 0) return -1;
    return (bank[control].value & PS5VK_DB_SHADER_CONTROL_KILL_ENABLE) &&
           !bank[depth].value && !bank[color].value;
}

/* Give such a program its MRT0 export memory by rewriting the colour format
 * the hardware receives. Returns 1 when the bank changed, 0 when the program
 * already allocates export memory or cannot kill, and -1 as above. */
static inline int ps5vk_kill_export_memory_apply(ps5_agc_register *bank,
                                                 unsigned count)
{
    const int needed = ps5vk_kill_needs_export_memory(bank, count);
    if (needed <= 0) return needed;
    bank[ps5vk_kill_export_last(bank, count, PS5VK_CX_SPI_SHADER_COL_FORMAT)].value =
        PS5VK_SPI_SHADER_32_R;
    return 1;
}

#endif
