#include "depth_layout.h"
#include "vk_internal.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    struct ps5vk_depth_layout layout;
    assert(!ps5vk_depth_layout(1920, 1080, &layout));
    assert(layout.bytes == 0x870000 && layout.pitch == 1920 && layout.padded_height == 1152);
    assert(layout.alignment == 65536 && layout.swizzle_mode == 24);
    assert(!ps5vk_depth_layout(1, 1, &layout) && layout.bytes == 65536);
    assert(!ps5vk_depth_layout(129, 128, &layout) && layout.bytes == 131072);
    assert(ps5vk_depth_layout(0, 1080, &layout) == -1 && layout.bytes == 0);
    assert(ps5vk_depth_layout(16385, 1, &layout) == -1);
    assert(!ps5vk_depth16_layout(128, 128, &layout));
    assert(layout.bytes == 65536 && layout.pitch == 128 && layout.padded_height == 128);
    assert(layout.alignment == 65536 && layout.swizzle_mode == 24);
    assert(ps5vk_depth16_layout(127, 128, &layout) == -1 && layout.bytes == 0);
    VkImageCreateInfo info = {.format=VK_FORMAT_D32_SFLOAT, .imageType=VK_IMAGE_TYPE_2D,
        .extent={1920,1080,1}, .mipLevels=1, .arrayLayers=1, .samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    VkMemoryRequirements req;
    assert(ps5vk_native_image_requirements(NULL, &info, &req) == VK_SUCCESS && req.size == 0x870000);
    info.format=VK_FORMAT_B8G8R8A8_UNORM;
    info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    assert(ps5vk_native_image_requirements(NULL, &info, &req) == VK_SUCCESS);
    assert(req.size == 0x880000 && req.alignment == 131072);
    info.extent.width=1; info.extent.height=1;
    assert(ps5vk_native_image_requirements(NULL, &info, &req) == VK_SUCCESS && req.size == 131072);
    info.format=VK_FORMAT_R16G16B16A16_SFLOAT;
    assert(ps5vk_native_image_requirements(NULL, &info, &req) == VK_ERROR_FORMAT_NOT_SUPPORTED && !req.size);
    {
        struct ps5vk_depth_stencil_layout ds;
        /* 128x128: one depth block, then one 256x256 stencil block. */
        assert(!ps5vk_depth_stencil_layout(128, 128, &ds));
        assert(ds.depth.bytes == 65536 && ds.stencil_offset == 65536 &&
               ds.stencil_bytes == 65536 && ds.bytes == 131072 && ds.alignment == 65536);
        /* Mesa AddrLib for 300x200: depth 384x256 (0x60000), stencil 512x256. */
        assert(!ps5vk_depth_stencil_layout(300, 200, &ds));
        assert(ds.depth.bytes == 0x60000 && ds.stencil_offset == 0x60000 &&
               ds.stencil_pitch == 512 && ds.stencil_bytes == 0x20000 && ds.bytes == 0x80000);
        assert(ps5vk_depth_stencil_layout(0, 1, &ds) == -1 && !ds.bytes);
        assert(ps5vk_depth_stencil_layout(1, 16385, &ds) == -1);
    }
    puts("D32 64KB_Z_X footprint: reference arithmetic only, not hardware execution");
}
