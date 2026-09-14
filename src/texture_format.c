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
 * that promote a pending capability.
 */
#include "texture_format.h"

/* Column aliases for the table below. */
#define GPL PS5VK_FORMAT_PROVENANCE_GPL_REFERENCE
#define PACK PS5VK_FORMAT_PROVENANCE_REGISTRY_PACKING
#define CAP_DST PS5VK_FORMAT_CAP_TRANSFER_DST
#define CAP_SRC PS5VK_FORMAT_CAP_TRANSFER_SRC
#define CAP_SAMP PS5VK_FORMAT_CAP_SAMPLED_IMAGE
#define CAP_LINEAR PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR
#define CAP_COLOR PS5VK_FORMAT_CAP_COLOR_ATTACHMENT
#define CAP_COLOR_READBACK PS5VK_FORMAT_CAP_COLOR_ATTACHMENT_READBACK
#define CAP_DEPTH PS5VK_FORMAT_CAP_DEPTH_STENCIL_ATTACHMENT
#define CAP_VERTEX PS5VK_FORMAT_CAP_VERTEX_BUFFER
#define CAP_UTEXEL PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER

/* Sampled row: the GFX1013 word/selectors/texel size are the pinned GPL
 * encoding; the pending column carries capabilities that are implemented and
 * host-tested but have no on-console witness yet. */
#define SAMPLED(f, bpt, word, s0, s1, s2, s3, EXTRA, PENDING) \
    { (f), (bpt), (word), {(s0), (s1), (s2), (s3)}, \
      CAP_SAMP | CAP_DST | (EXTRA) | (PENDING), CAP_SAMP | CAP_DST | (EXTRA), GPL }
/* Sampled row whose byte layout is the pinned registry's packed-component
 * order rather than one of the reference's explicit rows. WITNESSED lists
 * enabled capabilities, including the independently qualified sampled roles;
 * PENDING lists any remaining implemented-but-disabled operations. */
#define SAMPLED_PACKED(f, bpt, word, s0, s1, s2, s3, WITNESSED, PENDING) \
    { (f), (bpt), (word), {(s0), (s1), (s2), (s3)}, \
      CAP_SAMP | CAP_DST | (WITNESSED) | (PENDING), (WITNESSED), GPL | PACK }
/* Row with no sampled-image encoding: a render-target, depth or buffer role. */
#define BUFFER(f, CAPS) \
    { (f), 0, 0, {0, 0, 0, 0}, (CAPS), (CAPS), PS5VK_FORMAT_PROVENANCE_NONE }

