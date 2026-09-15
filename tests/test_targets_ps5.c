#include "targets_ps5.h"
#include "presentation_format_ps5.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uintptr_t base = UINT64_C(0x100020000);
static VkDeviceSize span_bytes = 131072;
VkResult ps5vk_image_span(VkDevice d, VkImage image, void **address, VkDeviceSize *bytes)
{ (void)d; (void)image; *address=(void *)base; *bytes=span_bytes; return VK_SUCCESS; }
int main(void)
{
    assert(ps5vk_native_video_format(VK_FORMAT_B8G8R8A8_UNORM)==UINT64_C(0x8000000000000000));
    assert(ps5vk_native_video_format(VK_FORMAT_B8G8R8A8_UNORM)!=UINT64_C(0x8000000022000000));
    assert(!ps5vk_native_video_format(VK_FORMAT_R8G8B8A8_UNORM));
    assert(!ps5vk_native_video_format(VK_FORMAT_D32_SFLOAT));
    assert(!ps5vk_native_video_format(VK_FORMAT_UNDEFINED));
    struct VkDevice_T device = {0};
    struct VkImage_T image = {.info={.imageType=VK_IMAGE_TYPE_2D, .format=VK_FORMAT_D32_SFLOAT,
        .extent={32,32,1}, .mipLevels=1, .arrayLayers=1, .samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL, .usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT}};
    struct VkImageView_T view = {.device=&device, .image=&image, .format=VK_FORMAT_D32_SFLOAT,
        .range={VK_IMAGE_ASPECT_DEPTH_BIT, 0,1,0,1}};
    struct ps5vk_target_registers target;
    assert(ps5vk_native_target(&device, &view, NULL, &target) == VK_SUCCESS && target.count == 22);
    assert(target.registers[7].offset == 0x12 && target.registers[7].value == (base >> 8));
    base += 64;
    assert(ps5vk_native_target(&device, &view, NULL, &target) == VK_ERROR_UNKNOWN && !target.count);
    base -= 64; view.format=image.info.format=VK_FORMAT_R8G8B8A8_UNORM;
    assert(ps5vk_native_target(&device, &view, NULL, &target) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    const uint32_t offsets[16]={0x318,0x31b,0x31c,0x31d,0x31e,0x31f,0x321,0x323,
        0x324,0x325,0x390,0x398,0x3a0,0x3a8,0x3b0,0x3b8};
    ps5_agc_register defaults[16];
    for(unsigned i=0;i<16;++i)defaults[i]=(ps5_agc_register){offsets[i],0};
    view.format=image.info.format=VK_FORMAT_B8G8R8A8_UNORM;
    image.info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;view.range.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    assert(ps5vk_native_target(&device,&view,defaults,&target)==VK_SUCCESS);
    assert(target.registers[2].offset==0x31c && (target.registers[2].value & 0x1800)==0x0800);

    /* --- slice A measurement: is a chosen layer addressable at all? --------
     * The multiview question is whether the pinned target path can be pointed
     * at one layer of a multi-layer surface. These are the host half of that
     * measurement: the per-layer footprint, the fail-closed edges, and the
     * register arithmetic that must hold for base + layer * footprint to mean
     * "that layer" - with the depth and color address encodings compared so a
     * mixed surface cannot use one stride for both. */
    span_bytes = 4 * 131072;
    image.info.arrayLayers = 4;
    VkDeviceSize footprint = 0;
    assert(ps5vk_native_layer_footprint(&device, &image, &footprint) == VK_SUCCESS);
    assert(footprint == 131072);                    /* 128 KiB-aligned color layer */
    view.range.baseArrayLayer = 0;
    struct ps5vk_target_registers layer0, layer1;
    assert(ps5vk_native_layer_target(&device, &view, 0, defaults, &layer0) == VK_SUCCESS);
    assert(layer0.count == target.count && !memcmp(layer0.registers, target.registers,
        target.count * sizeof(target.registers[0])));
    assert(ps5vk_native_layer_target(&device, &view, 1, defaults, &layer1) == VK_SUCCESS);
    /* Exactly the address-carrying registers move, and by exactly one layer:
     * the color builder writes address>>8 at register 0 and (address>>40)&0xff
     * at register 2 alongside its swap bits. */
    {
        unsigned differences = 0;
        for (unsigned i = 0; i < target.count; ++i) {
            if (layer0.registers[i].value == layer1.registers[i].value) continue;
            ++differences;
            const uint32_t delta = layer1.registers[i].value - layer0.registers[i].value;
            assert(layer0.registers[i].offset == layer1.registers[i].offset);
            assert(delta == (uint32_t)(footprint >> 8));   /* no 40-bit carry here */
        }
        assert(differences == 1);
        assert(layer0.registers[0].offset == layer1.registers[0].offset);
    }
    /* A layer outside the surface, an unaligned layer stride and a surface that
     * cannot hold the layer all stay refused before registers exist. */
    assert(ps5vk_native_layer_target(&device, &view, 4, defaults, &layer1) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !layer1.count);
    span_bytes = 131072 + 64;                       /* one layer plus a fragment */
    assert(ps5vk_native_layer_target(&device, &view, 1, defaults, &layer1) ==
           VK_ERROR_UNKNOWN && !layer1.count);
    span_bytes = 4 * 131072;
    base += 8;                                      /* breaks the 128 KiB rule */
    assert(ps5vk_native_layer_target(&device, &view, 1, defaults, &layer1) ==
           VK_ERROR_UNKNOWN && !layer1.count);
    base -= 8;
    /* The depth layer uses the same base-address mechanism with a 64 KiB
     * alignment and the D32 layout's footprint, so a mixed color+depth surface
     * must take the stricter alignment and its own per-role stride. */
    image.info.format = view.format = VK_FORMAT_D32_SFLOAT;
    image.info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    view.range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    const uintptr_t color_base = base;
    assert(ps5vk_native_layer_footprint(&device, &image, &footprint) == VK_SUCCESS &&
           footprint && !(footprint % 65536));
    assert(ps5vk_native_layer_target(&device, &view, 0, defaults, &layer0) == VK_SUCCESS);
    assert(ps5vk_native_layer_target(&device, &view, 1, defaults, &layer1) == VK_SUCCESS);
    assert(layer0.count == 22 && layer1.count == layer0.count);
    assert(layer1.registers[7].offset == 0x12 &&
           layer1.registers[7].value == layer0.registers[7].value + (uint32_t)(footprint >> 8));
    base = color_base + 65536;                      /* aligned for depth, not color */
    assert(ps5vk_native_layer_target(&device, &view, 0, defaults, &layer1) == VK_SUCCESS);
    image.info.format = view.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    view.range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    assert(ps5vk_native_layer_target(&device, &view, 0, defaults, &layer1) != VK_SUCCESS);
    base = color_base;
    puts("Target address bridge: host registers only, no submission");
}
