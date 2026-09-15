#include "graphics_formats.h"
#include "texture_format.h"
#include <assert.h>
#include <string.h>

/* Table-driven contract tests over the authoritative capability table.
 * These prove the published/reported decisions, the GFX1013 encoding and the
 * capability split; they are not a GPU execution claim. */
static void check_encoding(VkFormat format, uint32_t bytes, uint32_t word,
                           uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3)
{
    const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(format);
    assert(entry);
    assert(entry->bytes_per_texel == bytes);
    assert(entry->descriptor_format_word == word);
    assert(entry->selectors[0] == s0 && entry->selectors[1] == s1 &&
           entry->selectors[2] == s2 && entry->selectors[3] == s3);
    assert(entry->provenance & PS5VK_FORMAT_PROVENANCE_GPL_REFERENCE);
    assert(ps5vk_texture_format_sampled_encoding(format));
}

static void check_vertex(VkFormat format, uint32_t bytes, uint32_t components,
                         enum ps5vk_vertex_numeric numeric)
{
    const struct ps5vk_vertex_format vertex = ps5vk_vertex_format_info(format);
    assert(ps5vk_texture_format_witnessed(format, PS5VK_FORMAT_CAP_VERTEX_BUFFER));
    assert(vertex.bytes == bytes && vertex.components == components);
    assert(vertex.numeric == numeric);
    assert(ps5vk_vertex_format_size(format) == bytes);
}