static const struct ps5vk_texture_format formats[] = {
    /* --- 8-bit and packed sampled formats ---------------------------------
     * Every witnessed row has exact PS5 creation, upload, sampling and
     * readback evidence; the filterable rows additionally have two
     * byte-identical nearest/linear checkerboard runs. Integer rows use typed
     * isampler/usampler interfaces only, as Vulkan requires. */
    SAMPLED(VK_FORMAT_R8_UNORM, 1, UINT32_C(0x00100000), 4, 0, 0, 1,
            CAP_LINEAR | CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R8_SNORM, 1, UINT32_C(0x00200000), 4, 0, 0, 1,
            CAP_LINEAR | CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R8G8_UNORM, 2, UINT32_C(0x00e00000), 4, 5, 0, 1,
            CAP_LINEAR | CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R8G8_SNORM, 2, UINT32_C(0x00f00000), 4, 5, 0, 1,
            CAP_LINEAR | CAP_VERTEX, 0),
    /* The only colour attachment this profile both renders into and reads
     * back; the readback pair is a separate capability from the bare
     * attachment usage. */
    SAMPLED(VK_FORMAT_R8G8B8A8_UNORM, 4, UINT32_C(0x03800000), 4, 5, 6, 7,
            CAP_LINEAR | CAP_VERTEX | CAP_SRC | CAP_COLOR | CAP_COLOR_READBACK, 0),
    SAMPLED(VK_FORMAT_R8G8B8A8_SNORM, 4, UINT32_C(0x03900000), 4, 5, 6, 7,
            CAP_LINEAR | CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R8G8B8A8_SRGB, 4, UINT32_C(0x08200000), 4, 5, 6, 7,
            CAP_LINEAR, 0),
    SAMPLED(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4, UINT32_C(0x08400000), 4, 5, 6, 1,
            CAP_LINEAR, 0),
    SAMPLED(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, UINT32_C(0x02400000), 4, 5, 6, 1,
            CAP_LINEAR, 0),
    /* --- 16-bit sampled formats ------------------------------------------ */
    SAMPLED(VK_FORMAT_R16_UNORM, 2, UINT32_C(0x00700000), 4, 0, 0, 1,
            CAP_LINEAR | CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16_SNORM, 2, UINT32_C(0x00800000), 4, 0, 0, 1,
            CAP_LINEAR | CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16_SFLOAT, 2, UINT32_C(0x00d00000), 4, 0, 0, 1,
            CAP_LINEAR | CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16G16_UNORM, 4, UINT32_C(0x01700000), 4, 5, 0, 1,
            CAP_LINEAR | CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16G16_SNORM, 4, UINT32_C(0x01800000), 4, 5, 0, 1,
            CAP_LINEAR | CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16G16_SFLOAT, 4, UINT32_C(0x01d00000), 4, 5, 0, 1,
            CAP_LINEAR | CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16G16B16A16_UNORM, 8, UINT32_C(0x04100000), 4, 5, 6, 7,
            CAP_LINEAR | CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16G16B16A16_SNORM, 8, UINT32_C(0x04200000), 4, 5, 6, 7,
            CAP_LINEAR | CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16G16B16A16_SFLOAT, 8, UINT32_C(0x04700000), 4, 5, 6, 7,
            CAP_LINEAR | CAP_VERTEX, 0),
    /* --- 32-bit sampled formats ------------------------------------------ */
    SAMPLED(VK_FORMAT_R32_SFLOAT, 4, UINT32_C(0x01600000), 4, 0, 0, 1,
            CAP_LINEAR | CAP_VERTEX | CAP_UTEXEL, 0),
    SAMPLED(VK_FORMAT_R32G32_SFLOAT, 8, UINT32_C(0x04000000), 4, 5, 0, 1,
            CAP_LINEAR | CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R32G32B32A32_SFLOAT, 16, UINT32_C(0x04d00000), 4, 5, 6, 7,
            CAP_LINEAR | CAP_VERTEX, 0),
    /* --- typed integer sampled formats (nearest only) --------------------- */
    SAMPLED(VK_FORMAT_R8_UINT, 1, UINT32_C(0x00500000), 4, 0, 0, 1, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R8_SINT, 1, UINT32_C(0x00600000), 4, 0, 0, 1, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R8G8_UINT, 2, UINT32_C(0x01200000), 4, 5, 0, 1, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R8G8_SINT, 2, UINT32_C(0x01300000), 4, 5, 0, 1, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R8G8B8A8_UINT, 4, UINT32_C(0x03c00000), 4, 5, 6, 7, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R8G8B8A8_SINT, 4, UINT32_C(0x03d00000), 4, 5, 6, 7, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16_UINT, 2, UINT32_C(0x00b00000), 4, 0, 0, 1, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16_SINT, 2, UINT32_C(0x00c00000), 4, 0, 0, 1, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16G16_UINT, 4, UINT32_C(0x01b00000), 4, 5, 0, 1, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16G16_SINT, 4, UINT32_C(0x01c00000), 4, 5, 0, 1, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16G16B16A16_UINT, 8, UINT32_C(0x04500000), 4, 5, 6, 7, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R16G16B16A16_SINT, 8, UINT32_C(0x04600000), 4, 5, 6, 7, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R32_UINT, 4, UINT32_C(0x01400000), 4, 0, 0, 1,
            CAP_VERTEX | CAP_UTEXEL, 0),
    SAMPLED(VK_FORMAT_R32_SINT, 4, UINT32_C(0x01500000), 4, 0, 0, 1,
            CAP_VERTEX | CAP_UTEXEL, 0),
    SAMPLED(VK_FORMAT_R32G32_UINT, 8, UINT32_C(0x03e00000), 4, 5, 0, 1, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R32G32_SINT, 8, UINT32_C(0x03f00000), 4, 5, 0, 1, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R32G32B32A32_UINT, 16, UINT32_C(0x04b00000), 4, 5, 6, 7, CAP_VERTEX, 0),
    SAMPLED(VK_FORMAT_R32G32B32A32_SINT, 16, UINT32_C(0x04c00000), 4, 5, 6, 7, CAP_VERTEX, 0),
    /* --- packed sampled formats qualified on 2026-09-14 --------------------
     * `A8B8G8R8_*_PACK32` names the packed form whose R component occupies
     * bits 0-7 (the pinned registry's packed-component order), so its memory
     * layout is byte-identical to the R8G8B8A8_* row above: same GFX1013 word
     * and same identity selectors. Separate typed/normalized/sRGB sampling
     * and checkerboard filter diagnostics each passed twice with identical
     * SELF images. This does not promote attachment, storage or blit roles.
     * Exact artifacts and log digests are recorded in VALIDATION.md. */
    SAMPLED_PACKED(VK_FORMAT_A8B8G8R8_UNORM_PACK32, 4, UINT32_C(0x03800000), 4, 5, 6, 7,
                   CAP_VERTEX | CAP_SAMP | CAP_DST | CAP_LINEAR, 0),
    SAMPLED_PACKED(VK_FORMAT_A8B8G8R8_SNORM_PACK32, 4, UINT32_C(0x03900000), 4, 5, 6, 7,
                   CAP_VERTEX | CAP_SAMP | CAP_DST | CAP_LINEAR, 0),
    SAMPLED_PACKED(VK_FORMAT_A8B8G8R8_SRGB_PACK32, 4, UINT32_C(0x08200000), 4, 5, 6, 7,
                   CAP_SAMP | CAP_DST | CAP_LINEAR, 0),
    SAMPLED_PACKED(VK_FORMAT_A8B8G8R8_UINT_PACK32, 4, UINT32_C(0x03c00000), 4, 5, 6, 7,
                   CAP_VERTEX | CAP_SAMP | CAP_DST, 0),
    SAMPLED_PACKED(VK_FORMAT_A8B8G8R8_SINT_PACK32, 4, UINT32_C(0x03d00000), 4, 5, 6, 7,
                   CAP_VERTEX | CAP_SAMP | CAP_DST, 0),
    /* --- roles without a sampled-image encoding --------------------------- */
    /* VideoOut target and vertex input; deliberately not sampled. */
    BUFFER(VK_FORMAT_B8G8R8A8_UNORM, CAP_COLOR | CAP_VERTEX),
    /* 64KB_Z_X depth target; no pixel addressing for a clear or a copy. */
    BUFFER(VK_FORMAT_D32_SFLOAT, CAP_DEPTH),
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

VkBool32 ps5vk_texture_format_witnessed(VkFormat format, uint32_t capability)
{
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    return entry && capability && (entry->witnessed & capability) == capability;
}

VkBool32 ps5vk_texture_format_sampled_image(VkFormat format)
{
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    return entry && (entry->witnessed & PS5VK_FORMAT_CAP_SAMPLED_IMAGE) &&
        entry->descriptor_format_word != 0;
}

VkBool32 ps5vk_texture_format_sampled_encoding(VkFormat format)
{
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    return entry && (entry->capabilities & PS5VK_FORMAT_CAP_SAMPLED_IMAGE) &&
        entry->descriptor_format_word != 0 && entry->bytes_per_texel != 0;
}

void ps5vk_texture_format_properties(VkFormat format, VkFormatProperties *out)
{
    if (!out) return;
    VkFormatProperties properties = {0};
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    if (entry) {
        const uint32_t w = entry->witnessed;
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
    /* No linear-tiling image role exists in this profile; the field stays zero
     * rather than repeating the optimal-tiling bits. */
    *out = properties;
}

VkBool32 ps5vk_texture_format_image_usage(VkFormat format, VkImageUsageFlags usage)
{
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    if (!entry || !usage) return VK_FALSE;
    const uint32_t w = entry->witnessed;
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
    };
    for (unsigned i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i)
        if ((usage & roles[i].usage) && !(w & roles[i].capability))
            return VK_FALSE;
    /* Each implemented role contributes an exact combination; a usage the
     * profile has no executable path for is rejected here, before any object
     * exists. */
    if ((w & PS5VK_FORMAT_CAP_SAMPLED_IMAGE) &&
        (usage == VK_IMAGE_USAGE_SAMPLED_BIT ||
         usage == (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)))
        return VK_TRUE;
    if ((w & PS5VK_FORMAT_CAP_TRANSFER_DST) && usage == VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        return VK_TRUE;
    if ((w & PS5VK_FORMAT_CAP_TRANSFER_SRC) &&
        (usage == VK_IMAGE_USAGE_TRANSFER_SRC_BIT ||
         usage == (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)))
        return VK_TRUE;
    if ((w & PS5VK_FORMAT_CAP_COLOR_ATTACHMENT) && usage == attachment) return VK_TRUE;
    if ((w & PS5VK_FORMAT_CAP_COLOR_ATTACHMENT_READBACK) &&
        usage == (attachment | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return VK_TRUE;
    if ((w & PS5VK_FORMAT_CAP_DEPTH_STENCIL_ATTACHMENT) && usage == depth) return VK_TRUE;
    return VK_FALSE;
}
