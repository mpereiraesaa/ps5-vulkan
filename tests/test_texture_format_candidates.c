#include "texture_format.h"
#include <assert.h>

int main(void)
{
    assert(ps5vk_texture_format_supported(VK_FORMAT_R8_UNORM));
    assert(ps5vk_texture_format_supported(VK_FORMAT_R8G8_UNORM));
    assert(ps5vk_texture_format_supported(VK_FORMAT_R8G8B8A8_UNORM));
    assert(ps5vk_texture_format_supported(VK_FORMAT_R8G8B8A8_SRGB));
    assert(!ps5vk_texture_format_supported(VK_FORMAT_B8G8R8A8_UNORM));
}
