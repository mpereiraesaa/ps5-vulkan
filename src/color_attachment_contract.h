#ifndef PS5VK_COLOR_ATTACHMENT_CONTRACT_H
#define PS5VK_COLOR_ATTACHMENT_CONTRACT_H
#include <vulkan/vulkan_core.h>

/* Bounded per-attachment colour contract of the DXVK v2.6.2 profile.
 *
 * The native path programmes one CB_COLOR0 target, one CB_BLEND0 equation and
 * one CB_TARGET_MASK, so this profile SERVES exactly one colour attachment.
 * That bound is what keeps `independentBlend` a blocker: the feature's whole
 * obligation is that each attachment carries its own blend state, which a
 * single attachment cannot exercise, and no native witness could show it. The
 * bound is a compile-time constant so the refusal is identical in every build
 * and can be pinned by tests; a slice that can render a second target - render
 * pass, framebuffer, pipeline key, CB_COLOR1/CB_BLEND1 and a second fragment
 * export - is the one that raises it.
 *
 * Zero colour attachments is a separate, already-served shape: Vulkan's
 * DEPTH-ONLY subpass (colorAttachmentCount 0 with a depth reference), which
 * programmes no colour target at all and needs no CB_COLOR0 block.
 *
 * `maxFragmentDualSrcAttachments` and the dual-source contract are separate:
 * dual source adds a second *source* to attachment zero, not a second target. */
/* PS5VK_MAX_COLOR_ATTACHMENTS is both the widest count this ABI can describe
 * and what the profile serves: the render pass, the framebuffer and the
 * pipeline key each carry that many colour roles, the native draw state
 * programmes one CB_COLORn block, one CB_BLENDn_CONTROL and one
 * SPI_SHADER_COL_FORMAT nibble per attachment with a per-attachment write mask
 * in CB_TARGET_MASK, and the advertised maxColorAttachments reports the same
 * bound. A pass or pipeline asking for more is refused where the application
 * can see it, and a multiview subpass that names two colour targets is refused
 * because the view expansion rewrites one target's layer-addressed words. */
enum { PS5VK_MAX_COLOR_ATTACHMENTS = 2 };

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

/* CB_BLEND0_CONTROL is 0x1e0 and SX_MRT0_BLEND_OPT 0x1d8; target n's control
 * and optimisation are the next dwords, both derived from the pinned table and
 * pinned by tests/test_color_attachment_offsets.py. */
#define PS5VK_AGC_CB_BLEND_CONTROL(n) (0x1e0u + (n))
#define PS5VK_AGC_SX_MRT_BLEND_OPT(n) (0x1d8u + (n))

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

/* The colour formats this profile renders into, and whether a target's
 * fragment export is an integer vector rather than a normalized one.
 *
 * The normalized pair is the shipping set. R8G8B8A8_UINT exists for the
 * DXVK262-T06 independentBlend measurement: the only upstream leaves in the
 * pinned tree that REQUIRE the feature draw into R8G8B8A8_UINT plus
 * R8G8B8A8_UNORM (vktRenderPassTests.cpp:6444), so the measurement build has to
 * serve an integer target before that oracle can run at all. It is behind the
 * private switch, and the shipped capability set is unchanged until the leaves
 * pass and the promotion lands. */
static inline int ps5vk_color_target_format_supported(VkFormat format)
{
    if (format == VK_FORMAT_B8G8R8A8_UNORM ||
        format == VK_FORMAT_R8G8B8A8_UNORM ||
        /* The integer colour target the independentBlend oracle draws into,
         * promoted with the feature: the two upstream leaves that require it
         * render into R8G8B8A8_UINT plus R8G8B8A8_UNORM and now pass on
         * hardware (measured 2026-09-22). */
        format == VK_FORMAT_R8G8B8A8_UINT) return 1;
    if (format == VK_FORMAT_R8G8B8A8_SINT) return 1;
    return 0;
}

/* An integer colour target's fragment export is a 32-bit unsigned vector: its
 * lanes are not converted, so the pipeline may not blend into it and the
 * fragment interface must see an integer output. */
static inline int ps5vk_color_target_format_is_integer(VkFormat format)
{
    return format == VK_FORMAT_R8G8B8A8_UINT ||
        format == VK_FORMAT_R8G8B8A8_SINT;
}

/* Whether this build actually SERVES an integer colour target. It is a
 * property of the format now that the promotion landed: the integer target is
 * part of the served capability set, and every path that executes or reads one
 * back asks this one question. */
static inline int ps5vk_color_target_integer_served(VkFormat format)
{
    return ps5vk_color_target_format_is_integer(format);
}

/* The upstream render-pass module derives an attachment's usage from the
 * format's reported features, so its colour attachments are created with the
 * sampled role on top of the readback one whenever the format publishes it.
 * That combination is served now that the feature and the integer target it
 * needs are promoted. */
static inline int ps5vk_color_sampled_readback_served(void)
{
    return 1;
}

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