int main(void)
{
    /* --- GFX1013 sampled encodings (GPL-derived rows) --------------------- */
    check_encoding(VK_FORMAT_R8_UNORM, 1, UINT32_C(0x00100000), 4, 0, 0, 1);
    check_encoding(VK_FORMAT_R8_SNORM, 1, UINT32_C(0x00200000), 4, 0, 0, 1);
    check_encoding(VK_FORMAT_R8G8_UNORM, 2, UINT32_C(0x00e00000), 4, 5, 0, 1);
    check_encoding(VK_FORMAT_R8G8_SNORM, 2, UINT32_C(0x00f00000), 4, 5, 0, 1);
    check_encoding(VK_FORMAT_R8G8B8A8_UNORM, 4, UINT32_C(0x03800000), 4, 5, 6, 7);
    check_encoding(VK_FORMAT_R8G8B8A8_SNORM, 4, UINT32_C(0x03900000), 4, 5, 6, 7);
    check_encoding(VK_FORMAT_R8G8B8A8_SRGB, 4, UINT32_C(0x08200000), 4, 5, 6, 7);
    check_encoding(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4, UINT32_C(0x08400000), 4, 5, 6, 1);
    check_encoding(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, UINT32_C(0x02400000), 4, 5, 6, 1);
    check_encoding(VK_FORMAT_R16_UNORM, 2, UINT32_C(0x00700000), 4, 0, 0, 1);
    check_encoding(VK_FORMAT_R16_SNORM, 2, UINT32_C(0x00800000), 4, 0, 0, 1);
    check_encoding(VK_FORMAT_R16_SFLOAT, 2, UINT32_C(0x00d00000), 4, 0, 0, 1);
    check_encoding(VK_FORMAT_R16G16_UNORM, 4, UINT32_C(0x01700000), 4, 5, 0, 1);
    check_encoding(VK_FORMAT_R16G16_SNORM, 4, UINT32_C(0x01800000), 4, 5, 0, 1);
    check_encoding(VK_FORMAT_R16G16_SFLOAT, 4, UINT32_C(0x01d00000), 4, 5, 0, 1);
    check_encoding(VK_FORMAT_R16G16B16A16_UNORM, 8, UINT32_C(0x04100000), 4, 5, 6, 7);
    check_encoding(VK_FORMAT_R16G16B16A16_SNORM, 8, UINT32_C(0x04200000), 4, 5, 6, 7);
    check_encoding(VK_FORMAT_R16G16B16A16_SFLOAT, 8, UINT32_C(0x04700000), 4, 5, 6, 7);
    check_encoding(VK_FORMAT_R32_SFLOAT, 4, UINT32_C(0x01600000), 4, 0, 0, 1);
    check_encoding(VK_FORMAT_R32G32_SFLOAT, 8, UINT32_C(0x04000000), 4, 5, 0, 1);
    check_encoding(VK_FORMAT_R32G32B32A32_SFLOAT, 16, UINT32_C(0x04d00000), 4, 5, 6, 7);
    check_encoding(VK_FORMAT_R8_UINT, 1, UINT32_C(0x00500000), 4, 0, 0, 1);
    check_encoding(VK_FORMAT_R8_SINT, 1, UINT32_C(0x00600000), 4, 0, 0, 1);
    check_encoding(VK_FORMAT_R8G8_UINT, 2, UINT32_C(0x01200000), 4, 5, 0, 1);
    check_encoding(VK_FORMAT_R8G8_SINT, 2, UINT32_C(0x01300000), 4, 5, 0, 1);
    check_encoding(VK_FORMAT_R8G8B8A8_UINT, 4, UINT32_C(0x03c00000), 4, 5, 6, 7);
    check_encoding(VK_FORMAT_R8G8B8A8_SINT, 4, UINT32_C(0x03d00000), 4, 5, 6, 7);
    check_encoding(VK_FORMAT_R16_UINT, 2, UINT32_C(0x00b00000), 4, 0, 0, 1);
    check_encoding(VK_FORMAT_R16_SINT, 2, UINT32_C(0x00c00000), 4, 0, 0, 1);
    check_encoding(VK_FORMAT_R16G16_UINT, 4, UINT32_C(0x01b00000), 4, 5, 0, 1);
    check_encoding(VK_FORMAT_R16G16_SINT, 4, UINT32_C(0x01c00000), 4, 5, 0, 1);
    check_encoding(VK_FORMAT_R16G16B16A16_UINT, 8, UINT32_C(0x04500000), 4, 5, 6, 7);
    check_encoding(VK_FORMAT_R16G16B16A16_SINT, 8, UINT32_C(0x04600000), 4, 5, 6, 7);
    check_encoding(VK_FORMAT_R32_UINT, 4, UINT32_C(0x01400000), 4, 0, 0, 1);
    check_encoding(VK_FORMAT_R32_SINT, 4, UINT32_C(0x01500000), 4, 0, 0, 1);
    check_encoding(VK_FORMAT_R32G32_UINT, 8, UINT32_C(0x03e00000), 4, 5, 0, 1);
    check_encoding(VK_FORMAT_R32G32_SINT, 8, UINT32_C(0x03f00000), 4, 5, 0, 1);
    check_encoding(VK_FORMAT_R32G32B32A32_UINT, 16, UINT32_C(0x04b00000), 4, 5, 6, 7);
    check_encoding(VK_FORMAT_R32G32B32A32_SINT, 16, UINT32_C(0x04c00000), 4, 5, 6, 7);

    /* --- integer formats never claim linear filtering --------------------- */
    const VkFormat integer_formats[] = {
        VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
        VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT,
        VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
        VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
        VK_FORMAT_A8B8G8R8_UINT_PACK32, VK_FORMAT_A8B8G8R8_SINT_PACK32,
    };
    for (unsigned i = 0; i < sizeof(integer_formats) / sizeof(integer_formats[0]); ++i) {
        VkFormat format = integer_formats[i];
        assert(!ps5vk_texture_format_has(format, PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR));
        VkFormatProperties properties;
        memset(&properties, 0xff, sizeof(properties));
        ps5vk_texture_format_properties(format, &properties);
        if (ps5vk_texture_format_sampled_image(format))
            assert(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        assert(!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT));
    }

    /* --- packed formats: byte-identical to their RGBA8 counterpart -------- */
    const VkFormat packed[] = {
        VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_FORMAT_A8B8G8R8_SNORM_PACK32,
        VK_FORMAT_A8B8G8R8_SRGB_PACK32, VK_FORMAT_A8B8G8R8_UINT_PACK32,
        VK_FORMAT_A8B8G8R8_SINT_PACK32,
    };
    const uint32_t packed_words[] = {
        UINT32_C(0x03800000), UINT32_C(0x03900000), UINT32_C(0x08200000),
        UINT32_C(0x03c00000), UINT32_C(0x03d00000),
    };
    for (unsigned i = 0; i < sizeof(packed) / sizeof(packed[0]); ++i) {
        check_encoding(packed[i], 4, packed_words[i], 4, 5, 6, 7);
        const struct ps5vk_texture_format *entry =
            ps5vk_texture_format_lookup(packed[i]);
        assert(entry->provenance & PS5VK_FORMAT_PROVENANCE_REGISTRY_PACKING);
    }

    /* --- packed sampled/filter capabilities ------------------------------- */
    const int packed_linear[] = {1, 1, 1, 0, 0};
    for (unsigned i = 0; i < sizeof(packed) / sizeof(packed[0]); ++i) {
        VkFormat format = packed[i];
        assert(ps5vk_texture_format_has(format, PS5VK_FORMAT_CAP_SAMPLED_IMAGE));
        assert(ps5vk_texture_format_witnessed(format,
            PS5VK_FORMAT_CAP_SAMPLED_IMAGE | PS5VK_FORMAT_CAP_TRANSFER_DST));
        assert(ps5vk_texture_format_sampled_image(format));
        /* Integer packed rows stay nearest-only. */
        assert(ps5vk_texture_format_has(format, PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR) ==
               (VkBool32)(packed_linear[i] != 0));
        VkFormatProperties properties;
        memset(&properties, 0xff, sizeof(properties));
        ps5vk_texture_format_properties(format, &properties);
        assert(properties.optimalTilingFeatures ==
            (VkFormatFeatureFlags)(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                (packed_linear[i] ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0)));
        if (format == VK_FORMAT_A8B8G8R8_SRGB_PACK32)
            assert(!properties.bufferFeatures);
        else
            assert(properties.bufferFeatures ==
                   (VkFormatFeatureFlags)(VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
                                          VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT));
        assert(ps5vk_texture_format_image_usage(format, VK_IMAGE_USAGE_SAMPLED_BIT));
        assert(ps5vk_texture_format_image_usage(format,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
        assert(!ps5vk_texture_format_image_usage(format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
        assert(!ps5vk_texture_format_image_usage(format, VK_IMAGE_USAGE_STORAGE_BIT));
        assert(!ps5vk_texture_format_image_usage(format, VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
        assert(ps5vk_texture_format_sampled_encoding(format));
    }
    assert(ps5vk_texture_format_has(VK_FORMAT_A8B8G8R8_UNORM_PACK32,
        PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR));
    assert(!ps5vk_texture_format_has(VK_FORMAT_A8B8G8R8_UINT_PACK32,
        PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR));

    /* --- packed rows: directly witnessed uniform texel buffers ------------
     * The four non-sRGB A8B8G8R8 rows have the R8G8B8A8 memory layout, word
     * and selectors, so the descriptor path the RGBA8 witness established
     * encodes them unchanged. They must therefore report the capability as
     * enabled after the direct typed matrix witness. sRGB deliberately has no
     * such role because an sRGB buffer view needs its own decode contract. */
    const VkFormat packed_texel[] = {
        VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_FORMAT_A8B8G8R8_SNORM_PACK32,
        VK_FORMAT_A8B8G8R8_UINT_PACK32, VK_FORMAT_A8B8G8R8_SINT_PACK32,
    };
    const uint32_t packed_texel_words[] = {
        UINT32_C(56), UINT32_C(57), UINT32_C(60), UINT32_C(61),
    };
    for (unsigned i = 0; i < sizeof(packed_texel) / sizeof(packed_texel[0]); ++i) {
        const struct ps5vk_texture_format *entry =
            ps5vk_texture_format_lookup(packed_texel[i]);
        VkFormatProperties packed_properties;
        assert(entry && entry->bytes_per_texel == 4);
        assert(ps5vk_texture_format_has(packed_texel[i],
                                        PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        assert(ps5vk_texture_format_witnessed(packed_texel[i],
                                              PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        assert(ps5vk_texture_format_gfx10_format(entry) == packed_texel_words[i]);
        assert(ps5vk_texture_format_dst_sel(entry) == UINT32_C(0xfac));
        ps5vk_texture_format_properties(packed_texel[i], &packed_properties);
        assert(packed_properties.bufferFeatures &
               VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
    }
    assert(!ps5vk_texture_format_has(VK_FORMAT_A8B8G8R8_SRGB_PACK32,
                                     PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));

    /* --- one-byte rows: directly witnessed uniform texel buffers --------- */
    const VkFormat r8_texel[] = {
        VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
    };
    const uint32_t r8_texel_words[] = {1u, 2u, 5u, 6u};
    for (unsigned i = 0; i < sizeof(r8_texel) / sizeof(r8_texel[0]); ++i) {
        const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(r8_texel[i]);
        VkFormatProperties r8_properties;
        assert(entry && entry->bytes_per_texel == 1);
        assert(ps5vk_texture_format_has(r8_texel[i],
                                        PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        assert(ps5vk_texture_format_witnessed(r8_texel[i],
                                              PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        /* 8_UNORM/8_SNORM/8_UINT/8_SINT in the pinned GFX10 format table. */
        assert(ps5vk_texture_format_gfx10_format(entry) == r8_texel_words[i]);
        assert(ps5vk_texture_format_dst_sel(entry) == UINT32_C(0x204));
        ps5vk_texture_format_properties(r8_texel[i], &r8_properties);
        assert(r8_properties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
    }

    /* --- two-byte two-component rows: directly witnessed ----------------- */
    const VkFormat r8g8_texel[] = {
        VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM,
        VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
    };
    const uint32_t r8g8_texel_words[] = {14u, 15u, 18u, 19u};
    for (unsigned i = 0; i < sizeof(r8g8_texel) / sizeof(r8g8_texel[0]); ++i) {
        const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(r8g8_texel[i]);
        VkFormatProperties r8g8_properties;
        assert(entry && entry->bytes_per_texel == 2);
        assert(ps5vk_texture_format_has(r8g8_texel[i],
                                        PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        assert(ps5vk_texture_format_witnessed(r8g8_texel[i],
                                              PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        /* 8_8_UNORM/8_8_SNORM/8_8_UINT/8_8_SINT in the pinned GFX10 table. */
        assert(ps5vk_texture_format_gfx10_format(entry) == r8g8_texel_words[i]);
        assert(ps5vk_texture_format_dst_sel(entry) == UINT32_C(0x22c));
        ps5vk_texture_format_properties(r8g8_texel[i], &r8g8_properties);
        assert(r8g8_properties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
    }

    /* --- single-component 16-bit rows: same staging rule -------------------
     * R16_UNORM/SNORM/SFLOAT/UINT/SINT carry a two-byte element with the
     * (4,0,0,1) completion, so the derived completion word is 0x204 and the
     * combined GFX10 words are 7/8/13/11/12 for UNORM/SNORM/FLOAT/UINT/SINT.
     * The mandatory 16-bit table requires the role for SFLOAT, UINT and SINT;
     * UNORM and SNORM are enabled with them after the same direct witness. */
    const VkFormat r16_texel[] = {
        VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
    };
    const uint32_t r16_texel_words[] = {7u, 8u, 13u, 11u, 12u};
    for (unsigned i = 0; i < sizeof(r16_texel) / sizeof(r16_texel[0]); ++i) {
        const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(r16_texel[i]);
        VkFormatProperties r16_properties;
        assert(entry && entry->bytes_per_texel == 2);
        assert(ps5vk_texture_format_has(r16_texel[i],
                                        PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        assert(ps5vk_texture_format_witnessed(r16_texel[i],
                                              PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        /* 16_UNORM/16_SNORM/16_FLOAT/16_UINT/16_SINT in the pinned table. */
        assert(ps5vk_texture_format_gfx10_format(entry) == r16_texel_words[i]);
        assert(ps5vk_texture_format_dst_sel(entry) == UINT32_C(0x204));
        ps5vk_texture_format_properties(r16_texel[i], &r16_properties);
        assert(r16_properties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
    }

    /* --- two-component 16-bit rows: same staging rule ----------------------
     * R16G16_UNORM/SNORM/SFLOAT/UINT/SINT carry a four-byte element with the
     * (4,5,0,1) completion, so the derived completion word is 0x22C and the
     * combined GFX10 words are 23/24/29/27/28. The mandatory 16-bit table
     * requires the role for SFLOAT, UINT and SINT; UNORM and SNORM are staged
     * with them for family coherence after the same direct witness. */
    const VkFormat r16g16_texel[] = {
        VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT,
    };
    const uint32_t r16g16_texel_words[] = {23u, 24u, 29u, 27u, 28u};
    for (unsigned i = 0; i < sizeof(r16g16_texel) / sizeof(r16g16_texel[0]); ++i) {
        const struct ps5vk_texture_format *entry =
            ps5vk_texture_format_lookup(r16g16_texel[i]);
        VkFormatProperties r16g16_properties;
        assert(entry && entry->bytes_per_texel == 4);
        assert(ps5vk_texture_format_has(r16g16_texel[i],
                                        PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        assert(ps5vk_texture_format_witnessed(r16g16_texel[i],
                                              PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        /* 16_16_UNORM/SNORM/FLOAT/UINT/SINT in the pinned GFX10 table. */
        assert(ps5vk_texture_format_gfx10_format(entry) == r16g16_texel_words[i]);
        assert(ps5vk_texture_format_dst_sel(entry) == UINT32_C(0x22c));
        ps5vk_texture_format_properties(r16g16_texel[i], &r16g16_properties);
        assert(r16g16_properties.bufferFeatures &
               VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
    }

    /* --- four-component 16-bit rows: same staging rule ---------------------
     * R16G16B16A16_UNORM/SNORM/SFLOAT/UINT/SINT carry an eight-byte element
     * with the (4,5,6,7) completion, so the derived completion word is 0xfac
     * and the combined GFX10 words are 65/66/71/69/70. This is the first
     * family with an eight-byte element: the V# stride field is bits 16-29 of
     * word 1, so 8 fits, and NUM_RECORDS is bytes/bytes_per_texel. The
     * mandatory 16-bit table requires the role for SFLOAT, UINT and SINT;
     * UNORM and SNORM have no mandatory cell at all and are staged only for
     * family coherence after the same direct witness. */
    const VkFormat r16g16b16a16_texel[] = {
        VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UINT,
        VK_FORMAT_R16G16B16A16_SINT,
    };
    const uint32_t r16g16b16a16_texel_words[] = {65u, 66u, 71u, 69u, 70u};
    for (unsigned i = 0;
         i < sizeof(r16g16b16a16_texel) / sizeof(r16g16b16a16_texel[0]); ++i) {
        const struct ps5vk_texture_format *entry =
            ps5vk_texture_format_lookup(r16g16b16a16_texel[i]);
        VkFormatProperties r16g16b16a16_properties;
        assert(entry && entry->bytes_per_texel == 8);
        assert(ps5vk_texture_format_has(r16g16b16a16_texel[i],
                                        PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        assert(ps5vk_texture_format_witnessed(r16g16b16a16_texel[i],
                                              PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        /* 16_16_16_16_UNORM/SNORM/FLOAT/UINT/SINT in the pinned table. */
        assert(ps5vk_texture_format_gfx10_format(entry) ==
               r16g16b16a16_texel_words[i]);
        assert(ps5vk_texture_format_dst_sel(entry) == UINT32_C(0xfac));
        ps5vk_texture_format_properties(r16g16b16a16_texel[i],
                                        &r16g16b16a16_properties);
        assert(r16g16b16a16_properties.bufferFeatures &
               VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
    }

    /* --- two-component 32-bit rows: same staging rule ----------------------
     * R32G32_UINT/SINT/SFLOAT carry an eight-byte element with the (4,5,0,1)
     * completion, so the derived completion word is 0x22C and the combined
     * GFX10 words are 62/63/64. Unlike the 16-bit families, the mandatory
     * 32-bit table requires the role for ALL THREE of these rows, so there is
     * no coherence-only row here. All three have a direct console fetch. */
    const VkFormat r32g32_texel[] = {
        VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_SFLOAT,
    };
    const uint32_t r32g32_texel_words[] = {62u, 63u, 64u};
    for (unsigned i = 0; i < sizeof(r32g32_texel) / sizeof(r32g32_texel[0]); ++i) {
        const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(r32g32_texel[i]);
        VkFormatProperties r32g32_properties;
        assert(entry && entry->bytes_per_texel == 8);
        assert(ps5vk_texture_format_has(r32g32_texel[i],
                                        PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        assert(ps5vk_texture_format_witnessed(r32g32_texel[i],
                                              PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        /* 32_32_UINT/32_32_SINT/32_32_FLOAT in the pinned GFX10 table. */
        assert(ps5vk_texture_format_gfx10_format(entry) == r32g32_texel_words[i]);
        assert(ps5vk_texture_format_dst_sel(entry) == UINT32_C(0x22c));
        ps5vk_texture_format_properties(r32g32_texel[i], &r32g32_properties);
        assert(r32g32_properties.bufferFeatures &
               VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
    }

    /* --- four-component 32-bit rows: same staging rule ---------------------
     * R32G32B32A32_UINT/SINT/SFLOAT carry a sixteen-byte element with the
     * (4,5,6,7) completion, so the derived completion word is 0xfac and the
     * combined GFX10 words are 75/76/77. All three are mandatory
     * UNIFORM_TEXEL cells in both profiles, so none is a coherence-only row.
     * The sixteen-byte stride fits the V# stride field (bits 16-29 of word 1,
     * 0x3fff maximum) and the encoder accepts element sizes up to 16.
     * All three have a direct sixteen-byte console fetch. */
    const VkFormat r32g32b32a32_texel[] = {
        VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
        VK_FORMAT_R32G32B32A32_SFLOAT,
    };
    const uint32_t r32g32b32a32_texel_words[] = {75u, 76u, 77u};
    for (unsigned i = 0;
         i < sizeof(r32g32b32a32_texel) / sizeof(r32g32b32a32_texel[0]); ++i) {
        const struct ps5vk_texture_format *entry =
            ps5vk_texture_format_lookup(r32g32b32a32_texel[i]);
        VkFormatProperties r32g32b32a32_properties;
        assert(entry && entry->bytes_per_texel == 16);
        assert(ps5vk_texture_format_has(r32g32b32a32_texel[i],
                                        PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        assert(ps5vk_texture_format_witnessed(r32g32b32a32_texel[i],
                                              PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        /* 32_32_32_32_UINT/SINT/FLOAT in the pinned GFX10 table. */
        assert(ps5vk_texture_format_gfx10_format(entry) ==
               r32g32b32a32_texel_words[i]);
        assert(ps5vk_texture_format_dst_sel(entry) == UINT32_C(0xfac));
        ps5vk_texture_format_properties(r32g32b32a32_texel[i],
                                        &r32g32b32a32_properties);
        assert(r32g32b32a32_properties.bufferFeatures &
               VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
    }

    /* --- packed three-component float row: same staging rule --------------
     * B10G11R11_UFLOAT_PACK32 is a four-byte element with selectors
     * (4,5,6,1) - Vulkan's completion for a three-component float format is
     * (R,G,B,1) - so the derived completion word is 0x3ac and the combined
     * GFX10 word is 36 (10_11_11_FLOAT). Its mandatory UNIFORM_TEXEL cell is
     * a blocker in BOTH profiles of formats-mandatory-features-64bit, while
     * sampled image and linear filtering are already satisfied. Its packed
     * float decode now has a direct console fetch. */
    {
        const VkFormat packed_texel_float = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        const struct ps5vk_texture_format *entry =
            ps5vk_texture_format_lookup(packed_texel_float);
        VkFormatProperties packed_texel_float_properties;
        assert(entry && entry->bytes_per_texel == 4);
        assert(ps5vk_texture_format_has(packed_texel_float,
                                        PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        assert(ps5vk_texture_format_witnessed(packed_texel_float,
                                              PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        assert(ps5vk_texture_format_gfx10_format(entry) == 36u);
        assert(ps5vk_texture_format_dst_sel(entry) == UINT32_C(0x3ac));
        ps5vk_texture_format_properties(packed_texel_float,
                                        &packed_texel_float_properties);
        assert(packed_texel_float_properties.bufferFeatures &
               VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
    }

    /* --- vertex metadata matches the published vertex role ---------------- */
    check_vertex(VK_FORMAT_R8_UNORM, 1, 1, PS5VK_VERTEX_NUMERIC_FLOAT);
    check_vertex(VK_FORMAT_R8G8_SNORM, 2, 2, PS5VK_VERTEX_NUMERIC_FLOAT);
    check_vertex(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, PS5VK_VERTEX_NUMERIC_FLOAT);
    check_vertex(VK_FORMAT_B8G8R8A8_UNORM, 4, 4, PS5VK_VERTEX_NUMERIC_FLOAT);
    check_vertex(VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 4, PS5VK_VERTEX_NUMERIC_FLOAT);
    check_vertex(VK_FORMAT_R8_UINT, 1, 1, PS5VK_VERTEX_NUMERIC_UINT);
    check_vertex(VK_FORMAT_R16_SINT, 2, 1, PS5VK_VERTEX_NUMERIC_SINT);
    check_vertex(VK_FORMAT_R16G16B16A16_SFLOAT, 8, 4, PS5VK_VERTEX_NUMERIC_FLOAT);
    check_vertex(VK_FORMAT_R32_SFLOAT, 4, 1, PS5VK_VERTEX_NUMERIC_FLOAT);
    check_vertex(VK_FORMAT_R32G32_SFLOAT, 8, 2, PS5VK_VERTEX_NUMERIC_FLOAT);
    check_vertex(VK_FORMAT_R32G32B32_SFLOAT, 12, 3, PS5VK_VERTEX_NUMERIC_FLOAT);
    check_vertex(VK_FORMAT_R32G32B32A32_SFLOAT, 16, 4, PS5VK_VERTEX_NUMERIC_FLOAT);
    check_vertex(VK_FORMAT_R32G32B32_UINT, 12, 3, PS5VK_VERTEX_NUMERIC_UINT);
    check_vertex(VK_FORMAT_R32G32B32_SINT, 12, 3, PS5VK_VERTEX_NUMERIC_SINT);

    /* --- published properties for the advertised rows --------------------- */
    VkFormatProperties properties;
    ps5vk_texture_format_properties(VK_FORMAT_R8G8B8A8_UNORM, &properties);
    assert(properties.optimalTilingFeatures ==
        (VkFormatFeatureFlags)(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
         VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT));
    assert(properties.bufferFeatures ==
        (VkFormatFeatureFlags)(VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
                               VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT));
    assert(!properties.linearTilingFeatures);
    ps5vk_texture_format_properties(VK_FORMAT_B8G8R8A8_UNORM, &properties);
    assert(properties.optimalTilingFeatures == (VkFormatFeatureFlags)VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    assert(properties.bufferFeatures == (VkFormatFeatureFlags)VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
    /* The depth target advertises the transfer destination the whole-subresource
     * vkCmdClearDepthStencilImage consumes, and nothing else: there is no
     * transfer source, no sampled role and no blit role for 64KB_Z_X. */
    ps5vk_texture_format_properties(VK_FORMAT_D32_SFLOAT, &properties);
    assert(properties.optimalTilingFeatures ==
        (VkFormatFeatureFlags)(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                               VK_FORMAT_FEATURE_TRANSFER_DST_BIT));
    assert(!properties.bufferFeatures && !properties.linearTilingFeatures);
    ps5vk_texture_format_properties(VK_FORMAT_R32_UINT, &properties);
    assert(properties.bufferFeatures ==
        (VkFormatFeatureFlags)(VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
                               VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT));
    /* --- witnessed uniform texel buffer family ----------------------------
     * The four R8G8B8A8 rows carry the role in both masks after the two-run
     * RGBA8 texelFetch witness recorded in VALIDATION.md. The derivation
     * helpers must return the pinned reference values: the GFX10 combined
     * 8_8_8_8 words are 56/57/60/61, four-component identity completion is
     * (X,Y,Z,W) = 0xfac, and a one-component row keeps (X,0,0,1) = 0x204. */
    const VkFormat texel_formats[] = {
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
    };
    const uint32_t texel_words[] = {56u, 57u, 60u, 61u};
    for (unsigned i = 0; i < sizeof(texel_formats) / sizeof(texel_formats[0]); ++i) {
        const struct ps5vk_texture_format *entry = ps5vk_texture_format_lookup(texel_formats[i]);
        assert(entry && entry->bytes_per_texel == 4);
        assert(ps5vk_texture_format_has(texel_formats[i],
                                        PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        assert(ps5vk_texture_format_witnessed(texel_formats[i],
                                              PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
        assert(ps5vk_texture_format_gfx10_format(entry) == texel_words[i]);
        assert(ps5vk_texture_format_dst_sel(entry) == UINT32_C(0xfac));
        ps5vk_texture_format_properties(texel_formats[i], &properties);
        assert(properties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
    }
    assert(ps5vk_texture_format_gfx10_format(
        ps5vk_texture_format_lookup(VK_FORMAT_R32_UINT)) == 20u);
    assert(ps5vk_texture_format_dst_sel(
        ps5vk_texture_format_lookup(VK_FORMAT_R32_UINT)) == UINT32_C(0x204));
    assert(!ps5vk_texture_format_gfx10_format(NULL));
    assert(!ps5vk_texture_format_dst_sel(NULL));
    /* A row with no such implementation stays out of the role, including the
     * sRGB member of the same byte size and the BGRA row whose channel order
     * would need its own completion. */
    assert(!ps5vk_texture_format_has(VK_FORMAT_R8G8B8A8_SRGB,
                                     PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
    assert(!ps5vk_texture_format_has(VK_FORMAT_B8G8R8A8_UNORM,
                                     PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER));
    ps5vk_texture_format_properties(VK_FORMAT_R32G32B32_SFLOAT, &properties);
    assert(properties.bufferFeatures == (VkFormatFeatureFlags)VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
    assert(!properties.optimalTilingFeatures);
    assert(!ps5vk_texture_format_sampled_encoding(VK_FORMAT_R32G32B32_SFLOAT));

    /* --- no capability is published without a witness --------------------- */
    const VkFormat all_formats[] = {
        VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32,
        VK_FORMAT_A8B8G8R8_SNORM_PACK32, VK_FORMAT_A8B8G8R8_SRGB_PACK32,
        VK_FORMAT_A8B8G8R8_UINT_PACK32, VK_FORMAT_A8B8G8R8_SINT_PACK32,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
        VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_UNORM,
        VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UINT,
        VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UINT,
        VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SINT,
        VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_SINT,
        VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_SINT,
        VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_UINT,
    };
    for (unsigned i = 0; i < sizeof(all_formats) / sizeof(all_formats[0]); ++i) {
        uint32_t capabilities = ps5vk_texture_format_capabilities(all_formats[i]);
        const struct ps5vk_texture_format *entry =
            ps5vk_texture_format_lookup(all_formats[i]);
        assert(entry && capabilities);
        /* witnessed is a subset of capabilities, and linear filtering implies
         * a sampled-image role. */
        assert((entry->witnessed & ~capabilities) == 0);
        /* Every accepted combination must carry each requested role. Cover
         * all subsets of the core usage bits, not only today's happy paths. */
        const struct { VkImageUsageFlags usage; uint32_t capability; } roles[] = {
            {VK_IMAGE_USAGE_TRANSFER_SRC_BIT, PS5VK_FORMAT_CAP_TRANSFER_SRC},
            {VK_IMAGE_USAGE_TRANSFER_DST_BIT, PS5VK_FORMAT_CAP_TRANSFER_DST},
            {VK_IMAGE_USAGE_SAMPLED_BIT, PS5VK_FORMAT_CAP_SAMPLED_IMAGE},
            {VK_IMAGE_USAGE_STORAGE_BIT, PS5VK_FORMAT_CAP_STORAGE_IMAGE},
            {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, PS5VK_FORMAT_CAP_COLOR_ATTACHMENT},
            {VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
             PS5VK_FORMAT_CAP_DEPTH_STENCIL_ATTACHMENT},
        };
        for (VkImageUsageFlags usage = 1; usage < 256; ++usage) {
            if (!ps5vk_texture_format_image_usage(all_formats[i], usage)) continue;
            for (unsigned r = 0; r < sizeof(roles) / sizeof(roles[0]); ++r)
                assert(!(usage & roles[r].usage) ||
                       (entry->witnessed & roles[r].capability));
        }
        assert(!(entry->capabilities & PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR) ||
               (entry->capabilities & PS5VK_FORMAT_CAP_SAMPLED_IMAGE));
        memset(&properties, 0xff, sizeof(properties));
        ps5vk_texture_format_properties(all_formats[i], &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)
            assert(ps5vk_texture_format_witnessed(all_formats[i],
                PS5VK_FORMAT_CAP_COLOR_ATTACHMENT_BLEND));
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
            assert(ps5vk_texture_format_witnessed(all_formats[i],
                PS5VK_FORMAT_CAP_STORAGE_IMAGE));
        assert(!(properties.bufferFeatures & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT));
        assert(!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT));
        assert(!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT));
    }

    /* --- unknown formats are rejected, not defaulted ---------------------- */
    const VkFormat unknown[] = {
        VK_FORMAT_UNDEFINED, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8_USCALED,
        VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_R8G8B8_UNORM,
        VK_FORMAT_R8G8_USCALED, VK_FORMAT_R16G16_SSCALED,
    };
    for (unsigned i = 0; i < sizeof(unknown) / sizeof(unknown[0]); ++i) {
        assert(!ps5vk_texture_format_lookup(unknown[i]));
        assert(!ps5vk_texture_format_capabilities(unknown[i]));
        assert(!ps5vk_texture_format_has(unknown[i], PS5VK_FORMAT_CAP_SAMPLED_IMAGE));
        assert(!ps5vk_texture_format_witnessed(unknown[i], PS5VK_FORMAT_CAP_VERTEX_BUFFER));
        assert(!ps5vk_texture_format_sampled_image(unknown[i]));
        assert(!ps5vk_texture_format_sampled_encoding(unknown[i]));
        assert(!ps5vk_texture_format_image_usage(unknown[i], VK_IMAGE_USAGE_SAMPLED_BIT));
        memset(&properties, 0xff, sizeof(properties));
        ps5vk_texture_format_properties(unknown[i], &properties);
        assert(!properties.optimalTilingFeatures && !properties.bufferFeatures &&
               !properties.linearTilingFeatures);
        assert(!ps5vk_vertex_format_size(unknown[i]));
    }
    /* The zero-capability query never matches a zero mask either. */
    assert(!ps5vk_texture_format_has(VK_FORMAT_R8_UNORM, 0));
    assert(!ps5vk_texture_format_witnessed(VK_FORMAT_R8_UNORM, 0));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_R8_UNORM, 0));

    /* --- exact accepted image-usage combinations -------------------------- */
    const VkImageUsageFlags sampled_combos[] = {
        VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };
    for (unsigned i = 0; i < sizeof(sampled_combos) / sizeof(sampled_combos[0]); ++i)
        assert(ps5vk_texture_format_image_usage(VK_FORMAT_R8_UNORM, sampled_combos[i]));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    /* BGRA8 is a colour attachment only: no sampled role, no readback. */
    assert(ps5vk_texture_format_image_usage(VK_FORMAT_B8G8R8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_B8G8R8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_B8G8R8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT));
    /* D32 is a depth/stencil attachment, optionally clearable. */
    assert(ps5vk_texture_format_image_usage(VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
    assert(ps5vk_texture_format_image_usage(VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    /* The clear destination is a role on its own: Vulkan asks only for
     * transfer-destination usage on a cleared image, and such a D32 image is
     * still the tiled depth surface, not a padded linear one. */
    assert(ps5vk_texture_format_image_usage(VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT));
    /* RGBA8 has the transfer pair, the sampled route and the attachment with
     * its readback pair; nothing else. */
    assert(ps5vk_texture_format_image_usage(VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    assert(ps5vk_texture_format_image_usage(VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    assert(ps5vk_texture_format_image_usage(VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
    /* Vertex-only formats have no image role at all. */
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_R32G32B32_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_IMAGE_USAGE_SAMPLED_BIT));

    /* A null output is ignored rather than dereferenced. */
    ps5vk_texture_format_properties(VK_FORMAT_R8_UNORM, NULL);

    /* --- whole-table invariants ------------------------------------------ */
    const unsigned count = ps5vk_texture_format_count();
    assert(count >= sizeof(all_formats) / sizeof(all_formats[0]));
    assert(!ps5vk_texture_format_at(count));
    for (unsigned i = 0; i < count; ++i) {
        const struct ps5vk_texture_format *entry = ps5vk_texture_format_at(i);
        assert(entry);
        assert(ps5vk_texture_format_lookup(entry->format) == entry);
        assert((entry->witnessed & ~entry->capabilities) == 0);
        assert(!(entry->capabilities & PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR) ||
               (entry->capabilities & PS5VK_FORMAT_CAP_SAMPLED_IMAGE));
        assert(!(entry->witnessed & PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR) ||
               (entry->witnessed & PS5VK_FORMAT_CAP_SAMPLED_IMAGE));
        assert(!entry->capabilities || entry->provenance ||
               !(entry->capabilities & PS5VK_FORMAT_CAP_SAMPLED_IMAGE));
        for (unsigned j = i + 1; j < count; ++j)
            assert(ps5vk_texture_format_at(j)->format != entry->format);
    }
    return 0;
}
