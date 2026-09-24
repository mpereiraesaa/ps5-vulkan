/*
 * Copyright (C) 2026 BlackBearReloaded
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GFX10.3 image-format words and component selectors adapted from
 * BlackBearReloaded's ps5-opengl, src/gallium/ps5/ps5_screen.c at commit
 * 7f9bfabdddb187a11e4401058eba8c9e55194d0a (GPL-3.0-or-later).
 *
 * This is the only place in ps5-vulkan that decides which format capability
 * exists. See texture_format.h for the implemented/witnessed contract and
 * conformance_inventory/physical_format_validation.json for the diagnostics
 * that qualify a capability for publication.
 */
#include "texture_format.h"
#include "color_attachment_contract.h"

/* Column aliases for the table below. */
#define GPL PS5VK_FORMAT_PROVENANCE_GPL_REFERENCE
#define PACK PS5VK_FORMAT_PROVENANCE_REGISTRY_PACKING
#define CAP_DST PS5VK_FORMAT_CAP_TRANSFER_DST
#define CAP_SRC PS5VK_FORMAT_CAP_TRANSFER_SRC
#define CAP_SAMP PS5VK_FORMAT_CAP_SAMPLED_IMAGE
#define CAP_LINEAR PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR
#define CAP_BLIT_SRC PS5VK_FORMAT_CAP_BLIT_SRC
#define CAP_COLOR PS5VK_FORMAT_CAP_COLOR_ATTACHMENT
#define CAP_COLOR_READBACK PS5VK_FORMAT_CAP_COLOR_ATTACHMENT_READBACK
#define CAP_BLEND PS5VK_FORMAT_CAP_COLOR_ATTACHMENT_BLEND
#define CAP_DEPTH PS5VK_FORMAT_CAP_DEPTH_STENCIL_ATTACHMENT
#define CAP_VERTEX PS5VK_FORMAT_CAP_VERTEX_BUFFER
#define CAP_UTEXEL PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER
#define CAP_STORAGE_IMAGE PS5VK_FORMAT_CAP_STORAGE_IMAGE

/* Sampled row: the GFX1013 word/selectors/texel size are the pinned GPL
 * encoding. ENABLED carries additional directly qualified roles. */
/* The DXVK262-T06 independentBlend target: the only upstream leaves that
 * require the feature draw into R8G8B8A8_UINT plus R8G8B8A8_UNORM, so the
 * integer colour target is part of the served capability set - it renders, it
 * is blended-free, it clears with its raw word and it is read back. Promoted
 * with the feature (measured 2026-09-22). */
#define CAP_INTEGER_TARGET (CAP_COLOR | CAP_COLOR_READBACK | CAP_SRC)
#define SAMPLED(f, bpt, word, s0, s1, s2, s3, EXTRA, ENABLED) \
    { (f), (bpt), (word), {(s0), (s1), (s2), (s3)}, \
      CAP_SAMP | CAP_DST | (EXTRA) | (ENABLED), \
      CAP_SAMP | CAP_DST | (EXTRA) | (ENABLED), GPL, 1, 1, (bpt) }
/* Sampled row whose byte layout is the pinned registry's packed-component
 * order rather than one of the reference's explicit rows. WITNESSED lists
 * enabled capabilities, including the independently qualified sampled roles;
 * ENABLED lists additional directly qualified operations. */
#define SAMPLED_PACKED(f, bpt, word, s0, s1, s2, s3, WITNESSED, ENABLED) \
    { (f), (bpt), (word), {(s0), (s1), (s2), (s3)}, \
      CAP_SAMP | CAP_DST | (WITNESSED) | (ENABLED), \
      (WITNESSED) | (ENABLED), GPL | PACK, 1, 1, (bpt) }
/* Row with no sampled-image encoding: a render-target, depth or buffer role. */
#define BUFFER(f, CAPS) \
    { (f), 0, 0, {0, 0, 0, 0}, (CAPS), (CAPS), PS5VK_FORMAT_PROVENANCE_NONE, 0, 0, 0 }
