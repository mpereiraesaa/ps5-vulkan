/* The general colour readback (DXVK262-T10).
 *
 * The pinned DXVK 2.6.2 reads a render target back into a STAGING texture
 * with this postlude, measured on its first frame against a host driver and
 * shown here after the 1.0 barrier conversion:
 *
 *   BARRIER        global: COLOR_ATTACHMENT_OUTPUT/COLOR_ATTACHMENT_WRITE ->
 *                  fragment/colour stages (the attachment writes);
 *   IMAGE_BARRIER  COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC_OPTIMAL, src
 *                  access 0 (already made available above);
 *   COPY           one region: bufferOffset = the staging slice's offset in a
 *                  shared buffer, bufferRowLength 64, bufferImageHeight 64,
 *                  mip 0, layer 0, the full 64x64 extent;
 *   BARRIER        global: TRANSFER/TRANSFER_WRITE -> (...|HOST)/(...|HOST_READ);
 *   IMAGE_BARRIER  TRANSFER_SRC_OPTIMAL -> COLOR_ATTACHMENT_OPTIMAL.
 *
 * None of the strict CTS readback validators accepts it (the host
 * publication is global, not buffer-scoped, and the hand-back follows it), so
 * ps5vk_readback_regions_commands plans it and ps5vk_readback_region_detile
 * produces the bytes. Pinned here: DXVK's exact postlude at a nonzero offset,
 * a second copy in the same postlude (a sub-rectangle at another offset with a
 * wider row), each detiled texel against the 64KB_R_X source and every buffer
 * byte outside the regions untouched; and every refusal (no host
 * publication, copy before hand-over, a depth aspect, a mip level, a region
 * outside the image or the buffer, an unaligned offset, an overlap with the
 * image, too many regions, a draw in the postlude). The recorder's region
 * rule is exercised through the shared helper. */
#include "readback_commands_ps5.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { W = 64, H = 64, SOURCE = 256 * 1024, BUFFER = 64 * 1024 };
static unsigned char source[SOURCE], buffer_bytes[BUFFER];
static VkDeviceSize buffer_size = BUFFER;
VkResult ps5vk_image_span(VkDevice d, VkImage i, void **p, VkDeviceSize *n)
{ (void)d; (void)i; *p = source; *n = SOURCE; return VK_SUCCESS; }
VkResult ps5vk_buffer_span(VkDevice d, VkBuffer b, VkDeviceSize offset, VkDeviceSize size,
    void **p, VkDeviceSize *n)
{
    (void)d; (void)b; assert(!offset && size == VK_WHOLE_SIZE);
    *p = buffer_bytes; *n = buffer_size; return VK_SUCCESS;
}

static uint32_t texel(uint32_t x, uint32_t y) { return 0xff000000u | (y << 8) | x; }

static struct VkDevice_T device;
static struct VkImage_T image;
static VkBuffer staging = (VkBuffer)(uintptr_t)0x1000;

static struct ps5vk_operation global(VkPipelineStageFlags src, VkAccessFlags src_access,
    VkPipelineStageFlags dst, VkAccessFlags dst_access)
{
    return (struct ps5vk_operation){.type = PS5VK_BARRIER, .src_stage = src, .dst_stage = dst,
        .src_access = src_access, .dst_access = dst_access};
}
static struct ps5vk_operation layout(VkImageLayout from, VkImageLayout to)
{
    return (struct ps5vk_operation){.type = PS5VK_IMAGE_BARRIER,
        .src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT, .dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .image_barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = &image,
            .oldLayout = from, .newLayout = to,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}};
}
static struct ps5vk_operation copy(VkDeviceSize offset, uint32_t row, uint32_t height,
    int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    return (struct ps5vk_operation){.type = PS5VK_COPY_IMAGE_BUFFER, .copy_image = &image,
        .copy_destination = staging, .copy_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .copy_region = {.bufferOffset = offset, .bufferRowLength = row,
            .bufferImageHeight = height, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {x, y, 0}, .imageExtent = {w, h, 1}}};
}

