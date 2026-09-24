#include "texture_format.h"
#include "depth_layout.h"
#include "vk_image_transfer.h"
#include "targets_ps5.h"
#include "graphics_formats.h"
#include "vk_internal.h"
#include <assert.h>

static uintptr_t image_base = UINT64_C(0x100020000);
static VkDeviceSize image_bytes = UINT64_C(65536);
VkResult ps5vk_image_span(VkDevice device, VkImage image, void **address,
    VkDeviceSize *bytes)
{
    (void)device; (void)image;
    *address = (void *)image_base;
    *bytes = image_bytes;
    return VK_SUCCESS;
}

int main(void)
{
    VkFormatProperties properties = {0};
    ps5vk_texture_format_properties(VK_FORMAT_D16_UNORM, &properties);
    assert(properties.optimalTilingFeatures ==
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    assert(!properties.linearTilingFeatures && !properties.bufferFeatures);
    assert(ps5vk_texture_format_witnessed(VK_FORMAT_D16_UNORM,
        PS5VK_FORMAT_CAP_DEPTH_STENCIL_ATTACHMENT));
    assert(ps5vk_texture_format_image_usage(VK_FORMAT_D16_UNORM,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_D16_UNORM,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    assert(!ps5vk_texture_format_image_usage(VK_FORMAT_D16_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT));
    VkImageFormatProperties image_properties;
    assert(ps5vk_graphics_image_properties(VK_FORMAT_D16_UNORM,
        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, UINT64_C(1) << 30,
        &image_properties) == VK_SUCCESS);
    assert(image_properties.maxExtent.width == 128 &&
        image_properties.maxExtent.height == 128 && image_properties.maxExtent.depth == 1 &&
        image_properties.maxMipLevels == 1 && image_properties.maxArrayLayers == 1);
    assert(ps5vk_graphics_image_properties(VK_FORMAT_D16_UNORM,
        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        0, UINT64_C(1) << 30, &image_properties) == VK_ERROR_FORMAT_NOT_SUPPORTED);

    struct ps5vk_depth_layout layout;
    assert(!ps5vk_depth16_layout(128, 128, &layout));
    assert(layout.bytes == 65536 && layout.alignment == 65536);
    assert(ps5vk_depth16_layout(256, 128, &layout) == -1);

    struct VkDevice_T device = {0};
    struct VkImage_T image = {.device=&device, .info={
        .imageType=VK_IMAGE_TYPE_2D, .format=VK_FORMAT_D16_UNORM,
        .extent={128,128,1}, .mipLevels=1, .arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT, .tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT}};
    VkMemoryRequirements requirements;
    assert(ps5vk_native_image_requirements(&device, &image.info, &requirements) == VK_SUCCESS);
    assert(requirements.size == 65536 && requirements.alignment == 65536);
    struct VkImageView_T view = {.device=&device, .image=&image,
        .format=VK_FORMAT_D16_UNORM,
        .range={VK_IMAGE_ASPECT_DEPTH_BIT, 0,1,0,1}};
    struct ps5vk_target_registers target;
    assert(ps5vk_native_target(&device, &view, NULL, &target) == VK_SUCCESS);
    assert(target.count == PS5_DEPTH_REGISTER_COUNT);
    int found_z_info = 0;
    for (uint32_t i = 0; i < target.count; ++i) {
        if (target.registers[i].offset != 0x010u) continue;
        assert((target.registers[i].value & 3u) == 1u);
        found_z_info = 1;
    }
    assert(found_z_info);
    image.info.extent.width=127;
    assert(ps5vk_native_image_requirements(&device, &image.info, &requirements) ==
        VK_ERROR_FORMAT_NOT_SUPPORTED);
    image.info.extent.width=128;
    image.info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    assert(ps5vk_native_image_requirements(&device, &image.info, &requirements) ==
        VK_ERROR_FORMAT_NOT_SUPPORTED);

    uint32_t word = 0;
    assert(ps5vk_depth_attachment_clear_word(VK_FORMAT_D16_UNORM, 0.0f, &word));
    assert(word == 0u);
    assert(ps5vk_depth_attachment_clear_word(VK_FORMAT_D16_UNORM, 1.0f, &word));
    assert(word == UINT32_MAX);
    assert(ps5vk_depth_attachment_clear_word(VK_FORMAT_D16_UNORM, 0.5f, &word));
    assert(word == UINT32_C(0x80008000));
    assert(!ps5vk_depth_attachment_clear_word(VK_FORMAT_D16_UNORM, 1.1f, &word));
    assert(ps5vk_depth_attachment_clear_word(VK_FORMAT_D32_SFLOAT, 1.0f, &word));
    assert(word == UINT32_C(0x3f800000));
    return 0;
}
