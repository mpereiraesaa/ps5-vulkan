#include "texture_format.h"
#include <assert.h>

int main(void)
{
    static const VkFormat bc_formats[] = {
        VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK,
        VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
        VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK,
        VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK,
        VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK,
        VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK,
        VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC6H_SFLOAT_BLOCK,
        VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK,
    };
    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const VkImageUsageFlags sampled_upload =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags sampled_round_trip = VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    assert(sizeof(bc_formats) / sizeof(bc_formats[0]) == 16);
    for (unsigned i = 0; i < sizeof(bc_formats) / sizeof(bc_formats[0]); ++i) {
        VkFormatProperties properties = {0};
        ps5vk_texture_format_properties(bc_formats[i], &properties);
        assert((properties.optimalTilingFeatures & required) == required);
        assert(!properties.linearTilingFeatures && !properties.bufferFeatures);
        assert(ps5vk_texture_format_witnessed(bc_formats[i],
            PS5VK_FORMAT_CAP_SAMPLED_IMAGE |
            PS5VK_FORMAT_CAP_SAMPLED_IMAGE_LINEAR |
            PS5VK_FORMAT_CAP_BLIT_SRC |
            PS5VK_FORMAT_CAP_TRANSFER_SRC |
            PS5VK_FORMAT_CAP_TRANSFER_DST));
        assert(ps5vk_texture_format_image_usage(bc_formats[i], sampled_upload));
        assert(ps5vk_texture_format_image_usage(bc_formats[i], VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
        assert(ps5vk_texture_format_image_usage(bc_formats[i],
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
        assert(ps5vk_texture_format_image_usage(bc_formats[i], sampled_round_trip));
    }
    VkFormatProperties destination = {0};
    ps5vk_texture_format_properties(VK_FORMAT_R8G8B8A8_UNORM, &destination);
    assert((destination.linearTilingFeatures &
        (VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) ==
        (VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT));
    return 0;
}