/* DXVK's postlude, with a second copy inserted before the publication. */
enum { OPS = 6 };
static void dxvk_postlude(struct ps5vk_operation ops[OPS])
{
    ops[0] = global(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    ops[1] = layout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ops[2] = copy(16384, 64, 64, 0, 0, W, H);
    ops[3] = copy(40964, 20, 0, 5, 7, 11, 9);
    ops[4] = global(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT);
    ops[5] = layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}
static VkResult plan(const struct ps5vk_operation *ops, unsigned count,
    struct ps5vk_readback_regions *out, unsigned *site)
{
    struct ps5vk_layout_state layouts = {0};
    return ps5vk_readback_regions_commands(&device, ops, count, &layouts, out, site);
}

int main(void)
{
    image = (struct VkImage_T){.device = &device, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .info = {.format = VK_FORMAT_R8G8B8A8_UNORM, .imageType = VK_IMAGE_TYPE_2D,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT}};
    /* DXVK's render target is the colour-transfer role. */
    assert(ps5vk_readback_region_image(&image));
    /* The tiled surface: every texel at its 64KB_R_X offset. */
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const uint32_t value = texel(x, y);
            memcpy(source + ps5vk_rgba8_64k_rx_offset(x, y, W), &value, 4);
        }

    struct ps5vk_operation ops[OPS];
    dxvk_postlude(ops);
    struct ps5vk_readback_regions regions;
    unsigned site = 0;
    /* DXVK's exact one-copy postlude (without the extra copy). */
    struct ps5vk_operation one[5] = {ops[0], ops[1], ops[2], ops[4], ops[5]};
    assert(plan(one, 5, &regions, &site) == VK_SUCCESS && regions.count == 1);
    assert(regions.target[0].image == &image && regions.target[0].buffer == staging &&
           regions.target[0].region_copy && regions.target[0].region.bufferOffset == 16384 &&
           regions.target[0].layer_stride == SOURCE);
    /* The strict CTS partition does not recognise this postlude as its own. */
    {
        struct ps5vk_layout_state layouts = {0};
        struct ps5vk_readback_plan strict;
        struct ps5vk_readback_partition partition;
        const VkResult shaped = ps5vk_readback_partition(one, 5, &partition);
        assert(shaped != VK_SUCCESS ||
               ps5vk_readback_commands(&device, one + partition.readback_first,
                   partition.readback_count, NULL, &layouts, &strict, NULL) != VK_SUCCESS);
    }

    /* Two copies in one postlude, and the bytes each one produces. */
    assert(plan(ops, OPS, &regions, &site) == VK_SUCCESS && regions.count == 2);
    memset(buffer_bytes, 0xcd, sizeof(buffer_bytes));
    for (unsigned r = 0; r < regions.count; ++r)
        assert(!ps5vk_readback_region_detile(&image, regions.target[r].layer_stride,
            &regions.target[r].region, buffer_bytes, sizeof(buffer_bytes), source, SOURCE));
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            uint32_t value;
            memcpy(&value, buffer_bytes + 16384 + ((size_t)y * 64 + x) * 4, 4);
            assert(value == texel(x, y));
        }
    for (uint32_t y = 0; y < 9; ++y)
        for (uint32_t x = 0; x < 20; ++x) {
            uint32_t value;
            memcpy(&value, buffer_bytes + 40964 + ((size_t)y * 20 + x) * 4, 4);
            if (x < 11) assert(value == texel(5 + x, 7 + y));
            else assert(value == 0xcdcdcdcdu);   /* row padding is not written */
        }
    for (size_t b = 0; b < 16384; ++b) assert(buffer_bytes[b] == 0xcd);
    for (size_t b = 16384 + W * H * 4; b < 40964; ++b) assert(buffer_bytes[b] == 0xcd);
    for (size_t b = 40964 + (8 * 20 + 11) * 4; b < BUFFER; ++b) assert(buffer_bytes[b] == 0xcd);

    /* Refusals: each returns an error and leaves the caller's state alone. */
    struct ps5vk_operation bad[OPS];
