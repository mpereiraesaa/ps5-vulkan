#ifndef PS5VK_COLOR_ATTACHMENT_CONTRACT_H
#define PS5VK_COLOR_ATTACHMENT_CONTRACT_H
#include <vulkan/vulkan_core.h>

/* Bounded per-attachment colour contract of the DXVK v2.6.2 profile.
 *
 * The native path programmes one CB_COLOR0 target, one CB_BLEND0 equation and
 * one CB_TARGET_MASK, so this profile serves exactly one colour attachment.
 * That bound is what keeps `independentBlend` a blocker: the feature's whole
 * obligation is that each attachment carries its own blend state, which a
 * single attachment cannot exercise, and no native witness could show it. The
 * bound is a compile-time constant so the refusal is identical in every build
 * and can be pinned by tests; a slice that can render a second target - render
 * pass, framebuffer, pipeline key, CB_COLOR1/CB_BLEND1 and a second fragment
 * export - is the one that raises it.
 *
 * `maxFragmentDualSrcAttachments` and the dual-source contract are separate:
 * dual source adds a second *source* to attachment zero, not a second target. */
enum { PS5VK_MAX_COLOR_ATTACHMENTS = 1 };

/* AGC context-register offsets for the colour targets.
 *
 * The native path programmes one CB_COLORn block plus its blend control, and
 * the AGC offset of any context register is (raw - 0x28000) / 4 in the pinned
 * gfx103 register table. That rule reproduces every offset this driver already
 * writes: CB_COLOR0_BASE -> 0x318, CB_COLOR0_INFO -> 0x31c, CB_TARGET_MASK ->
 * 0x08e and CB_BLEND0_CONTROL -> 0x1e0. The second target's block is therefore
 * derivable rather than guessed, and tests/test_color_attachment_offsets.py
 * recomputes both lists from the pinned table, so a stale or mistyped entry
 * fails instead of programming the wrong register.
 *
 * The CB_COLOR1 block is NOT CB_COLOR0 shifted: BASE/VIEW/INFO/ATTRIB/DCC/CMASK
 * and their FMASK/CLEAR/DCC entries move by one dword each, while the EXT and
 * ATTRIB2/3 entries move by one dword from a different base. */
enum { PS5VK_COLOR_TARGET_REGISTERS = 16 };
extern const uint32_t ps5vk_color_attachment_offsets[2][PS5VK_COLOR_TARGET_REGISTERS];

/* CB_BLEND0_CONTROL is 0x1e0; target n's control is the next dword. */
#define PS5VK_AGC_CB_BLEND_CONTROL(n) (0x1e0u + (n))

/* SPI_SHADER_COL_FORMAT is one nibble per colour target, target i in bits
 * [4i, 4i+3], and the code the pinned compiler writes for one RGBA8 target is
 * Mesa ac_choose_spi_color_formats' pair: FP16_ABGR (4) when that target
 * blends, 32_ABGR (9) when it does not. Measured against the pinned PSBC with
 * a single-output module: option 0 -> 0x9, option 4 -> 0x4, option 9 -> 0x9;
 * and with a two-output module: option 0x99 -> 0x99 with CB_SHADER_MASK 0xff,
 * option 0x4 -> 0x4 with mask 0xf (the second export dropped). The option is
 * therefore a property of the subpass's colour attachments, not of the
 * pipeline as a whole, and the second target's code rides in the second
 * nibble. */
uint32_t ps5vk_color_export_format_code(int blending);
uint32_t ps5vk_color_export_format_option(const unsigned char *blend_enable,
                                          uint32_t count);

/* A pipeline that declares a count the profile cannot render, or an operation
 * the backend does not programme, is refused where the application can see it
 * rather than accepted and failed later. */
int ps5vk_color_attachment_count_supported(uint32_t count);
int ps5vk_color_blend_state_shape_supported(const VkPipelineColorBlendStateCreateInfo *state);

/* True when this attachment state consumes the fragment module's secondary
 * export. Such a state also needs dualSrcBlend enabled on the logical device
 * and the compiler-proven secondary export; the caller checks those. */
int ps5vk_color_attachment_uses_src1(const VkPipelineColorBlendAttachmentState *state);

#endif