/* Complete components absent from the Vulkan format in the descriptor.
 * In particular BC1 RGB must not expose BC1's optional transparent alpha. */
#define BC(f, gfxfmt, bytes, components) \
    { (f), 0, ((uint32_t)(gfxfmt) << 20), \
      {4, (components) >= 2 ? 5 : 0, (components) >= 3 ? 6 : 0, \
       (components) == 4 ? 7 : 1}, \
      CAP_SAMP | CAP_LINEAR | CAP_SRC | CAP_DST | CAP_BLIT_SRC, 0, \
      PS5VK_FORMAT_PROVENANCE_GFX10_FORMAT_ENUM, 4, 4, (bytes) }

static const struct ps5vk_texture_format formats[] = {
    /* --- 8-bit and packed sampled formats ---------------------------------
     * Every witnessed row has exact PS5 creation, upload, sampling and
     * readback evidence; the filterable rows additionally have two
     * byte-identical nearest/linear checkerboard runs. Integer rows use typed
     * isampler/usampler interfaces only, as Vulkan requires. The one-byte and
     * two-byte one- and two-component rows also carry the uniform-texel-buffer
     * role. A direct typed compute matrix validated their GFX10 words,
     * completion selectors and 1/2-byte element strides on console. */
    /* The four one-component one-byte rows also carry the uniform-texel-buffer
     * role: the descriptor path derives the GFX10
     * combined word (1/2/5/6 = 8_UNORM/8_SNORM/8_UINT/8_SINT) and the
     * (4,0,0,1) completion from the row and takes the element stride from
     * bytes_per_texel. The direct typed matrix covers this sub-4-byte path. */
    SAMPLED(VK_FORMAT_R8_UNORM, 1, UINT32_C(0x00100000), 4, 0, 0, 1,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R8_SNORM, 1, UINT32_C(0x00200000), 4, 0, 0, 1,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R8G8_UNORM, 2, UINT32_C(0x00e00000), 4, 5, 0, 1,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R8G8_SNORM, 2, UINT32_C(0x00f00000), 4, 5, 0, 1,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    /* The only colour attachment this profile both renders into and reads
     * back; the readback pair is a separate capability from the bare
     * attachment usage. */
    SAMPLED(VK_FORMAT_R8G8B8A8_UNORM, 4, UINT32_C(0x03800000), 4, 5, 6, 7,
            CAP_LINEAR | CAP_VERTEX | CAP_SRC | CAP_COLOR | CAP_COLOR_READBACK | CAP_UTEXEL,
            CAP_BLEND),
    SAMPLED(VK_FORMAT_R8G8B8A8_SNORM, 4, UINT32_C(0x03900000), 4, 5, 6, 7,
            CAP_LINEAR | CAP_VERTEX | CAP_UTEXEL, 0),
    SAMPLED(VK_FORMAT_R8G8B8A8_SRGB, 4, UINT32_C(0x08200000), 4, 5, 6, 7,
            CAP_LINEAR, 0),
    SAMPLED(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4, UINT32_C(0x08400000), 4, 5, 6, 1,
            CAP_LINEAR, 0),
    SAMPLED(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, UINT32_C(0x02400000), 4, 5, 6, 1,
            CAP_LINEAR, CAP_UTEXEL),
    /* --- 16-bit sampled formats ------------------------------------------
     * The five single-component rows also carry the uniform-texel-buffer role.
     * Their GFX10 combined
     * words are 7/8/13/11/12 = 16_UNORM/SNORM/FLOAT/UINT/SINT, the completion
     * is (4,0,0,1) -> 0x204 and the element is two bytes, so the descriptor
     * path needs nothing new. The mandatory 16-bit table requires the role for
     * SFLOAT, UINT and SINT; UNORM and SNORM are staged with them for family
     * coherence. Direct typed console fetches cover all five and the wider
     * R16 vector families. */
    SAMPLED(VK_FORMAT_R16_UNORM, 2, UINT32_C(0x00700000), 4, 0, 0, 1,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R16_SNORM, 2, UINT32_C(0x00800000), 4, 0, 0, 1,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R16_SFLOAT, 2, UINT32_C(0x00d00000), 4, 0, 0, 1,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R16G16_UNORM, 4, UINT32_C(0x01700000), 4, 5, 0, 1,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R16G16_SNORM, 4, UINT32_C(0x01800000), 4, 5, 0, 1,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R16G16_SFLOAT, 4, UINT32_C(0x01d00000), 4, 5, 0, 1,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R16G16B16A16_UNORM, 8, UINT32_C(0x04100000), 4, 5, 6, 7,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R16G16B16A16_SNORM, 8, UINT32_C(0x04200000), 4, 5, 6, 7,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R16G16B16A16_SFLOAT, 8, UINT32_C(0x04700000), 4, 5, 6, 7,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    /* --- 32-bit sampled formats ------------------------------------------ */
    SAMPLED(VK_FORMAT_R32_SFLOAT, 4, UINT32_C(0x01600000), 4, 0, 0, 1,
            CAP_LINEAR | CAP_VERTEX | CAP_UTEXEL, 0),
    SAMPLED(VK_FORMAT_R32G32_SFLOAT, 8, UINT32_C(0x04000000), 4, 5, 0, 1,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R32G32B32A32_SFLOAT, 16, UINT32_C(0x04d00000), 4, 5, 6, 7,
            CAP_LINEAR | CAP_VERTEX, CAP_UTEXEL),
    /* --- typed integer sampled formats (nearest only) --------------------- */
    SAMPLED(VK_FORMAT_R8_UINT, 1, UINT32_C(0x00500000), 4, 0, 0, 1,
            CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R8_SINT, 1, UINT32_C(0x00600000), 4, 0, 0, 1,
            CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R8G8_UINT, 2, UINT32_C(0x01200000), 4, 5, 0, 1,
            CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R8G8_SINT, 2, UINT32_C(0x01300000), 4, 5, 0, 1,
            CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R8G8B8A8_UINT, 4, UINT32_C(0x03c00000), 4, 5, 6, 7,
            CAP_VERTEX | CAP_UTEXEL | CAP_INTEGER_TARGET, CAP_INTEGER_TARGET),
    SAMPLED(VK_FORMAT_R8G8B8A8_SINT, 4, UINT32_C(0x03d00000), 4, 5, 6, 7,
            CAP_VERTEX | CAP_UTEXEL, 0),
    SAMPLED(VK_FORMAT_R16_UINT, 2, UINT32_C(0x00b00000), 4, 0, 0, 1,
            CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R16_SINT, 2, UINT32_C(0x00c00000), 4, 0, 0, 1,
            CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R16G16_UINT, 4, UINT32_C(0x01b00000), 4, 5, 0, 1,
            CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R16G16_SINT, 4, UINT32_C(0x01c00000), 4, 5, 0, 1,
            CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R16G16B16A16_UINT, 8, UINT32_C(0x04500000), 4, 5, 6, 7,
            CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R16G16B16A16_SINT, 8, UINT32_C(0x04600000), 4, 5, 6, 7,
            CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R32_UINT, 4, UINT32_C(0x01400000), 4, 0, 0, 1,
            CAP_VERTEX | CAP_UTEXEL, CAP_SRC | CAP_STORAGE_IMAGE),
    SAMPLED(VK_FORMAT_R32_SINT, 4, UINT32_C(0x01500000), 4, 0, 0, 1,
            CAP_VERTEX | CAP_UTEXEL, 0),
    SAMPLED(VK_FORMAT_R32G32_UINT, 8, UINT32_C(0x03e00000), 4, 5, 0, 1,
            CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R32G32_SINT, 8, UINT32_C(0x03f00000), 4, 5, 0, 1,
            CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R32G32B32A32_UINT, 16, UINT32_C(0x04b00000), 4, 5, 6, 7,
            CAP_VERTEX, CAP_UTEXEL),
    SAMPLED(VK_FORMAT_R32G32B32A32_SINT, 16, UINT32_C(0x04c00000), 4, 5, 6, 7,
            CAP_VERTEX, CAP_UTEXEL),
    /* --- packed sampled formats qualified on 2026-09-14 --------------------
     * `A8B8G8R8_*_PACK32` names the packed form whose R component occupies
     * bits 0-7 (the pinned registry's packed-component order), so its memory
     * layout is byte-identical to the R8G8B8A8_* row above: same GFX1013 word
     * and same identity selectors. Separate typed/normalized/sRGB sampling
     * and checkerboard filter diagnostics each passed twice with identical
     * SELF images. This does not promote attachment, storage or blit roles.
     * Exact artifacts and log digests are recorded in VALIDATION.md.
     * The four non-sRGB rows also carry the uniform-texel-buffer role in the
     * enabled role: their memory layout, GFX1013 word and identity selectors
     * are the R8G8B8A8 ones. Direct typed console fetches cover all four. */
    SAMPLED_PACKED(VK_FORMAT_A8B8G8R8_UNORM_PACK32, 4, UINT32_C(0x03800000), 4, 5, 6, 7,
                   CAP_VERTEX | CAP_SAMP | CAP_DST | CAP_LINEAR, CAP_UTEXEL),
    SAMPLED_PACKED(VK_FORMAT_A8B8G8R8_SNORM_PACK32, 4, UINT32_C(0x03900000), 4, 5, 6, 7,
                   CAP_VERTEX | CAP_SAMP | CAP_DST | CAP_LINEAR, CAP_UTEXEL),
    SAMPLED_PACKED(VK_FORMAT_A8B8G8R8_SRGB_PACK32, 4, UINT32_C(0x08200000), 4, 5, 6, 7,
                   CAP_SAMP | CAP_DST | CAP_LINEAR, 0),
    SAMPLED_PACKED(VK_FORMAT_A8B8G8R8_UINT_PACK32, 4, UINT32_C(0x03c00000), 4, 5, 6, 7,
                   CAP_VERTEX | CAP_SAMP | CAP_DST, CAP_UTEXEL),
    SAMPLED_PACKED(VK_FORMAT_A8B8G8R8_SINT_PACK32, 4, UINT32_C(0x03d00000), 4, 5, 6, 7,
                   CAP_VERTEX | CAP_SAMP | CAP_DST, CAP_UTEXEL),
    /* --- roles without a sampled-image encoding --------------------------- */
    /* GFX10's BC image-format field is nine bits wide. The descriptor and
     * padded block layout are represented for all 16 Vulkan variants;
     * witnessed stays zero until upload and the CTS-mandated transfer and
     * blit roles are integrated through the queue executor. */
    BC(VK_FORMAT_BC1_RGB_UNORM_BLOCK, 169, 8, 3),
    BC(VK_FORMAT_BC1_RGB_SRGB_BLOCK, 170, 8, 3),
    BC(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 169, 8, 4),
    BC(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 170, 8, 4),
    BC(VK_FORMAT_BC2_UNORM_BLOCK, 171, 16, 4),
    BC(VK_FORMAT_BC2_SRGB_BLOCK, 172, 16, 4),
    BC(VK_FORMAT_BC3_UNORM_BLOCK, 173, 16, 4),
    BC(VK_FORMAT_BC3_SRGB_BLOCK, 174, 16, 4),
    BC(VK_FORMAT_BC4_UNORM_BLOCK, 175, 8, 1),
    BC(VK_FORMAT_BC4_SNORM_BLOCK, 176, 8, 1),
    BC(VK_FORMAT_BC5_UNORM_BLOCK, 177, 16, 2),
    BC(VK_FORMAT_BC5_SNORM_BLOCK, 178, 16, 2),
    BC(VK_FORMAT_BC6H_UFLOAT_BLOCK, 179, 16, 3),
    BC(VK_FORMAT_BC6H_SFLOAT_BLOCK, 180, 16, 3),
    BC(VK_FORMAT_BC7_UNORM_BLOCK, 181, 16, 4),
    BC(VK_FORMAT_BC7_SRGB_BLOCK, 182, 16, 4),
    /* VideoOut target and vertex input; deliberately not sampled. */
    BUFFER(VK_FORMAT_B8G8R8A8_UNORM, CAP_COLOR | CAP_VERTEX),
    /* 64KB_Z_X depth target. TRANSFER_DST is the whole-subresource clear:
     * vkCmdClearDepthStencilImage writes one uniform 32-bit word over the
     * entire surface, which is tiling-invariant. TRANSFER_SRC is the whole
     * surface readback, which does need pixel addressing and now has it -
     * src/depth_detile.c carries the SW_64K_Z_X equation. No sampled role and
     * no blit role is claimed: neither has an implemented path. */
    BUFFER(VK_FORMAT_D32_SFLOAT, CAP_DEPTH | CAP_DST | CAP_SRC),
    /* Three-component rows are vertex-only: GFX1013 has no 96-bit image
     * data format, so no sampled encoding is claimed for them. */
    BUFFER(VK_FORMAT_R32G32B32_SFLOAT, CAP_VERTEX),
    BUFFER(VK_FORMAT_R32G32B32_SINT, CAP_VERTEX),
    BUFFER(VK_FORMAT_R32G32B32_UINT, CAP_VERTEX),
    /* Packed 10-bit vertex input only. */
    BUFFER(VK_FORMAT_A2B10G10R10_UNORM_PACK32, CAP_VERTEX),
};

const struct ps5vk_texture_format *ps5vk_texture_format_lookup(VkFormat format)
{
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
        if (formats[i].format == format) return &formats[i];
    return 0;
}

unsigned ps5vk_texture_format_count(void)
{
    return (unsigned)(sizeof(formats) / sizeof(formats[0]));
}

const struct ps5vk_texture_format *ps5vk_texture_format_at(unsigned index)
{
    return index < ps5vk_texture_format_count() ? &formats[index] : 0;
}

uint32_t ps5vk_texture_format_dst_sel(const struct ps5vk_texture_format *format)
{
    if (!format) return 0;
    return (uint32_t)format->selectors[0] |
        ((uint32_t)format->selectors[1] << 3) |
        ((uint32_t)format->selectors[2] << 6) |
        ((uint32_t)format->selectors[3] << 9);
}

uint32_t ps5vk_texture_format_gfx10_format(const struct ps5vk_texture_format *format)
{
    return format ? ((format->descriptor_format_word >> 20) & 0x1ffu) : 0;
}

uint32_t ps5vk_texture_format_capabilities(VkFormat format)
{
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    return entry ? entry->capabilities : 0;
}

VkBool32 ps5vk_texture_format_has(VkFormat format, uint32_t capability)
{
    return (ps5vk_texture_format_capabilities(format) & capability) == capability &&
        capability != 0;
}

static uint32_t format_witnessed_capabilities(const struct ps5vk_texture_format *entry)
{
    if (!entry) return 0;
    uint32_t witnessed = entry->witnessed;
#if defined(PS5VK_TEXTURE_COMPRESSION_BC_DIAGNOSTIC) && \
    PS5VK_TEXTURE_COMPRESSION_BC_DIAGNOSTIC
    /* This build-only switch serves the original CTS and the public SDK
     * witness. The shipping build reports only rows whose evidence was
     * promoted into the table. */
    if (!entry->bytes_per_texel && entry->block_width == 4 &&
        entry->block_height == 4 && entry->bytes_per_block &&
        (entry->capabilities & PS5VK_FORMAT_CAP_SAMPLED_IMAGE))
        witnessed |= entry->capabilities &
            (PS5VK_FORMAT_CAP_SAMPLED_IMAGE |
             PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR |
             PS5VK_FORMAT_CAP_TRANSFER_SRC |
             PS5VK_FORMAT_CAP_TRANSFER_DST |
             PS5VK_FORMAT_CAP_BLIT_SRC);
#endif
    return witnessed;
}

VkBool32 ps5vk_texture_format_witnessed(VkFormat format, uint32_t capability)
{
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    const uint32_t witnessed = format_witnessed_capabilities(entry);
    return entry && capability && (witnessed & capability) == capability;
}

VkBool32 ps5vk_texture_format_sampled_image(VkFormat format)
{
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    return entry && ps5vk_texture_format_witnessed(format,
        PS5VK_FORMAT_CAP_SAMPLED_IMAGE) &&
        entry->descriptor_format_word != 0;
}

VkBool32 ps5vk_texture_format_sampled_encoding(VkFormat format)
{
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    return entry && (entry->capabilities & PS5VK_FORMAT_CAP_SAMPLED_IMAGE) &&
        entry->descriptor_format_word != 0 && entry->bytes_per_block != 0 &&
        entry->block_width != 0 && entry->block_height != 0;
}

VkBool32 ps5vk_texture_format_block_compressed(VkFormat format)
{
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    return entry && !entry->bytes_per_texel && entry->block_width > 1 &&
        entry->block_height != 0 &&
        entry->bytes_per_block != 0;
}

void ps5vk_texture_format_properties(VkFormat format, VkFormatProperties *out)
{
    if (!out) return;
    VkFormatProperties properties = {0};
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    if (entry) {
        const uint32_t w = format_witnessed_capabilities(entry);
        if (w & PS5VK_FORMAT_CAP_SAMPLED_IMAGE)
            properties.optimalTilingFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (w & PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR)
            properties.optimalTilingFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if (w & PS5VK_FORMAT_CAP_TRANSFER_SRC)
            properties.optimalTilingFeatures |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if (w & PS5VK_FORMAT_CAP_TRANSFER_DST)
            properties.optimalTilingFeatures |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if (w & PS5VK_FORMAT_CAP_COLOR_ATTACHMENT)
            properties.optimalTilingFeatures |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if (w & PS5VK_FORMAT_CAP_COLOR_ATTACHMENT_BLEND)
            properties.optimalTilingFeatures |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        /* DXVK262-T06: the upstream blend factory gates every leaf on this bit
         * (isSupportedBlendFormat), and the dual-source family it serves draws
         * into VK_FORMAT_R8G8B8A8_UNORM - all 98 applicable leaves passed once
         * it was reported. Only that format widens: it is the one the witness
         * and the leaves measured, and the one whose channel order the partial
         * write masks name directly. */
        if ((w & PS5VK_FORMAT_CAP_COLOR_ATTACHMENT) &&
            format == VK_FORMAT_R8G8B8A8_UNORM)
            properties.optimalTilingFeatures |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        if (w & PS5VK_FORMAT_CAP_DEPTH_STENCIL_ATTACHMENT)
            properties.optimalTilingFeatures |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (w & PS5VK_FORMAT_CAP_STORAGE_IMAGE)
            properties.optimalTilingFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if (w & PS5VK_FORMAT_CAP_STORAGE_IMAGE_ATOMIC)
            properties.optimalTilingFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
        if (w & PS5VK_FORMAT_CAP_BLIT_SRC)
            properties.optimalTilingFeatures |= VK_FORMAT_FEATURE_BLIT_SRC_BIT;
        if (w & PS5VK_FORMAT_CAP_BLIT_DST)
            properties.optimalTilingFeatures |= VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if (w & PS5VK_FORMAT_CAP_VERTEX_BUFFER)
            properties.bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
        if (w & PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER)
            properties.bufferFeatures |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
        if (w & PS5VK_FORMAT_CAP_STORAGE_TEXEL_BUFFER)
            properties.bufferFeatures |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
    }
    /* One linear-tiling role exists: the pinned upstream draw module's RGBA8
     * host-readback staging image, whose only usage is a transfer destination
     * and whose dimensions are single-mip, single-layer and single-sample
     * (vkGetPhysicalDeviceImageFormatProperties reports the same shape). Every
     * other format keeps a zeroed linearTilingFeatures field rather than
     * repeating the optimal-tiling bits. */
    if (format == VK_FORMAT_R8G8B8A8_UNORM)
        properties.linearTilingFeatures |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
#if defined(PS5VK_TEXTURE_COMPRESSION_BC_DIAGNOSTIC) && \
    PS5VK_TEXTURE_COMPRESSION_BC_DIAGNOSTIC
    /* The bounded BC1/BC3 CPU decode path writes a LINEAR RGBA8 transfer
     * destination. Publish the blit role only in its build-only profile. */
    if (format == VK_FORMAT_R8G8B8A8_UNORM)
        properties.linearTilingFeatures |= VK_FORMAT_FEATURE_BLIT_DST_BIT;
#endif
    *out = properties;
}

VkBool32 ps5vk_texture_format_image_usage(VkFormat format, VkImageUsageFlags usage)
{
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    if (!entry || !usage) return VK_FALSE;
    const uint32_t w = format_witnessed_capabilities(entry);
    const VkImageUsageFlags attachment = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const VkImageUsageFlags depth = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    /* Validate every requested role independently before testing the exact
     * combinations supported by the backend. In particular, sampling and
     * transfer-source support do not imply transfer-destination support. */
    const struct { VkImageUsageFlags usage; uint32_t capability; } roles[] = {
        {VK_IMAGE_USAGE_SAMPLED_BIT, PS5VK_FORMAT_CAP_SAMPLED_IMAGE},
        {VK_IMAGE_USAGE_TRANSFER_SRC_BIT, PS5VK_FORMAT_CAP_TRANSFER_SRC},
        {VK_IMAGE_USAGE_TRANSFER_DST_BIT, PS5VK_FORMAT_CAP_TRANSFER_DST},
        {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, PS5VK_FORMAT_CAP_COLOR_ATTACHMENT},
        {VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
         PS5VK_FORMAT_CAP_DEPTH_STENCIL_ATTACHMENT},
        {VK_IMAGE_USAGE_STORAGE_BIT, PS5VK_FORMAT_CAP_STORAGE_IMAGE},
    };
    for (unsigned i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i)
        if ((usage & roles[i].usage) && !(w & roles[i].capability))
            return VK_FALSE;
    /* Each implemented role contributes an exact combination; a usage the
     * profile has no executable path for is rejected here, before any object
     * exists. */
    if (ps5vk_texture_format_block_compressed(format) &&
        (w & (PS5VK_FORMAT_CAP_SAMPLED_IMAGE | PS5VK_FORMAT_CAP_TRANSFER_SRC |
              PS5VK_FORMAT_CAP_TRANSFER_DST)) ==
            (PS5VK_FORMAT_CAP_SAMPLED_IMAGE | PS5VK_FORMAT_CAP_TRANSFER_SRC |
             PS5VK_FORMAT_CAP_TRANSFER_DST) &&
        usage == (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return VK_TRUE;
    if ((w & PS5VK_FORMAT_CAP_SAMPLED_IMAGE) &&
        (usage == VK_IMAGE_USAGE_SAMPLED_BIT ||
         usage == (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)))
        return VK_TRUE;
    /* The BDA result image is a resource-only R32_UINT UAV over padded
     * linear backing. Its clear, compute write and readback use all three
     * declared roles in GENERAL. */
    if (format == VK_FORMAT_R32_UINT &&
        (w & (PS5VK_FORMAT_CAP_STORAGE_IMAGE | PS5VK_FORMAT_CAP_TRANSFER_SRC |
              PS5VK_FORMAT_CAP_TRANSFER_DST)) ==
            (PS5VK_FORMAT_CAP_STORAGE_IMAGE | PS5VK_FORMAT_CAP_TRANSFER_SRC |
             PS5VK_FORMAT_CAP_TRANSFER_DST) &&
        usage == (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return VK_TRUE;
    if ((w & PS5VK_FORMAT_CAP_TRANSFER_DST) && usage == VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        return VK_TRUE;
    /* The standalone transfer roles. A depth format is excluded: its transfer
     * source exists only as the readback of a depth ATTACHMENT (the rule
     * further down), and a D32 image that is nothing but a transfer surface
     * has no path here - it would be neither the tiled depth attachment the
     * Z_X equation describes nor a padded linear one. */
    if ((w & PS5VK_FORMAT_CAP_TRANSFER_SRC) &&
        format != VK_FORMAT_R32_UINT &&
        !(w & PS5VK_FORMAT_CAP_DEPTH_STENCIL_ATTACHMENT) &&
        (usage == VK_IMAGE_USAGE_TRANSFER_SRC_BIT ||
         usage == (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)))
        return VK_TRUE;
    if ((w & PS5VK_FORMAT_CAP_COLOR_ATTACHMENT) && usage == attachment) return VK_TRUE;
    /* Depth attachment plus its readback, and plus the whole-subresource
     * clear the same upstream cases perform. The pinned depth_clamp family
     * creates its depth target as DEPTH_STENCIL_ATTACHMENT | TRANSFER_SRC and
     * reads it back over the DEPTH aspect, so the combination has to exist
     * for the image to be creatable; it is granted only to a row that carries
     * both the depth-attachment and the transfer-source capability, which is
     * the one D32_SFLOAT shape whose Z_X pixel addressing is implemented. */
    if ((w & PS5VK_FORMAT_CAP_DEPTH_STENCIL_ATTACHMENT) &&
        (w & PS5VK_FORMAT_CAP_TRANSFER_SRC) &&
        (usage == (depth | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
         usage == (depth | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT))) return VK_TRUE;
    /* Colour attachment plus the readback role plus a transfer destination.
     * The pinned upstream draw tests create their colour target exactly this
     * way (COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST), so the combination
     * has to exist for their image to be creatable at all; only the row that
     * carries the readback capability has it, which keeps this to the one
     * R8G8B8A8_UNORM attachment shape whose clear and buffer-upload
     * destinations are implemented and witnessed. */
    if ((w & PS5VK_FORMAT_CAP_COLOR_ATTACHMENT_READBACK) &&
        usage == (attachment | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return VK_TRUE;
    if ((w & PS5VK_FORMAT_CAP_COLOR_ATTACHMENT_READBACK) &&
        usage == (attachment | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return VK_TRUE;
    /* The upstream render-pass module derives an attachment's usage from the
     * format's own reported features, so a format that also publishes a
     * sampled role is asked for
     * COLOR_ATTACHMENT|TRANSFER_SRC|TRANSFER_DST|SAMPLED. The readback colour
     * row is the one that carries all four roles (its format is sampled, it is
     * rendered into and it is read back), so the combination is admitted for
     * that row and for nothing else; the leaf that asks for it renders into the
     * target and reads it back, and a sample of a tiled attachment is refused
     * where it is actually described. */
    if (ps5vk_color_sampled_readback_served() &&
        (w & PS5VK_FORMAT_CAP_COLOR_ATTACHMENT_READBACK) &&
        usage == (attachment | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        return VK_TRUE;
    /* The pinned multiview helper's attachment adds the input-attachment role
     * to that same readback colour shape. Only the normalized row carries that
     * role: the integer colour target served since the independentBlend
     * promotion is a readback target but never a multiview or input-attachment
     * backing, so a row that only inherits the readback capability must not
     * gain the input role by accident. The usage set stays exact. */
    if (format == VK_FORMAT_R8G8B8A8_UNORM &&
        (w & PS5VK_FORMAT_CAP_COLOR_ATTACHMENT_READBACK) &&
        usage == (attachment | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return VK_TRUE;
    if ((w & PS5VK_FORMAT_CAP_DEPTH_STENCIL_ATTACHMENT) && usage == depth) return VK_TRUE;
    /* Depth target that vkCmdClearDepthStencilImage may clear. Vulkan requires
     * the transfer-destination usage on the cleared image, so the combination
     * has to exist for the command to be reachable at all. */
    if ((w & PS5VK_FORMAT_CAP_DEPTH_STENCIL_ATTACHMENT) &&
        (w & PS5VK_FORMAT_CAP_TRANSFER_DST) &&
        usage == (depth | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return VK_TRUE;
    return VK_FALSE;
}