#define REFUSED(n) do { \
        struct ps5vk_layout_state layouts = {0}; \
        assert(ps5vk_readback_regions_commands(&device, bad, (n), &layouts, &regions, &site) == \
               VK_ERROR_FEATURE_NOT_PRESENT && !layouts.count); } while (0)
    /* No host publication after the last copy. */
    dxvk_postlude(bad); bad[4].dst_access = VK_ACCESS_TRANSFER_READ_BIT; REFUSED(OPS);
    dxvk_postlude(bad); bad[4].src_access = 0; REFUSED(OPS);
    /* A copy before the hand-over to TRANSFER_SRC. */
    dxvk_postlude(bad); bad[1] = bad[0]; REFUSED(OPS);
    /* A depth aspect, a mip level, a region leaving the image or the buffer,
     * an unaligned offset, a row shorter than the extent. */
    dxvk_postlude(bad); bad[2].copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; REFUSED(OPS);
    dxvk_postlude(bad); bad[2].copy_region.imageSubresource.mipLevel = 1; REFUSED(OPS);
    dxvk_postlude(bad); bad[3].copy_region.imageOffset.x = 60; REFUSED(OPS);
    dxvk_postlude(bad); bad[2].copy_region.bufferOffset = BUFFER - 16; REFUSED(OPS);
    dxvk_postlude(bad); bad[2].copy_region.bufferOffset = 16386; REFUSED(OPS);
    dxvk_postlude(bad); bad[3].copy_region.bufferRowLength = 10; REFUSED(OPS);
    dxvk_postlude(bad); bad[2].copy_region.imageSubresource.layerCount = 2; REFUSED(OPS);
    /* A draw or an upload is not part of a readback postlude. */
    dxvk_postlude(bad); bad[0].type = PS5VK_DRAW; REFUSED(OPS);
    dxvk_postlude(bad); bad[3].type = PS5VK_COPY_BUFFER_IMAGE; REFUSED(OPS);
    /* A layout barrier that is neither a hand-over nor a hand-back. */
    dxvk_postlude(bad); bad[5].image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; REFUSED(OPS);
    /* A buffer-scoped member must name a buffer a copy writes. */
    dxvk_postlude(bad); bad[4].buffer_barrier.buffer = (VkBuffer)(uintptr_t)0x2000; REFUSED(OPS);
    /* More regions than a readback carries. */
    {
        struct ps5vk_operation many[PS5VK_MAX_READBACK_REGIONS + 3];
        many[0] = ops[1];
        for (unsigned k = 0; k <= PS5VK_MAX_READBACK_REGIONS; ++k)
            many[1 + k] = copy(4096u * k, 0, 0, 0, 0, 8, 8);
        many[PS5VK_MAX_READBACK_REGIONS + 2] = ops[4];
        struct ps5vk_layout_state layouts = {0};
        assert(ps5vk_readback_regions_commands(&device, many, PS5VK_MAX_READBACK_REGIONS + 3,
            &layouts, &regions, &site) == VK_ERROR_FEATURE_NOT_PRESENT);
        assert(ps5vk_readback_regions_commands(&device, many, PS5VK_MAX_READBACK_REGIONS + 2,
            &layouts, &regions, &site) == VK_ERROR_FEATURE_NOT_PRESENT);   /* unpublished */
        many[PS5VK_MAX_READBACK_REGIONS + 1] = ops[4];
        assert(ps5vk_readback_regions_commands(&device, many, PS5VK_MAX_READBACK_REGIONS + 2,
            &layouts, &regions, &site) == VK_SUCCESS && regions.count == PS5VK_MAX_READBACK_REGIONS);
    }
    /* A depth image is not a region readback image. */
    struct VkImage_T depth = image;
    depth.info.format = VK_FORMAT_D32_SFLOAT;
    depth.info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    assert(!ps5vk_readback_region_image(&depth));
    /* The shared region rule, as the recorder applies it. */
    const VkBufferImageCopy whole = ops[2].copy_region;
    assert(ps5vk_readback_region_bytes(&image, &whole) == (uint64_t)W * H * 4);
    VkBufferImageCopy tight = ops[3].copy_region;
    assert(ps5vk_readback_region_bytes(&image, &tight) == (8u * 20u + 11u) * 4u);
    tight.imageExtent.width = 0;
    assert(!ps5vk_readback_region_bytes(&image, &tight));
    /* The three image barriers DXVK's first frame records (host trace,
     * sync2 converted to 1.0 masks), admitted by the shared predicates the
     * recorder and the native prelude use. */
    {
        VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = &image,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = 0x1980u};
        assert(ps5vk_attachment_initialization_handover_barrier(&b));        /* (1) */
        b.dstAccessMask = 0x1980u | VK_ACCESS_HOST_WRITE_BIT;
        assert(!ps5vk_attachment_initialization_handover_barrier(&b));
        b = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = &image,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
        assert(ps5vk_colour_readback_dependency_barrier(&b));                /* (2) */
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        assert(!ps5vk_colour_readback_dependency_barrier(&b));
        b = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = &image,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcAccessMask = 0, .dstAccessMask = 0x1980u};
        assert(ps5vk_colour_readback_dependency_barrier(&b));                /* (3) */
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        assert(!ps5vk_colour_readback_dependency_barrier(&b));
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        assert(!ps5vk_colour_readback_dependency_barrier(&b));
    }
    puts("readback regions: pass (host tiled surface, no GPU execution)");
    return 0;
}
