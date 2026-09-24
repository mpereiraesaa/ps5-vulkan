/*
 * Host contract tests for the Vulkan 1.0 image copy and colour clear slice.
 *
 * The advertised role is RGBA8 with transfer-only usage, backed by the padded
 * linear layout. The oracle drives the public entry points end to end (record,
 * submit, wait, compare) with guard bytes inside the row padding and outside the
 * copied region, reproduces the upstream oracle's buffer->image->copy->buffer
 * readback shape, cross-checks the module-local region planner against the
 * graphics-group planner, and asserts that the tiled colour-attachment role,
 * depth clears and in-render-pass attachment clears stay fail-closed.
 */
#include "vk_internal.h"
#include "vk_command.h"
#include "vk_framebuffer.h"
#include "vk_image_transfer.h"
#include "physical_device_profile.h"
#include "texture_copy.h"
#include "texture_layout.h"
#include "graphics_formats.h"
#include "bc_blit_decode.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = calloc(1, (size_t)size); *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static unsigned flush_calls, invalidate_calls;
static int fail_next_invalidate;
static VkResult flush_sync(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; ++flush_calls; return VK_SUCCESS; }
static VkResult invalidate_sync(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{
    (void)ctx; (void)backing; (void)offset; (void)size; ++invalidate_calls;
    if (fail_next_invalidate) { fail_next_invalidate = 0; return VK_ERROR_MEMORY_MAP_FAILED; }
    return VK_SUCCESS;
}
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, flush_sync, invalidate_sync};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }

VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
                                 .max_allocation = 1u << 20,
                                 .queue_flags = VK_QUEUE_COMPUTE_BIT};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU",
        .vendor_id = 0x1002u,
        .heap_size = 1u << 20,
        .allocation_granularity = 1,
        .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

static VkDevice device;
static VkCommandPool pool;
enum { WIDTH = 8, HEIGHT = 4 };

static VkImage make_image_subresources(VkFormat format, VkImageUsageFlags usage,
    uint32_t width, uint32_t height, uint32_t mips, uint32_t layers, VkImageTiling tiling, void **mapped)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {width, height, 1},
                              .mipLevels = mips, .arrayLayers = layers,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = tiling,
                              .usage = usage,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage image;
    assert(vkCreateImage(device, &info, NULL, &image) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = requirements.size, .memoryTypeIndex = 0u};
    VkDeviceMemory memory;
    assert(vkAllocateMemory(device, &mi, NULL, &memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, image, memory, 0) == VK_SUCCESS);
    if (mapped) assert(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS);
    return image;
}

static VkImage make_image_tiled_extent(VkFormat format, VkImageUsageFlags usage,
    uint32_t width, uint32_t height, VkImageTiling tiling, void **mapped)
{
    return make_image_subresources(format, usage, width, height, 1, 1, tiling, mapped);
}

static VkImage make_image_extent(VkFormat format, VkImageUsageFlags usage,
    uint32_t width, uint32_t height, void **mapped)
{
    return make_image_tiled_extent(format, usage, width, height,
        VK_IMAGE_TILING_OPTIMAL, mapped);
}

static VkImage make_image(VkFormat format, VkImageUsageFlags usage, void **mapped)
{
    return make_image_extent(format, usage, WIDTH, HEIGHT, mapped);
}

static VkCommandBuffer begin(void)
{
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VkCommandBuffer c;
    assert(vkAllocateCommandBuffers(device, &ai, &c) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(c, &bi) == VK_SUCCESS);
    return c;
}

static VkBuffer make_buffer(VkBufferUsageFlags usage, VkDeviceSize size, void **mapped)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer;
    assert(vkCreateBuffer(device, &info, NULL, &buffer) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = requirements.size, .memoryTypeIndex = 0u};
    VkDeviceMemory memory;
    assert(vkAllocateMemory(device, &mi, NULL, &memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);
    if (mapped) assert(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS);
    return buffer;
}

static VkImageMemoryBarrier transfer_barrier(VkImage image, VkImageLayout old_layout,
    VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access)
{
    return (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .oldLayout = old_layout, .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
}

static void submit_and_wait(VkCommandBuffer command)
{
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    assert(vkCreateFence(device, &fi, NULL, &fence) == VK_SUCCESS);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1, .pCommandBuffers = &command};
    assert(vkQueueSubmit(&device->queue, 1, &si, fence) == VK_SUCCESS);
    assert(vkWaitForFences(device, 1, &fence, VK_TRUE, 1000000000ull) == VK_SUCCESS);
    vkDestroyFence(device, fence, NULL);
}

/* BC upload/readback travels through the block-layout executor, where row
 * bytes and padded image pitch differ from RGBA8 texel rows. */
static void bc_block_transfer_round_trip(void)
{
    enum { W = 8, H = 8, BLOCK_ROWS = 2, ROW_BYTES = 16 };
    const VkFormat format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    struct ps5vk_texture_mip_layout layout;
    assert(ps5vk_texture_mip_layout_for_slices(format, W, H, 1, 1, &layout) == 0);
    assert(layout.bytes >= (VkDeviceSize)ROW_BYTES * BLOCK_ROWS);
    void *image_map = NULL, *upload_map = NULL, *readback_map = NULL;
    VkImage image = make_image_extent(format, usage, W, H, &image_map);
    VkBuffer upload = make_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        ROW_BYTES * BLOCK_ROWS, &upload_map);
    VkBuffer readback = make_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        ROW_BYTES * BLOCK_ROWS, &readback_map);
    for (uint32_t i = 0; i < ROW_BYTES * BLOCK_ROWS; ++i)
        ((uint8_t *)upload_map)[i] = (uint8_t)(i * 29u + 7u);
    memset(readback_map, 0, ROW_BYTES * BLOCK_ROWS);
    const VkBufferImageCopy region = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {W, H, 1}};
    VkCommandBuffer command = begin();
    VkImageMemoryBarrier initial = transfer_barrier(image, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &initial);
    vkCmdCopyBufferToImage(command, upload, image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier sampled = transfer_barrier(image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &sampled);
    VkImageMemoryBarrier read_source = transfer_barrier(image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &read_source);
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback, 1, &region);
    assert(command->state == PS5VK_RECORDING && command->operation_count == 5);
    assert(ps5vk_image_domain(&command->operations[1]) == PS5VK_IMAGE_DOMAIN_LINEAR &&
        ps5vk_image_linear_validate(device, &command->operations[1]) == VK_SUCCESS);
    assert(ps5vk_image_domain(&command->operations[4]) == PS5VK_IMAGE_DOMAIN_LINEAR &&
        ps5vk_image_linear_validate(device, &command->operations[4]) == VK_SUCCESS);
    submit_and_wait(command);
    assert(memcmp(upload_map, readback_map, ROW_BYTES * BLOCK_ROWS) == 0);
    for (uint32_t row = 0; row < BLOCK_ROWS; ++row) {
        const uint8_t *image_row = (const uint8_t *)image_map + row * layout.levels[0].row_pitch;
        assert(memcmp(image_row, (const uint8_t *)upload_map + row * ROW_BYTES,
            ROW_BYTES) == 0);
        for (VkDeviceSize pad = ROW_BYTES; pad < layout.levels[0].row_pitch; ++pad)
            assert(image_row[pad] == 0);
    }
    vkDestroyBuffer(device, readback, NULL);
    vkDestroyBuffer(device, upload, NULL);
    vkDestroyImage(device, image, NULL);
}

static void bc_nearest_blit_round_trip(VkFormat format, const uint8_t *block,
    uint32_t block_bytes, const uint8_t expected[4])
{
    enum { W = 8, H = 8, BLOCKS = 4 };
    const VkImageUsageFlags source_usage = VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    void *source_map = NULL, *upload_map = NULL, *destination_map = NULL;
    VkImage source = make_image_extent(format, source_usage, W, H, &source_map);
    VkImage destination = make_image_tiled_extent(VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT, W, H, VK_IMAGE_TILING_LINEAR, &destination_map);
    VkBuffer upload = make_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        BLOCKS * block_bytes, &upload_map);
    for (uint32_t i = 0; i < BLOCKS; ++i)
        memcpy((uint8_t *)upload_map + i * block_bytes, block, block_bytes);
    struct ps5vk_texture_layout destination_layout;
    assert(ps5vk_texture_layout_for_format(VK_FORMAT_R8G8B8A8_UNORM,
        W, H, &destination_layout) == VK_SUCCESS);
    memset(destination_map, 0xa5, (size_t)destination_layout.bytes);
    VkSubresourceLayout subresource = {0};
    const VkImageSubresource subresource_id = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    vkGetImageSubresourceLayout(device, destination, &subresource_id, &subresource);
    assert(subresource.rowPitch == destination_layout.row_pitch &&
        subresource.size == destination_layout.bytes);

    const VkBufferImageCopy upload_region = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {W, H, 1}};
    VkImageBlit blit = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {W, H, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{0, 0, 0}, {W, H, 1}},
    };
    VkCommandBuffer command = begin();
    VkImageMemoryBarrier source_initial = transfer_barrier(source,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &source_initial);
    vkCmdCopyBufferToImage(command, upload, source,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload_region);
    VkImageMemoryBarrier source_sampled = transfer_barrier(source,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &source_sampled);
    VkImageMemoryBarrier source_transfer = transfer_barrier(source,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &source_transfer);
    VkImageMemoryBarrier destination_initial = transfer_barrier(destination,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &destination_initial);
    vkCmdBlitImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
    assert(command->state == PS5VK_RECORDING && command->operation_count == 6);
    struct ps5vk_operation *operation = &command->operations[5];
    assert(operation->type == PS5VK_BLIT_BC_TO_RGBA8 &&
        operation->image_blit_filter == VK_FILTER_NEAREST &&
        operation->owned_payload_size == sizeof(VkImageBlit) &&
        ps5vk_image_domain(operation) == PS5VK_IMAGE_DOMAIN_LINEAR &&
        ps5vk_image_linear_validate(device, operation) == VK_SUCCESS);
    /* The operation owns the region; a later caller edit cannot narrow it. */
    blit.dstOffsets[1].x = 1;
    VkImageMemoryBarrier destination_host = transfer_barrier(destination,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1, &destination_host);
    assert(ps5vk_image_linear_validate(device, operation) == VK_SUCCESS);
    submit_and_wait(command);
    for (uint32_t y = 0; y < H; ++y) {
        const uint8_t *row = (const uint8_t *)destination_map +
            (VkDeviceSize)y * subresource.rowPitch;
        for (uint32_t x = 0; x < W; ++x)
            assert(memcmp(row + x * 4u, expected, 4) == 0);
        for (VkDeviceSize padding = W * 4u; padding < subresource.rowPitch; ++padding)
            assert(row[padding] == 0xa5);
    }
    vkDestroyBuffer(device, upload, NULL);
    vkDestroyImage(device, destination, NULL);
    vkDestroyImage(device, source, NULL);
}

/* Analytic two-block step: nearest preserves black/white; linear produces
 * 1/4 and 3/4 at the boundary when magnified two times. Padding is untouched. */
static void bc_scaled_blit(VkFilter filter, int variant, int signed_source)
{
    void *source_map = NULL, *destination_map = NULL;
    VkImage source = make_image_extent(signed_source ? VK_FORMAT_BC4_SNORM_BLOCK :
        VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT, 8, 4, &source_map);
    VkImage destination = make_image_tiled_extent(VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT, 16, 4, VK_IMAGE_TILING_LINEAR, &destination_map);
    const uint8_t blocks[16] = {0,0,0,0,0,0,0,0, 255,255,0,0,0,0,0,0};
    const uint8_t signed_blocks[16]={0x81,0x81,0,0,0,0,0,0, 0x7f,0x7f,0,0,0,0,0,0};
    memcpy(source_map, signed_source ? signed_blocks : blocks, sizeof(blocks));
    memset(destination_map, 0xa5, 4 * 256);
    source->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    destination->layout = VK_IMAGE_LAYOUT_GENERAL;
    const int mirror=variant & 1, destination_mirror=(variant >> 1) & 1;
    VkImageBlit region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .srcOffsets = {{mirror ? 8 : 0,0,0},{mirror ? 0 : 8,4,1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstOffsets = {{destination_mirror ? 16 : 0,0,0},{destination_mirror ? 0 : 16,4,1}},
    };
    VkImageBlit regions[2]={region,region};
    unsigned count=1;
    if (variant==4) {
        count=2;
        regions[0].srcOffsets[1].y=regions[0].dstOffsets[1].y=2;
        regions[1].srcOffsets[0].y=regions[1].dstOffsets[0].y=2;
    } else if (variant==5) {
        regions[0].dstOffsets[0].y=1;
        regions[0].dstOffsets[1].y=3;
    }
    VkCommandBuffer command = begin();
    vkCmdBlitImage(command,source,source->layout,destination,destination->layout,
        count,regions,filter);
    assert(command->state == PS5VK_RECORDING);
    struct ps5vk_operation invalid=command->operations[0];
    VkImageBlit bad=region;
    invalid.owned_payload=&bad;
    invalid.owned_payload_size=sizeof(bad);
    invalid.image_region_count=1;
    bad.srcOffsets[1].x=9;
    assert(ps5vk_image_linear_validate(device,&invalid)!=VK_SUCCESS);
    bad=region; bad.dstOffsets[1].y=5;
    assert(ps5vk_image_linear_validate(device,&invalid)!=VK_SUCCESS);
    bad=region; bad.srcOffsets[0].x=-1;
    assert(ps5vk_image_linear_validate(device,&invalid)!=VK_SUCCESS);
    bad=region; bad.dstOffsets[1].x=bad.dstOffsets[0].x;
    assert(ps5vk_image_linear_validate(device,&invalid)!=VK_SUCCESS);
    submit_and_wait(command);
    for (unsigned y=0;y<4;++y) {
        const uint8_t *row=(const uint8_t *)destination_map+y*256;
        for (unsigned x=0;x<16;++x) {
            if (variant==5 && (y==0 || y==3)) {
                for (unsigned c=0;c<4;++c) assert(row[x*4+c]==0xa5);
                continue;
            }
            unsigned at=(mirror != destination_mirror) ? 15-x : x;
            unsigned expected=at<8 ? 0 : 255;
            if (filter==VK_FILTER_LINEAR && at==7) expected=signed_source ? 0 : 64;
            if (filter==VK_FILTER_LINEAR && at==8) expected=signed_source ? 128 : 191;
            assert(row[x*4]==expected && row[x*4+1]==(signed_source ? 0 : expected) &&
                   row[x*4+2]==(signed_source ? 0 : expected) && row[x*4+3]==255);
        }
        for (unsigned x=64;x<256;++x) assert(row[x]==0xa5);
    }
    vkDestroyImage(device,destination,NULL);
    vkDestroyImage(device,source,NULL);
}

/* Original CTS blits use optimal images with transfer-only usage. Exercise
 * all GENERAL/TRANSFER operation layout pairs and actual buffer readback. */
static void bc_optimal_transfer_blit(VkImageLayout source_layout, VkImageLayout destination_layout, int srgb)
{
    const VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    void *src_map=NULL,*dst_map=NULL,*upload_map=NULL,*readback_map=NULL;
    VkImage source=make_image_extent(srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK,usage,4,4,&src_map);
    VkImage destination=make_image_extent(srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,usage,4,4,&dst_map);
    if (srgb) {
        destination->layout=VK_IMAGE_LAYOUT_GENERAL;
        memset(dst_map,0xa5,1024);
        VkCommandBuffer clear=begin();
        const VkClearColorValue value={.float32={0.5f,0.25f,0.0f,0.25f}};
        const VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(clear,destination,VK_IMAGE_LAYOUT_GENERAL,&value,1,&range);
        submit_and_wait(clear);
        for(unsigned y=0;y<4;++y) {
            const uint8_t *row=(const uint8_t *)dst_map+y*256;
            for(unsigned x=0;x<4;++x)
                assert(row[x*4]==188 && row[x*4+1]==137 && !row[x*4+2] && row[x*4+3]==64);
            for(unsigned x=16;x<256;++x) assert(row[x]==0xa5);
        }
    }
    VkBuffer upload=make_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT,8,&upload_map);
    VkBuffer readback=make_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT,64,&readback_map);
    const uint8_t red_block[8]={0,srgb ? 0x80 : 0xf8,0,0,0,0,0,0};
    memcpy(upload_map,red_block,8); memset(readback_map,0xa5,64);
    const VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={4,4,1}};
    const VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .srcOffsets={{0,0,0},{4,4,1}},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstOffsets={{0,0,0},{4,4,1}}};
    VkCommandBuffer command=begin();
    VkImageMemoryBarrier b=transfer_barrier(source,VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    vkCmdCopyBufferToImage(command,upload,source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    b=transfer_barrier(source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,source_layout,
        VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    b=transfer_barrier(destination,VK_IMAGE_LAYOUT_UNDEFINED,destination_layout,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    vkCmdBlitImage(command,source,source_layout,destination,destination_layout,1,&blit,VK_FILTER_LINEAR);
    assert(command->state==PS5VK_RECORDING);
    b=transfer_barrier(destination,destination_layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&b);
    vkCmdCopyImageToBuffer(command,destination,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
    submit_and_wait(command);
    for(unsigned i=0;i<16;++i) {
        const uint8_t *p=(const uint8_t *)readback_map+i*4;
        assert(p[0]==(srgb ? 132 : 255) && !p[1] && !p[2] && p[3]==255);
    }
    vkDestroyBuffer(device,readback,NULL);vkDestroyBuffer(device,upload,NULL);
    vkDestroyImage(device,destination,NULL);vkDestroyImage(device,source,NULL);
}

/* The same image has odd base dimensions, five mips, and three layers.
 * Every source subresource carries a distinct solid RGB565 endpoint. */
static void bc_source_subresources(VkFormat format, VkFilter filter)
{
    void *src_map=NULL, *dst_map=NULL;
    VkImage source=make_image_subresources(format,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        17,9,5,3,VK_IMAGE_TILING_OPTIMAL,&src_map);
    VkImage destination=make_image_tiled_extent(VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT,8,8,VK_IMAGE_TILING_LINEAR,&dst_map);
    VkImageFormatProperties query;
    assert(ps5vk_graphics_image_properties(format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,UINT64_C(268435456),&query)==VK_SUCCESS);
    assert(query.maxMipLevels>=5 && query.maxArrayLayers>=3);
    struct ps5vk_texture_mip_layout layout;
    assert(!ps5vk_texture_mip_layout_for_slices(format,17,9,3,5,&layout));
    memset(src_map,0xa5,(size_t)layout.bytes);
    const unsigned block_bytes=format==VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8 : 16;
    for(unsigned layer=0;layer<3;++layer) for(unsigned mip=0;mip<5;++mip) {
        unsigned width=17>>mip, height=9>>mip;
        if(!width)width=1;
        if(!height)height=1;
        uint8_t block[16]={0};
        if(block_bytes==16)block[0]=255;
        uint8_t *color=block+(block_bytes==16 ? 8 : 0);
        /* Red endpoint varies over all15 subresources. */
        unsigned r=1+layer*5+mip;
        color[1]=(uint8_t)(r<<3);
        for(unsigned y=0;y<(height+3)/4;++y) for(unsigned x=0;x<(width+3)/4;++x)
            memcpy((uint8_t *)src_map+layer*layout.layer_stride+layout.levels[mip].offset+
                y*layout.levels[mip].row_pitch+x*block_bytes,block,block_bytes);
    }
    uint8_t *snapshot=malloc((size_t)layout.bytes); assert(snapshot);
    memcpy(snapshot,src_map,(size_t)layout.bytes);
    source->layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    destination->layout=VK_IMAGE_LAYOUT_GENERAL;
    for(unsigned layer=0;layer<3;++layer) for(unsigned mip=0;mip<5;++mip) {
        unsigned width=17>>mip,height=9>>mip;
        if(!width)width=1;
        if(!height)height=1;
        memset(dst_map,0xa5,8*256);
        VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,layer,1},
            .srcOffsets={{0,0,0},{(int)width,(int)height,1}},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .dstOffsets={{0,0,0},{8,8,1}}};
        VkCommandBuffer command=begin();
        vkCmdBlitImage(command,source,source->layout,destination,destination->layout,1,&blit,filter);
        assert(command->state==PS5VK_RECORDING);
        submit_and_wait(command);
        unsigned red=((1+layer*5+mip)*527+23)>>6;
        for(unsigned y=0;y<8;++y) {
            const uint8_t *row=(const uint8_t *)dst_map+y*256;
            for(unsigned x=0;x<8;++x)
                assert(row[x*4]==red && row[x*4+1]==0 && row[x*4+2]==0 && row[x*4+3]==255);
            for(unsigned x=32;x<256;++x)assert(row[x]==0xa5);
        }
        assert(!memcmp(src_map,snapshot,(size_t)layout.bytes));
    }
    /* Bounds use Vulkan's floor mip extent, not padded storage dimensions.
     * Invalid indices must be rejected before a shift or an address is used. */
    for(unsigned invalid=0;invalid<6;++invalid) {
        VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,1,2,1},
            .srcOffsets={{0,0,0},{8,4,1}},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .dstOffsets={{0,0,0},{8,8,1}}};
        if(invalid==0)blit.srcSubresource.mipLevel=5;
        if(invalid==1)blit.srcSubresource.mipLevel=UINT32_MAX;
        if(invalid==2)blit.srcSubresource.baseArrayLayer=3;
        if(invalid==3)blit.srcOffsets[1].x=9;
        if(invalid==4)blit.srcOffsets[1].y=5;
        if(invalid==5)blit.srcSubresource.layerCount=2;
        VkCommandBuffer command=begin();
        vkCmdBlitImage(command,source,source->layout,destination,destination->layout,1,&blit,filter);
        assert(command->state!=PS5VK_RECORDING && command->operation_count==0);
        assert(!memcmp(src_map,snapshot,(size_t)layout.bytes));
    }
    free(snapshot);
    vkDestroyImage(device,destination,NULL); vkDestroyImage(device,source,NULL);
}

static void bc_destination_subresources(VkFormat destination_format, VkFilter filter)
{
    void *src_map=NULL,*dst_map=NULL;
    VkImage source=make_image_subresources(VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        8,8,2,3,VK_IMAGE_TILING_OPTIMAL,&src_map);
    VkImage destination=make_image_subresources(destination_format,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        17,9,3,4,VK_IMAGE_TILING_OPTIMAL,&dst_map);
    struct ps5vk_texture_mip_layout src,dst;
    assert(!ps5vk_texture_mip_layout_for_slices(source->info.format,8,8,3,2,&src));
    assert(!ps5vk_texture_mip_layout_for_slices(destination_format,17,9,4,3,&dst));
    memset(src_map,0xa5,(size_t)src.bytes);
    for(unsigned layer=0;layer<3;++layer)for(unsigned mip=0;mip<2;++mip) {
        uint8_t block[8]={31,0,0,0,0,0,0,0}; /* blue in unselected mip/layer */
        if(mip && layer==1){block[0]=0;block[1]=0xf8;}
        if(mip && layer==2){block[0]=0xe0;block[1]=7;}
        unsigned blocks=mip ? 1 : 2;
        for(unsigned y=0;y<blocks;++y)for(unsigned x=0;x<blocks;++x)
            memcpy((uint8_t *)src_map+layer*src.layer_stride+src.levels[mip].offset+
                y*src.levels[mip].row_pitch+x*8,block,8);
    }
    uint8_t *expected=malloc((size_t)dst.bytes); assert(expected);
    source->layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    destination->layout=VK_IMAGE_LAYOUT_GENERAL;
    for(unsigned mip=0;mip<3;++mip) {
        unsigned width=17>>mip,height=9>>mip;
        memset(dst_map,0xa5,(size_t)dst.bytes);memset(expected,0xa5,(size_t)dst.bytes);
        VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,1,1,2},
            .srcOffsets={{0,0,0},{4,4,1}},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,1,2},
            .dstOffsets={{1,0,0},{(int)width,(int)height,1}}};
        VkCommandBuffer command=begin();
        vkCmdBlitImage(command,source,source->layout,destination,destination->layout,1,&blit,filter);
        assert(command->state==PS5VK_RECORDING);
        submit_and_wait(command);
        for(unsigned layer=1;layer<3;++layer)for(unsigned y=0;y<height;++y)
            for(unsigned x=1;x<width;++x) {
                uint8_t *pixel=expected+layer*dst.layer_stride+dst.levels[mip].offset+
                    y*dst.levels[mip].row_pitch+x*4;
                pixel[0]=layer==1 ? 255 : 0;pixel[1]=layer==2 ? 255 : 0;
                pixel[2]=0;pixel[3]=255;
            }
        assert(!memcmp(dst_map,expected,(size_t)dst.bytes));
    }
    for(unsigned invalid=0;invalid<10;++invalid) {
        VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,1,1,2},
            .srcOffsets={{0,0,0},{4,4,1}},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,2,1,2},
            .dstOffsets={{0,0,0},{4,2,1}}};
        if(invalid==0)blit.dstSubresource.mipLevel=3;
        if(invalid==1)blit.dstSubresource.mipLevel=UINT32_MAX;
        if(invalid==2)blit.dstSubresource.baseArrayLayer=4;
        if(invalid==3)blit.dstSubresource.baseArrayLayer=UINT32_MAX;
        if(invalid==4)blit.dstSubresource.baseArrayLayer=3; /* two layers overrun */
        if(invalid==5)blit.srcSubresource.layerCount=blit.dstSubresource.layerCount=UINT32_MAX;
        if(invalid==6)blit.srcSubresource.layerCount=blit.dstSubresource.layerCount=0;
        if(invalid==7)blit.dstSubresource.layerCount=1;
        if(invalid==8)blit.dstOffsets[1].x=5; /* padded ceil width is not Vulkan width */
        if(invalid==9)blit.dstOffsets[1].y=3;
        VkCommandBuffer command=begin();
        vkCmdBlitImage(command,source,source->layout,destination,destination->layout,1,&blit,filter);
        assert(command->state!=PS5VK_RECORDING && command->operation_count==0);
        assert(!memcmp(dst_map,expected,(size_t)dst.bytes));
    }
    free(expected);
    vkDestroyImage(device,destination,NULL);vkDestroyImage(device,source,NULL);
}

static void bc_all_formats_blit(void)
{
    const uint8_t zero[16]={0};
    const uint8_t bc7_zero[16]={0x40}; /* mode 6, zero endpoints and indices */
    const uint8_t opaque[4]={0,0,0,255}, transparent[4]={0,0,0,0};
    for (VkFormat format=VK_FORMAT_BC1_RGB_UNORM_BLOCK;
         format<=VK_FORMAT_BC7_SRGB_BLOCK; ++format) {
        int alpha=(format==VK_FORMAT_BC2_UNORM_BLOCK || format==VK_FORMAT_BC2_SRGB_BLOCK ||
                   format==VK_FORMAT_BC3_UNORM_BLOCK || format==VK_FORMAT_BC3_SRGB_BLOCK ||
                   format==VK_FORMAT_BC7_UNORM_BLOCK || format==VK_FORMAT_BC7_SRGB_BLOCK);
        int short_block=(format<=VK_FORMAT_BC1_RGBA_SRGB_BLOCK ||
                         format==VK_FORMAT_BC4_UNORM_BLOCK || format==VK_FORMAT_BC4_SNORM_BLOCK);
        bc_nearest_blit_round_trip(format,
            format>=VK_FORMAT_BC7_UNORM_BLOCK ? bc7_zero : zero,
            short_block ? 8 : 16, alpha ? transparent : opaque);
    }
}

static void bda_storage_image_trace(void)
{
    enum { DIM=8 };
    const VkImageUsageFlags usage=VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkFormatProperties format={0};
    ps5vk_graphics_format_properties(VK_FORMAT_R32_UINT,&format);
    assert(format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    VkImageFormatProperties properties={0};
    assert(ps5vk_graphics_image_properties(VK_FORMAT_R32_UINT,VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,usage,0,1u<<20,&properties)==VK_SUCCESS);
    assert(properties.maxExtent.width==8 && properties.maxExtent.height==8 &&
        properties.maxMipLevels==1 && properties.maxArrayLayers==1);
    VkImageCreateInfo info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R32_UINT,
        .extent={DIM,DIM,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkImage image=VK_NULL_HANDLE;
    info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    assert(vkCreateImage(device,&info,NULL,&image)==VK_ERROR_FORMAT_NOT_SUPPORTED && !image);
    info.usage=usage;
    info.extent.width=DIM+1;
    assert(vkCreateImage(device,&info,NULL,&image)==VK_ERROR_FORMAT_NOT_SUPPORTED && !image);
    info.extent.width=DIM;
    assert(vkCreateImage(device,&info,NULL,&image)==VK_SUCCESS);
    VkMemoryRequirements requirements={0};
    vkGetImageMemoryRequirements(device,image,&requirements);
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=requirements.size,.memoryTypeIndex=0};
    VkDeviceMemory memory=VK_NULL_HANDLE;
    assert(vkAllocateMemory(device,&allocation,NULL,&memory)==VK_SUCCESS);
    assert(vkBindImageMemory(device,image,memory,0)==VK_SUCCESS);
    void *mapped=NULL;
    assert(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS);
    VkImageViewCreateInfo view_info={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R32_UINT,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView view=VK_NULL_HANDLE;
    assert(vkCreateImageView(device,&view_info,NULL,&view)==VK_SUCCESS);
    void *buffer_mapped=NULL;
    VkBuffer readback=make_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT,DIM*DIM*4,
        &buffer_mapped);
    VkCommandBuffer command=begin();
    VkImageMemoryBarrier initial=transfer_barrier(image,VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&initial);
    VkClearColorValue clear={.uint32={0,0,0,0}};
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(command,image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
    VkMemoryBarrier to_shader={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&to_shader,0,NULL,0,NULL);
    VkMemoryBarrier to_transfer={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&to_transfer,0,NULL,0,NULL);
    VkBufferImageCopy region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={DIM,DIM,1}};
    vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_GENERAL,readback,1,&region);
    assert(vkEndCommandBuffer(command)==VK_SUCCESS);
    assert(command->operation_count==5 &&
        command->operations[0].type==PS5VK_IMAGE_BARRIER &&
        command->operations[1].type==PS5VK_CLEAR_COLOR_IMAGE &&
        command->operations[2].type==PS5VK_BARRIER &&
        command->operations[3].type==PS5VK_BARRIER &&
        command->operations[4].type==PS5VK_COPY_IMAGE_BUFFER);
    assert(ps5vk_image_linear_execute(device,&command->operations[0])==VK_SUCCESS);
    assert(ps5vk_image_transfer_execute(device,&command->operations[1])==VK_SUCCESS);
    for(uint32_t y=0;y<DIM;++y)
        for(uint32_t x=0;x<DIM;++x)
            ((uint32_t *)((unsigned char *)mapped+y*256))[x]=1;
    assert(ps5vk_image_linear_execute(device,&command->operations[4])==VK_SUCCESS);
    for(uint32_t i=0;i<DIM*DIM;++i)assert(((uint32_t *)buffer_mapped)[i]==1);
    vkDestroyBuffer(device,readback,NULL);
    vkDestroyImageView(device,view,NULL);
    vkDestroyImage(device,image,NULL);
}

/* The exact input-attachment resource the pinned multiview helper needs: 2D
 * R8G8B8A8_UNORM, optimal tiling, flags 0, extent depth 1, one mip, one sample,
 * arrayLayers 1..6, usage COLOR_ATTACHMENT|TRANSFER_SRC|INPUT_ATTACHMENT|
 * TRANSFER_DST. Creation uses the REAL native requirement arithmetic here, so
 * the layer accounting and its overflow guard are exercised too. */
static void input_attachment_shape(void)
{
    const VkImageUsageFlags exact = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageCreateInfo base = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D, .format=VK_FORMAT_R8G8B8A8_UNORM,
        .extent={WIDTH, HEIGHT, 1u}, .mipLevels=1u, .arrayLayers=1u,
        .samples=VK_SAMPLE_COUNT_1_BIT, .tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=exact, .sharingMode=VK_SHARING_MODE_EXCLUSIVE};

    /* One layer and the six-layer boundary both create, and the requirement
     * arithmetic really covers every layer: six layers are six strides. */
    VkDeviceSize single_stride = 0, single_alignment = 0, single_bytes = 0;
    assert(ps5vk_native_layered_storage(VK_FORMAT_R8G8B8A8_UNORM, WIDTH, HEIGHT, 1u,
                                        &single_stride, &single_alignment, &single_bytes) == VK_SUCCESS);
    for (uint32_t layers = 1u; layers <= (uint32_t)PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR; ++layers) {
        VkImageCreateInfo info = base; info.arrayLayers = layers;
        VkImage image = VK_NULL_HANDLE;
        assert(vkCreateImage(device, &info, NULL, &image) == VK_SUCCESS && image);
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, image, &requirements);
        assert(requirements.size == single_bytes * layers &&
               requirements.alignment == single_alignment && requirements.memoryTypeBits == 1u);
        vkDestroyImage(device, image, NULL);
    }
    /* ...and the arithmetic refuses a count that would wrap rather than
     * reporting a small size for it. */
    VkDeviceSize stride = 0, alignment = 0, bytes = 0;
    assert(ps5vk_native_layered_storage(VK_FORMAT_R8G8B8A8_UNORM, WIDTH, HEIGHT, UINT64_MAX,
                                        &stride, &alignment, &bytes) != VK_SUCCESS);

    /* Deeper than the measured floor is refused: the query reports six, so
     * creation may not accept seven. */
    VkImageCreateInfo info = base;
    info.arrayLayers = (uint32_t)PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR + 1u;
    VkImage image = (VkImage)(uintptr_t)1;
    unsigned before = device->graphics_objects;
    assert(vkCreateImage(device, &info, NULL, &image) == VK_ERROR_FEATURE_NOT_PRESENT &&
           image == VK_NULL_HANDLE && device->graphics_objects == before);

    /* Every neighbouring usage fails closed with no partial publication: a
     * missing role, an extra role, and a role set the query does not answer. */
    /* The same shape WITHOUT the input-attachment role is the draw colour
     * target this profile already supported, so it keeps working unchanged -
     * the new role did not redefine it. */
    info = base; info.usage = exact & ~(VkImageUsageFlags)VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    info.arrayLayers = 1u;
    image = VK_NULL_HANDLE;
    assert(vkCreateImage(device, &info, NULL, &image) == VK_SUCCESS && image);
    vkDestroyImage(device, image, NULL);
    const VkImageUsageFlags neighbours[] = {
        exact & ~(VkImageUsageFlags)VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        exact & ~(VkImageUsageFlags)VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        exact | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    };
    for (unsigned i = 0; i < sizeof(neighbours)/sizeof(neighbours[0]); ++i) {
        info = base; info.usage = neighbours[i]; info.arrayLayers = 2u;
        image = (VkImage)(uintptr_t)1;
        before = device->graphics_objects;
        VkResult rc = vkCreateImage(device, &info, NULL, &image);
        assert(rc != VK_SUCCESS && image == VK_NULL_HANDLE &&
               device->graphics_objects == before);
    }
    /* ...and the same usage on another format is refused as well: the shape is
     * exactly one colour format, not an input-attachment capability. */
    const VkFormat other_formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
                                      VK_FORMAT_R8G8B8A8_UINT};
    for (unsigned i = 0; i < sizeof(other_formats)/sizeof(other_formats[0]); ++i) {
        info = base; info.format = other_formats[i]; info.arrayLayers = 2u;
        image = (VkImage)(uintptr_t)1;
        before = device->graphics_objects;
        assert(vkCreateImage(device, &info, NULL, &image) != VK_SUCCESS &&
               image == VK_NULL_HANDLE && device->graphics_objects == before);
    }

    /* Reproduce the native probe's second submission end to end.  The source
     * is the exact six-layer input-attachment backing after its render pass;
     * the destination is the one linear staging role.  This pins queue-time
     * classification as well as record-time acceptance. */
    info = base; info.arrayLayers = 6u;
    VkImage target = VK_NULL_HANDLE;
    assert(vkCreateImage(device, &info, NULL, &target) == VK_SUCCESS);
    VkMemoryRequirements target_requirements;
    vkGetImageMemoryRequirements(device, target, &target_requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = target_requirements.size, .memoryTypeIndex = 0};
    VkDeviceMemory target_memory = VK_NULL_HANDLE;
    assert(vkAllocateMemory(device, &allocation, NULL, &target_memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, target, target_memory, 0) == VK_SUCCESS);
    target->layout = VK_IMAGE_LAYOUT_GENERAL;

    VkImageCreateInfo staging_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {WIDTH, HEIGHT, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage readback = VK_NULL_HANDLE;
    assert(vkCreateImage(device, &staging_info, NULL, &readback) == VK_SUCCESS);
    VkMemoryRequirements readback_requirements;
    vkGetImageMemoryRequirements(device, readback, &readback_requirements);
    allocation.allocationSize = readback_requirements.size;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    assert(vkAllocateMemory(device, &allocation, NULL, &readback_memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, readback, readback_memory, 0) == VK_SUCCESS);

    VkCommandPool saved_pool = pool;
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    assert(vkCreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
    VkCommandBuffer command = begin();
    VkImageMemoryBarrier staging_in = transfer_barrier(readback,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &staging_in);
    VkImageCopy whole = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {WIDTH, HEIGHT, 1}};
    vkCmdCopyImage(command, target, VK_IMAGE_LAYOUT_GENERAL, readback,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &whole);
    VkImageMemoryBarrier staging_out = transfer_barrier(readback,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1, &staging_out);
    assert(command->state == PS5VK_RECORDING);
    submit_and_wait(command);
    assert(readback->layout == VK_IMAGE_LAYOUT_GENERAL);
    vkDestroyImage(device, readback, NULL);
    vkFreeMemory(device, readback_memory, NULL);
    vkDestroyImage(device, target, NULL);
    vkFreeMemory(device, target_memory, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    pool = saved_pool;
}

static void readback_return_recording(void)
{
    VkImage image=make_image(VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,NULL);
    VkDeviceMemory memory=image->memory;
    VkImageMemoryBarrier barrier=transfer_barrier(image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    for(unsigned invalid_scope=0;invalid_scope<2;++invalid_scope) {
        VkCommandBuffer c=begin();
        vkCmdPipelineBarrier(c,invalid_scope?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:
            (VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT),
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&barrier);
        assert(vkEndCommandBuffer(c)==(invalid_scope?VK_ERROR_UNKNOWN:VK_SUCCESS));
        if(!invalid_scope)assert(c->operation_count==1 &&
            c->operations[0].type==PS5VK_IMAGE_BARRIER);
        vkFreeCommandBuffers(device,pool,1,&c);
    }
    vkDestroyImage(device,image,NULL);
    vkFreeMemory(device,memory,NULL);
}

int main(void)
{
    VkInstance instance;
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1;
    VkPhysicalDevice physical;
    assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    assert(vkCreateDevice(physical, &dci, NULL, &device) == VK_SUCCESS);
    device->graphics_enabled = VK_TRUE; /* the native platform installs these during configure */
    device->image_requirements = ps5vk_native_image_requirements;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    assert(vkCreateCommandPool(device, &pci, NULL, &pool) == VK_SUCCESS);
    for(int srgb=0;srgb<2;++srgb)
    for(unsigned general_source=0;general_source<2;++general_source)
        for(unsigned general_destination=0;general_destination<2;++general_destination)
            bc_optimal_transfer_blit(general_source ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                general_destination ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, srgb);
    bc_source_subresources(VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FILTER_NEAREST);
    bc_source_subresources(VK_FORMAT_BC3_UNORM_BLOCK,VK_FILTER_LINEAR);
    bc_destination_subresources(VK_FORMAT_R8G8B8A8_UNORM,VK_FILTER_NEAREST);
    bc_destination_subresources(VK_FORMAT_R8G8B8A8_SRGB,VK_FILTER_LINEAR);
    bc_all_formats_blit();
    for (int variant=0;variant<6;++variant) {
        bc_scaled_blit(VK_FILTER_NEAREST,variant,0);
        bc_scaled_blit(VK_FILTER_LINEAR,variant,0);
        bc_scaled_blit(VK_FILTER_LINEAR,variant,1);
    }
    /* Mid-gray sRGB must retain precision before filtering (132/255). */
    assert(fabsf(ps5vk_bc_blit_srgb_to_linear(132)-0.23074005f)<0.000001f);
    bda_storage_image_trace();
    readback_return_recording();
    bc_block_transfer_round_trip();
    const uint8_t bc1_red[8] = {0x00, 0xf8, 0x00, 0x00, 0, 0, 0, 0};
    const uint8_t bc3_green[16] = {
        0xff, 0xff, 0, 0, 0, 0, 0, 0,
        0xe0, 0x07, 0x00, 0x00, 0, 0, 0, 0};
    const uint8_t red[4] = {255, 0, 0, 255};
    const uint8_t green[4] = {0, 255, 0, 255};
    bc_nearest_blit_round_trip(VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        bc1_red, sizeof(bc1_red), red);
    bc_nearest_blit_round_trip(VK_FORMAT_BC3_UNORM_BLOCK,
        bc3_green, sizeof(bc3_green), green);

    const VkImageUsageFlags transfer_usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    void *source_map = NULL, *destination_map = NULL;
    VkImage source = make_image(VK_FORMAT_R8G8B8A8_UNORM, transfer_usage, &source_map);
    VkImage destination = make_image(VK_FORMAT_R8G8B8A8_UNORM, transfer_usage, &destination_map);
    struct ps5vk_texture_layout layout;
    assert(ps5vk_texture_layout(WIDTH, HEIGHT, &layout) == 0);
    /* the module-local transfer layout must equal the shared texture layout */
    assert(layout.row_pitch == (((uint32_t)WIDTH * 4u + 255u) & ~255u));
    memset(source_map, 0, layout.bytes);
    memset(destination_map, 0xa5, layout.bytes); /* guards in padding and outside copies */

    /* --- colour clear then image copy, in one submission --- */
    VkClearColorValue clear = {.float32 = {0.25f, 0.5f, 0.75f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageCopy region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {1, 1, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {2, 2, 0},
        .extent = {3, 2, 1},
    };
    /* Both images start discarded; the clear and the copy then require the
     * layouts the caller declares, exactly as the upstream oracle does. */
    VkCommandBuffer command = begin();
    VkImageMemoryBarrier to_destination[] = {
        transfer_barrier(source, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         0, VK_ACCESS_TRANSFER_WRITE_BIT),
        transfer_barrier(destination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         0, VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 2, to_destination);
    vkCmdClearColorImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    VkImageMemoryBarrier to_source = transfer_barrier(source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_source);
    vkCmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    assert(command->state == PS5VK_RECORDING && command->operation_count == 5);
    submit_and_wait(command);
    assert(source->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(destination->layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    const uint8_t *source_bytes = source_map;
    const uint8_t *destination_bytes = destination_map;
    const uint8_t expected[4] = {64, 128, 191, 255}; /* 0.25/0.5/0.75/1.0 as RGBA8 */
    for (unsigned y = 0; y < HEIGHT; ++y) {
        for (unsigned x = 0; x < WIDTH; ++x) {
            assert(memcmp(source_bytes + (size_t)y * layout.row_pitch + x * 4u, expected, 4) == 0);
        }
        /* The row padding after the pixels stays untouched by the clear. */
        for (unsigned p = WIDTH * 4u; p < layout.row_pitch; ++p)
            assert(source_bytes[(size_t)y * layout.row_pitch + p] == 0);
    }
    for (unsigned y = 0; y < HEIGHT; ++y) {
        for (unsigned x = 0; x < WIDTH; ++x) {
            const uint8_t *pixel = destination_bytes + (size_t)y * layout.row_pitch + x * 4u;
            const int copied = x >= 2 && x < 5 && y >= 2 && y < 4;
            if (copied) assert(memcmp(pixel, expected, 4) == 0);
            else assert(pixel[0] == 0xa5 && pixel[1] == 0xa5 && pixel[2] == 0xa5 && pixel[3] == 0xa5);
        }
    }

    /* --- validation is transactional --- */
    VkCommandBuffer bad;
    /* --- upstream oracle shape: buffer -> image -> copy -> buffer --- */
    const VkDeviceSize pixels = (VkDeviceSize)WIDTH * HEIGHT * 4u;
    void *upload_map = NULL, *readback_map = NULL;
    VkBuffer upload = make_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pixels, &upload_map);
    VkBuffer readback = make_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT, pixels, &readback_map);
    for (VkDeviceSize j = 0; j < pixels; ++j)
        ((uint8_t *)upload_map)[j] = (uint8_t)(j * 7u + 3u);
    memset(readback_map, 0, (size_t)pixels);
    /* Tight row description, exactly what the pinned upstream case records. */
    VkBufferImageCopy whole = {
        .bufferOffset = 0, .bufferRowLength = WIDTH, .bufferImageHeight = HEIGHT,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0}, .imageExtent = {WIDTH, HEIGHT, 1}};
    command = begin();
    vkCmdCopyBufferToImage(command, upload, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &whole);
    VkImageMemoryBarrier readback_barrier = transfer_barrier(destination,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &readback_barrier);
    vkCmdCopyImageToBuffer(command, destination, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &whole);
    assert(command->state == PS5VK_RECORDING && command->operation_count == 3);
    submit_and_wait(command);
    assert(memcmp(readback_map, upload_map, (size_t)pixels) == 0);
    assert(invalidate_calls >= 2 && flush_calls >= 3);
    {
        /* The upload writes only the described pixels, never the row padding. */
        const uint8_t *image = destination_map;
        for (unsigned y = 0; y < HEIGHT; ++y)
            for (unsigned p = WIDTH * 4u; p < layout.row_pitch; ++p)
                assert(image[(size_t)y * layout.row_pitch + p] == 0xa5);
    }

    /* --- the module-local region planner agrees with the shared planner --- */
    {
        const uint32_t row_lengths[] = {0u, WIDTH, WIDTH + 1u};
        const int32_t offsets[] = {0, 1, 2};
        const uint32_t widths[] = {1u, 3u, WIDTH};
        const uint32_t heights[] = {1u, 2u, HEIGHT};
        unsigned agreements = 0;
        for (unsigned a = 0; a < 3; ++a)
            for (unsigned b = 0; b < 3; ++b)
                for (unsigned c = 0; c < 3; ++c)
                    for (unsigned d = 0; d < 3; ++d) {
                        VkBufferImageCopy probe = {
                            .bufferOffset = (VkDeviceSize)(a == 2 ? 4u : 0u),
                            .bufferRowLength = row_lengths[a],
                            .bufferImageHeight = heights[b],
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageOffset = {offsets[b], offsets[c], 0},
                            .imageExtent = {widths[c], heights[d], 1}};
                        struct ps5vk_texture_copy shared;
                        VkResult shared_result = ps5vk_texture_copy_plan(WIDTH, HEIGHT,
                            pixels, layout.bytes, &probe, &shared);
                        VkResult local_result = ps5vk_image_linear_region_validate(destination,
                            &probe, pixels, layout.bytes);
                        assert((shared_result == VK_SUCCESS) == (local_result == VK_SUCCESS));
                        ++agreements;
                    }
        assert(agreements == 81u);
    }

    /* --- linear transfer validation is fail-closed --- */
    VkBuffer wrong_usage = make_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT, pixels, NULL);
    VkBuffer wrong_destination = make_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pixels, NULL);
    bad = begin();
    vkCmdCopyBufferToImage(bad, wrong_usage, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &whole);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    bad = begin();
    vkCmdCopyImageToBuffer(bad, destination, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wrong_destination, 1, &whole);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    bad = begin();
    {
        VkBufferImageCopy misaligned = whole;
        misaligned.bufferOffset = 1;
        vkCmdCopyBufferToImage(bad, upload, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &misaligned);
    }
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    bad = begin();
    {
        VkBufferImageCopy oversize = whole;
        oversize.imageExtent.width = WIDTH;
        oversize.imageOffset.x = 1;
        vkCmdCopyImageToBuffer(bad, destination, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &oversize);
    }
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    /* A render-target image cannot be transitioned by a transfer-only barrier. */
    bad = begin();
    {
        VkImageMemoryBarrier illegal = transfer_barrier(destination,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdPipelineBarrier(bad, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, NULL, 0, NULL, 1, &illegal);
    }
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);

    bad = begin();
    vkCmdCopyImage(bad, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, &region);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    bad = begin();
    vkCmdCopyImage(bad, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    bad = begin();
    {
        VkImageCopy oversize = region;
        oversize.extent.width = WIDTH + 1u; /* catches unsigned bound underflow */
        oversize.srcOffset.x = 0;
        vkCmdCopyImage(bad, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &oversize);
    }
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    bad = begin();
    vkCmdClearColorImage(bad, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &(VkClearColorValue){.float32 = {2.0f, 0.0f, 0.0f, 1.0f}}, 1, &range);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    bad = begin();
    vkCmdClearColorImage(bad, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, NULL);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);

    /* Submit/head validation owns and revalidates the clear range payload. */
    bad = begin();
    vkCmdClearColorImage(bad, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear, 1, &range);
    assert(bad->state == PS5VK_RECORDING && bad->operation_count == 1);
    bad->operations[0].owned_payload_size--;
    assert(ps5vk_image_transfer_validate(device, &bad->operations[0]) != VK_SUCCESS);

    /* Failed invalidation happens before any CPU store. */
    destination->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    uint8_t *snapshot = malloc((size_t)layout.bytes);
    assert(snapshot);
    memcpy(snapshot, destination_map, (size_t)layout.bytes);
    bad = begin();
    vkCmdCopyBufferToImage(bad, upload, destination,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &whole);
    assert(bad->state == PS5VK_RECORDING && bad->operation_count == 1);
    fail_next_invalidate = 1;
    assert(ps5vk_image_linear_execute(device, &bad->operations[0]) == VK_ERROR_DEVICE_LOST);
    assert(memcmp(snapshot, destination_map, (size_t)layout.bytes) == 0);

    destination->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    memcpy(snapshot, readback_map, (size_t)pixels);
    bad = begin();
    vkCmdCopyImageToBuffer(bad, destination, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback, 1, &whole);
    assert(bad->state == PS5VK_RECORDING && bad->operation_count == 1);
    fail_next_invalidate = 1;
    assert(ps5vk_image_linear_execute(device, &bad->operations[0]) == VK_ERROR_DEVICE_LOST);
    assert(memcmp(snapshot, readback_map, (size_t)pixels) == 0);
    free(snapshot);

    /* Distinct resource handles backed by overlapping memory fail closed. */
    VkImage alias_source = make_image(VK_FORMAT_R8G8B8A8_UNORM, transfer_usage, NULL);
    VkImage alias_destination = make_image(VK_FORMAT_R8G8B8A8_UNORM, transfer_usage, NULL);
    alias_destination->memory = alias_source->memory;
    alias_destination->offset = alias_source->offset;
    bad = begin();
    vkCmdCopyImage(bad, alias_source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   alias_destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);

    VkBufferCreateInfo alias_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = pixels, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer alias_buffer;
    assert(vkCreateBuffer(device, &alias_info, NULL, &alias_buffer) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, alias_buffer, alias_source->memory, 0) == VK_SUCCESS);
    bad = begin();
    vkCmdCopyBufferToImage(bad, alias_buffer, alias_source,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &whole);
    assert(bad->state == PS5VK_RECORDING && bad->operation_count == 1);
    assert(ps5vk_image_linear_validate(device, &bad->operations[0]) != VK_SUCCESS);

    bad = begin();
    vkCmdBlitImage(bad, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, NULL, VK_FILTER_NEAREST);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    bad = begin();
    vkCmdResolveImage(bad, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, NULL);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);

    /* the tiled colour-attachment role is refused rather than cleared linearly */
    VkImage attachment = make_image(VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, NULL);
    bad = begin();
    vkCmdClearColorImage(bad, attachment, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);

    /* The one colour-attachment shape that also declares a transfer destination
     * is the pinned upstream CTS draw target, and it takes the same padded
     * linear clear and upload as the transfer role. Only that exact shape: the
     * predicates below keep every neighbouring shape out. */
    VkImage colour_dst = make_image(VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    NULL);
    assert(ps5vk_colour_transfer_image(colour_dst));
    assert(!ps5vk_colour_transfer_image(attachment));
    VkImage transfer_only = make_image(VK_FORMAT_R8G8B8A8_UNORM,
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       NULL);
    assert(!ps5vk_colour_transfer_image(transfer_only));
    assert(ps5vk_pure_transfer_image(transfer_only));
    /* The linear readback path additionally admits only the promoted six-layer
     * input-attachment backing.  It does not become a clear/upload colour role,
     * and every neighbouring mutation stays outside the readback predicate. */
    struct VkImage_T input_readback = {0};
    input_readback.info = (VkImageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {WIDTH, HEIGHT, 1}, .mipLevels = 1, .arrayLayers = 6,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    assert(ps5vk_input_attachment_readback_image(&input_readback));
    assert(ps5vk_colour_readback_image(&input_readback));
    assert(!ps5vk_colour_transfer_image(&input_readback));
    input_readback.info.arrayLayers = 5;
    assert(!ps5vk_input_attachment_readback_image(&input_readback));
    input_readback.info.arrayLayers = 6;
    input_readback.info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    assert(!ps5vk_input_attachment_readback_image(&input_readback));
    VkCommandBuffer colour_clear = begin();
    vkCmdClearColorImage(colour_clear, colour_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear, 1, &range);
    assert(colour_clear->state == PS5VK_RECORDING && colour_clear->operation_count == 1);
    VkBuffer colour_upload = make_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         WIDTH * HEIGHT * 4, NULL);
    VkBufferImageCopy colour_region = {
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0}, .imageExtent = {WIDTH, HEIGHT, 1}};
    VkCommandBuffer colour_upload_cmd = begin();
    vkCmdCopyBufferToImage(colour_upload_cmd, colour_upload, colour_dst,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &colour_region);
    assert(colour_upload_cmd->state == PS5VK_RECORDING &&
           colour_upload_cmd->operation_count == 1);
    /* The readback role of the attachment shape is unchanged, and the transfer
     * role still refuses the attachment geometry it never had. */
    VkCommandBuffer colour_clear_bad = begin();
    vkCmdClearColorImage(colour_clear_bad, colour_dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         &clear, 1, &range);
    assert(colour_clear_bad->state == PS5VK_INVALID && colour_clear_bad->operation_count == 0);

    /* --- whole-subresource depth clear --------------------------------------
     * A depth target that carries the transfer-destination usage records a real
     * operation; one that does not stays fail-closed, because Vulkan requires
     * that usage on the cleared image. */
    VkImage depth = make_image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, NULL);
    bad = begin();
    vkCmdClearDepthStencilImage(bad, depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &(VkClearDepthStencilValue){.depth = 1.0f, .stencil = 0}, 1,
                                &(VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1});
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);

    VkImage depth_dst = make_image(VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, NULL);
    const VkImageSubresourceRange whole_depth = {VK_IMAGE_ASPECT_DEPTH_BIT, 0,
        VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkCommandBuffer cleared = begin();
    vkCmdClearDepthStencilImage(cleared, depth_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &(VkClearDepthStencilValue){.depth = 1.0f, .stencil = 0}, 1,
                                &whole_depth);
    assert(cleared->state == PS5VK_RECORDING && cleared->operation_count == 1);
    assert(cleared->operations[0].type == PS5VK_CLEAR_DEPTH_STENCIL_IMAGE);
    assert(cleared->operations[0].image_destination == depth_dst);
    assert(cleared->operations[0].image_destination_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(cleared->operations[0].image_region_count == 1);
    /* The recorded word is the exact D32_SFLOAT bit pattern of 1.0f, which is
     * what the render pass load-op clear writes for the same value. */
    assert(cleared->operations[0].clear_word == 0x3f800000u);
    /* The range array is owned: a later caller mutation cannot change it. */
    const VkImageSubresourceRange *owned =
        (const VkImageSubresourceRange *)cleared->operations[0].owned_payload;
    assert(owned && owned != &whole_depth && owned->aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT);
    assert(cleared->operations[0].owned_payload_size == sizeof(whole_depth));
    /* The explicit one-level/one-layer spelling of the same whole subresource
     * is equally accepted, and GENERAL is a valid clear layout. */
    cleared = begin();
    vkCmdClearDepthStencilImage(cleared, depth_dst, VK_IMAGE_LAYOUT_GENERAL,
                                &(VkClearDepthStencilValue){.depth = 0.0f, .stencil = 0}, 1,
                                &(VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1});
    assert(cleared->state == PS5VK_RECORDING && cleared->operations[0].clear_word == 0u);

    /* The stencil member is IGNORED for a depth-only range, not rejected:
     * Vulkan reads it only when the aspect mask includes the stencil bit. The
     * pinned upstream CTS depends on this, clearing a D32_SFLOAT image with
     * makeClearValueDepthStencil(0.1f, 0x10), so a nonzero stencil must record
     * exactly the same depth clear as a zero one. */
    VkCommandBuffer stencil_zero = begin();
    vkCmdClearDepthStencilImage(stencil_zero, depth_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &(VkClearDepthStencilValue){.depth = 0.1f, .stencil = 0}, 1,
                                &whole_depth);
    VkCommandBuffer stencil_set = begin();
    vkCmdClearDepthStencilImage(stencil_set, depth_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &(VkClearDepthStencilValue){.depth = 0.1f, .stencil = 0x10}, 1,
                                &whole_depth);
    assert(stencil_zero->state == PS5VK_RECORDING && stencil_set->state == PS5VK_RECORDING);
    assert(stencil_zero->operation_count == 1 && stencil_set->operation_count == 1);
    assert(stencil_set->operations[0].type == PS5VK_CLEAR_DEPTH_STENCIL_IMAGE);
    /* Byte-identical recorded work: the same D32 word, the same range payload. */
    assert(stencil_set->operations[0].clear_word == stencil_zero->operations[0].clear_word);
    assert(stencil_set->operations[0].clear_word == 0x3dcccccdu);
    assert(stencil_set->operations[0].image_region_count ==
           stencil_zero->operations[0].image_region_count);
    assert(stencil_set->operations[0].owned_payload_size ==
           stencil_zero->operations[0].owned_payload_size);
    assert(!memcmp(stencil_set->operations[0].owned_payload,
                   stencil_zero->operations[0].owned_payload,
                   stencil_set->operations[0].owned_payload_size));
    /* The maximum stencil value is equally ignored. */
    bad = begin();
    vkCmdClearDepthStencilImage(bad, depth_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &(VkClearDepthStencilValue){.depth = 1.0f, .stencil = 0xffffffffu},
                                1, &whole_depth);
    assert(bad->state == PS5VK_RECORDING && bad->operations[0].clear_word == 0x3f800000u);

    /* Everything that would need 64KB_Z_X pixel addressing, an aspect this
     * format does not have, or a value the surface cannot store, stays closed
     * and records nothing. */
    const VkClearDepthStencilValue one = {.depth = 1.0f, .stencil = 0};
    const struct { VkImageLayout layout; VkClearDepthStencilValue value; VkImageSubresourceRange range; }
    refused[] = {
        /* stencil aspect: D32_SFLOAT has no stencil plane */
        {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, one, {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1}},
        {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, one,
         {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1}},
        {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, one, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
        /* partial ranges need per-pixel addressing */
        {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, one, {VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1, 0, 1}},
        {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, one, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1, 1}},
        {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, one, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 0, 1}},
        {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, one, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 0}},
        /* values outside the storable range, including NaN */
        {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, {.depth = 1.5f},
         {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}},
        {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, {.depth = -0.5f},
         {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}},
        {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, {.depth = (float)NAN},
         {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}},
        /* layouts that are not a transfer destination */
        {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, one, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}},
        {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, one,
         {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}},
        {VK_IMAGE_LAYOUT_UNDEFINED, one, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}},
    };
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        bad = begin();
        vkCmdClearDepthStencilImage(bad, depth_dst, refused[i].layout, &refused[i].value,
                                    1, &refused[i].range);
        assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    }
    /* A second range that is invalid rejects the whole call, leaving nothing. */
    const VkImageSubresourceRange pair[2] = {{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
                                             {VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1, 0, 1}};
    bad = begin();
    vkCmdClearDepthStencilImage(bad, depth_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 2, pair);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    /* Null and empty forms. */
    bad = begin();
    vkCmdClearDepthStencilImage(bad, depth_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 0, &whole_depth);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    bad = begin();
    vkCmdClearDepthStencilImage(bad, depth_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, NULL, 1, &whole_depth);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    bad = begin();
    vkCmdClearDepthStencilImage(bad, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &whole_depth);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    /* The colour transfer role is not a depth target. */
    bad = begin();
    vkCmdClearDepthStencilImage(bad, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &whole_depth);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);

    /* The advertised TRANSFER_DST bit is reachable in every form, which is why
     * it may be advertised at all: a D32 image created ONLY as a transfer
     * destination is a valid clear target, keeps the tiled depth footprint
     * rather than a padded linear one, and clears through the same path. The
     * feature bits themselves are pinned in tests/test_texture_format.c. */
    VkImage clear_only = make_image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT, NULL);
    VkMemoryRequirements depth_requirements, clear_only_requirements;
    vkGetImageMemoryRequirements(device, depth_dst, &depth_requirements);
    vkGetImageMemoryRequirements(device, clear_only, &clear_only_requirements);
    assert(clear_only_requirements.size == depth_requirements.size &&
           clear_only_requirements.alignment == depth_requirements.alignment);
    cleared = begin();
    vkCmdClearDepthStencilImage(cleared, clear_only, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &one, 1, &whole_depth);
    assert(cleared->state == PS5VK_RECORDING && cleared->operation_count == 1 &&
           cleared->operations[0].type == PS5VK_CLEAR_DEPTH_STENCIL_IMAGE);

    /* --- the cleared depth target becoming a depth attachment ---------------
     * Without this transition the clear cannot control anything: the whole
     * point of an explicit depth clear is that a later depth-tested draw reads
     * what it wrote. The contract is bounded to exactly that pair of scopes. */
    const VkImageSubresourceRange depth_range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0,
        VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkImageMemoryBarrier to_attachment = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = depth_dst, .subresourceRange = depth_range};
    VkCommandBuffer ordered = begin();
    vkCmdPipelineBarrier(ordered, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 0, NULL, 0, NULL, 1, &to_attachment);
    assert(ordered->state == PS5VK_RECORDING && ordered->operation_count == 1);
    assert(ordered->operations[0].type == PS5VK_IMAGE_BARRIER);
    assert(ordered->operations[0].image_barrier.newLayout ==
           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    /* The whole sequence a witness records: acquire the target, clear it, then
     * hand it to the depth tests. */
    ordered = begin();
    VkImageMemoryBarrier acquire = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = depth_dst, .subresourceRange = depth_range};
    vkCmdPipelineBarrier(ordered, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &acquire);
    vkCmdClearDepthStencilImage(ordered, depth_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &one, 1, &whole_depth);
    vkCmdPipelineBarrier(ordered, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 0, NULL, 0, NULL, 1, &to_attachment);
    assert(ordered->state == PS5VK_RECORDING && ordered->operation_count == 3);
    assert(ordered->operations[1].type == PS5VK_CLEAR_DEPTH_STENCIL_IMAGE);

    /* A pipeline may test depth at either fragment-test stage, so a narrower
     * single-stage destination mask is a valid barrier and must not be
     * refused; the emitted ordering is the same conservative acquire. */
    for (unsigned stage = 0; stage < 2; ++stage) {
        VkCommandBuffer narrow = begin();
        vkCmdPipelineBarrier(narrow, VK_PIPELINE_STAGE_TRANSFER_BIT,
            stage ? VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                  : VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            0, 0, NULL, 0, NULL, 1, &to_attachment);
        assert(narrow->state == PS5VK_RECORDING && narrow->operation_count == 1);
    }

    /* Anything outside that exact contract records nothing. */
    struct { VkPipelineStageFlags src, dst; VkAccessFlags src_access, dst_access;
             VkImageLayout old_layout, new_layout; VkImageAspectFlags aspect; } refused_barrier[] = {
        /* the colour aspect never orders a depth target */
        {VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
         VK_ACCESS_TRANSFER_WRITE_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
         VK_IMAGE_ASPECT_COLOR_BIT},
        /* read without write */
        {VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
         VK_IMAGE_ASPECT_DEPTH_BIT},
        /* a colour attachment scope on a depth target */
        {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
         VK_IMAGE_ASPECT_DEPTH_BIT},
        /* the reverse transition is not part of the contract */
        {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         VK_IMAGE_ASPECT_DEPTH_BIT},
        /* the depth target never becomes a sampled image */
        {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_ASPECT_DEPTH_BIT},
    };
    for (unsigned i = 0; i < sizeof(refused_barrier) / sizeof(refused_barrier[0]); ++i) {
        VkImageMemoryBarrier b = to_attachment;
        b.srcAccessMask = refused_barrier[i].src_access;
        b.dstAccessMask = refused_barrier[i].dst_access;
        b.oldLayout = refused_barrier[i].old_layout;
        b.newLayout = refused_barrier[i].new_layout;
        b.subresourceRange.aspectMask = refused_barrier[i].aspect;
        bad = begin();
        vkCmdPipelineBarrier(bad, refused_barrier[i].src, refused_barrier[i].dst,
                             0, 0, NULL, 0, NULL, 1, &b);
        assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    }
    /* The colour transfer role keeps its own contract: the depth transition is
     * not reachable for it. */
    VkImageMemoryBarrier colour_to_depth = to_attachment;
    colour_to_depth.image = destination;
    bad = begin();
    vkCmdPipelineBarrier(bad, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 0, NULL, 0, NULL, 1, &colour_to_depth);
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);

    /* in-render-pass attachment clears fail closed */
    bad = begin();
    vkCmdClearAttachments(bad, 1, &(VkClearAttachment){VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color.float32 = {0, 0, 0, 1}}},
                          1, &(VkClearRect){.rect = {{0, 0}, {WIDTH, HEIGHT}}, .baseArrayLayer = 0, .layerCount = 1});
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0); /* no active render pass */

    /* Closest otherwise-valid boundary: an active one-subpass render pass. */
    struct VkImageView_T active_view = {.device = device, .image = attachment,
        .range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkAttachmentDescription active_attachments[1] = {
        {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE}};
    struct ps5vk_subpass active_subpasses[1] = {
        {.color[0] = {.attachment = 0}, .color_count = 1, .depth = {.attachment = VK_ATTACHMENT_UNUSED}}};
    struct VkRenderPass_T active_pass = {.device = device, .attachment_count = 1,
        .subpass_count = 1, .attachments = active_attachments,
        .subpasses = active_subpasses};
    struct VkFramebuffer_T active_fb = {.device = device, .width = WIDTH, .height = HEIGHT,
        .attachment_count = 1, .attachments = {&active_view},
        .formats = {VK_FORMAT_R8G8B8A8_UNORM}, .samples = {VK_SAMPLE_COUNT_1_BIT},
        .color_attachments = {0}, .color_count = 1, .depth_attachment = VK_ATTACHMENT_UNUSED};
    VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = &active_pass, .framebuffer = &active_fb,
        .renderArea = {.extent = {WIDTH, HEIGHT}}};
    VkClearAttachment ca = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0};
    VkClearRect cr = {.rect = {.extent = {WIDTH, HEIGHT}}, .layerCount = 1};
    bad = begin();
    vkCmdBeginRenderPass(bad, &rp, VK_SUBPASS_CONTENTS_INLINE);
    assert(bad->state == PS5VK_RECORDING && bad->render_pass == &active_pass);
    vkCmdClearAttachments(bad, 1, &ca, 1, &cr);
    assert(bad->state == PS5VK_RECORDING && bad->operation_count==2);
    assert(ps5vk_clear_attachment_valid(&bad->operations[1]));
    ca.clearValue.color.float32[0]=1.0f;
    assert(bad->operations[1].clear_word==0); /* owned value */
    VkClearRect rectangles[2]={cr,cr};
    rectangles[1].rect.extent.width=WIDTH+1;
    vkCmdClearAttachments(bad,1,&ca,2,rectangles);
    assert(bad->state==PS5VK_INVALID && bad->operation_count==2); /* no valid prefix */

    /* --- the one linear-tiling role: the pinned host-readback staging image ---
     * Exactly one descriptor is accepted - RGBA8, 2D, one mip, one layer, one
     * sample, LINEAR tiling, TRANSFER_DST alone, exclusive sharing, UNDEFINED
     * initial layout - and its bytes are the padded linear layout the transfer
     * role already uses, so vkGetImageSubresourceLayout can describe it. */
    VkImageCreateInfo staging = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {WIDTH, HEIGHT, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage staging_image = VK_NULL_HANDLE;
    assert(vkCreateImage(device, &staging, NULL, &staging_image) == VK_SUCCESS);
    assert(ps5vk_linear_staging_image(staging_image));
    assert(!ps5vk_pure_transfer_image(staging_image) && !ps5vk_colour_transfer_image(staging_image));
    VkMemoryRequirements staging_requirements;
    vkGetImageMemoryRequirements(device, staging_image, &staging_requirements);
    struct ps5vk_texture_layout staging_layout = {0};
    assert(ps5vk_texture_layout_for_format(VK_FORMAT_R8G8B8A8_UNORM, WIDTH, HEIGHT,
        &staging_layout) == 0);
    assert(staging_requirements.size == staging_layout.bytes);
    assert(staging_requirements.alignment == staging_layout.alignment);
    assert(staging_requirements.memoryTypeBits == 1);
    VkImageSubresource subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout subresource_layout = {0};
    vkGetImageSubresourceLayout(device, staging_image, &subresource, &subresource_layout);
    assert(subresource_layout.offset == 0 && subresource_layout.rowPitch == staging_layout.row_pitch);
    assert(subresource_layout.depthPitch == staging_layout.bytes &&
           subresource_layout.size == staging_layout.bytes);
    assert(subresource_layout.arrayPitch == staging_layout.bytes);
    /* A tiled image and a subresource this role does not have report nothing
     * rather than a fabricated linear layout. */
    VkSubresourceLayout tiled_layout = {0};
    vkGetImageSubresourceLayout(device, source, &subresource, &tiled_layout);
    assert(!tiled_layout.offset && !tiled_layout.rowPitch && !tiled_layout.size);
    VkImageSubresource wrong_mip = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0};
    VkSubresourceLayout wrong_layout = {0};
    vkGetImageSubresourceLayout(device, staging_image, &wrong_mip, &wrong_layout);
    assert(!wrong_layout.rowPitch && !wrong_layout.size);
    VkImageSubresource wrong_aspect = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0};
    vkGetImageSubresourceLayout(device, staging_image, &wrong_aspect, &wrong_layout);
    assert(!wrong_layout.rowPitch && !wrong_layout.size);
    vkDestroyImage(device, staging_image, NULL);

    /* Everything else that asks for linear tiling stays refused before an
     * object exists. */
    {
        VkImageCreateInfo refused = staging;
        VkImage image = VK_NULL_HANDLE;
        refused.format = VK_FORMAT_B8G8R8A8_UNORM;
        assert(vkCreateImage(device, &refused, NULL, &image) == VK_ERROR_FORMAT_NOT_SUPPORTED && !image);
        refused = staging;
        refused.format = VK_FORMAT_R8G8B8A8_SNORM;
        assert(vkCreateImage(device, &refused, NULL, &image) == VK_ERROR_FORMAT_NOT_SUPPORTED && !image);
        refused = staging;
        refused.mipLevels = 2;
        assert(vkCreateImage(device, &refused, NULL, &image) == VK_ERROR_FORMAT_NOT_SUPPORTED && !image);
        refused = staging;
        refused.arrayLayers = 2;
        assert(vkCreateImage(device, &refused, NULL, &image) == VK_ERROR_FORMAT_NOT_SUPPORTED && !image);
        refused = staging;
        refused.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        assert(vkCreateImage(device, &refused, NULL, &image) == VK_ERROR_FORMAT_NOT_SUPPORTED && !image);
        refused = staging;
        refused.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        assert(vkCreateImage(device, &refused, NULL, &image) == VK_ERROR_FORMAT_NOT_SUPPORTED && !image);
        /* A 1D and a 3D linear request are both well formed for the generic
         * descriptor gate and still refused as the unsupported combinations
         * they are. */
        refused = staging;
        refused.imageType = VK_IMAGE_TYPE_1D;
        refused.extent.height = 1;
        assert(vkCreateImage(device, &refused, NULL, &image) == VK_ERROR_FORMAT_NOT_SUPPORTED && !image);
        refused = staging;
        refused.imageType = VK_IMAGE_TYPE_3D;
        refused.extent.depth = 4;
        assert(vkCreateImage(device, &refused, NULL, &image) == VK_ERROR_FORMAT_NOT_SUPPORTED && !image);
        refused = staging;
        refused.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        refused.extent.height = WIDTH;
        refused.arrayLayers = 6;
        assert(vkCreateImage(device, &refused, NULL, &image) == VK_ERROR_FORMAT_NOT_SUPPORTED && !image);
        /* Exclusive sharing and an UNDEFINED initial layout are generic
         * descriptor rules, so they are refused by the ordinary gate. */
        refused = staging;
        refused.sharingMode = VK_SHARING_MODE_CONCURRENT;
        assert(vkCreateImage(device, &refused, NULL, &image) == VK_ERROR_FEATURE_NOT_PRESENT && !image);
        refused = staging;
        refused.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        assert(vkCreateImage(device, &refused, NULL, &image) == VK_ERROR_FEATURE_NOT_PRESENT && !image);
    }

    /* --- the destination sequence the native witness pins on hardware ------
     * The witness clears the colour attachment and uploads an edge into it
     * through the transfer destination the pinned upstream draw cases declare,
     * then reads those bytes from the CPU. The same sequence is executed and
     * pinned here, so the witness cannot disagree with the driver about the
     * bytes it reads. */
    {
        enum { EDGE = 2 };
        const uint32_t clear_word = 0xff604020u, upload_word = 0xff1e140au;
        const uint32_t pitch = (WIDTH * 4u + 255u) & ~255u;
        VkImage colour = make_image(VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT, NULL);
        assert(ps5vk_colour_transfer_image(colour));
        uint32_t *upload_words = NULL;
        VkBuffer upload = make_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      EDGE * EDGE * 4, (void **)&upload_words);
        for (unsigned i = 0; i < EDGE * EDGE; ++i) upload_words[i] = upload_word;

        VkCommandBuffer command = begin();
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        /* The transitions are recorded and executed by the pinned sequence; this
         * block pins the bytes, so the committed layout they would establish is
         * injected exactly as a completed submission leaves it. */
        colour->layout = VK_IMAGE_LAYOUT_GENERAL;
        VkClearColorValue clear = {0};
        clear.float32[0] = 0x20 / 255.0f;
        clear.float32[1] = 0x40 / 255.0f;
        clear.float32[2] = 0x60 / 255.0f;
        clear.float32[3] = 1.0f;
        vkCmdClearColorImage(command, colour, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        VkBufferImageCopy region = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {EDGE, EDGE, 1}};
        vkCmdCopyBufferToImage(command, upload, colour, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        assert(command->state == PS5VK_RECORDING);
        (void)range;
        submit_and_wait(command);

        void *address = NULL;
        VkDeviceSize bytes = 0;
        assert(ps5vk_image_span(device, colour, &address, &bytes) == VK_SUCCESS);
        assert(bytes >= (VkDeviceSize)pitch * HEIGHT);
        unsigned clear_matched = 0, upload_matched = 0;
        for (unsigned y = 0; y < HEIGHT; ++y)
            for (unsigned x = 0; x < WIDTH; ++x) {
                uint32_t word = 0;
                memcpy(&word, (unsigned char *)address + (VkDeviceSize)y * pitch +
                       (VkDeviceSize)x * 4u, sizeof(word));
                if (x < EDGE && y < EDGE) upload_matched += word == upload_word;
                else clear_matched += word == clear_word;
            }
        assert(clear_matched == WIDTH * HEIGHT - EDGE * EDGE);
        assert(upload_matched == EDGE * EDGE);
        vkDestroyImage(device, colour, NULL);
    }

    /* --- layered attachment storage (T02-C3a) -----------------------------
     * Color and depth attachments may now name more than one array layer. The
     * storage is one footprint per layer, so creation, the layer target
     * selection and the image-view ranges have to agree about which layers
     * exist: a 2D_ARRAY view may address any layer the image really has and
     * none beyond it. */
    {
        VkImageCreateInfo layered = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {WIDTH, HEIGHT, 1},
            .mipLevels = 1, .arrayLayers = 3, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkImage array = VK_NULL_HANDLE;
        assert(vkCreateImage(device, &layered, NULL, &array) == VK_SUCCESS);
        VkMemoryRequirements layered_requirements;
        vkGetImageMemoryRequirements(device, array, &layered_requirements);
        VkMemoryAllocateInfo layered_allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = layered_requirements.size, .memoryTypeIndex = 0u};
        VkDeviceMemory layered_memory = VK_NULL_HANDLE;
        assert(vkAllocateMemory(device, &layered_allocation, NULL, &layered_memory) == VK_SUCCESS);
        assert(vkBindImageMemory(device, array, layered_memory, 0) == VK_SUCCESS);
        VkDeviceSize stride = 0, alignment = 0, bytes = 0;
        assert(ps5vk_native_layered_storage(layered.format, WIDTH, HEIGHT, 3u,
            &stride, &alignment, &bytes) == VK_SUCCESS);
        assert(layered_requirements.size == bytes &&
               layered_requirements.alignment == alignment);
        VkImageSubresourceRange whole_array={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,3};
        VkImageMemoryBarrier array_barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.image=array,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.subresourceRange=whole_array};
        VkCommandBuffer array_commands=begin();
        vkCmdPipelineBarrier(array_commands,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&array_barrier);
        vkCmdClearColorImage(array_commands,array,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &clear,1,&whole_array);
        assert(array_commands->state==PS5VK_RECORDING && array_commands->operation_count==2);
        assert(ps5vk_array_color_clear(&array_commands->operations[1]));
        assert(ps5vk_image_domain(&array_commands->operations[1])==PS5VK_IMAGE_DOMAIN_NONE);
        assert(ps5vk_image_transfer_execute(device,&array_commands->operations[1])!=VK_SUCCESS);
        whole_array.layerCount=2; /* owned range, not borrowed */
        assert(ps5vk_array_color_clear(&array_commands->operations[1]));
        VkCommandBuffer partial_array=begin();
        vkCmdClearColorImage(partial_array,array,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &clear,1,&whole_array);
        assert(partial_array->state==PS5VK_INVALID && !partial_array->operation_count);
        array_barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        array_barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        array_barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        array_barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(array_commands,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&array_barrier);
        assert(vkEndCommandBuffer(array_commands)==VK_SUCCESS);
        /* The whole array is addressable, and nothing beyond it is. */
        VkImageViewCreateInfo array_view = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = array,
            .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = layered.format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 3}};
        VkImageView view = VK_NULL_HANDLE;
        assert(vkCreateImageView(device, &array_view, NULL, &view) == VK_SUCCESS);
        vkDestroyImageView(device, view, NULL);
        /* A sub-range that stays inside the image is addressable; one that
         * reaches past the last layer is not. */
        array_view.subresourceRange = (VkImageSubresourceRange){
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, 2};
        assert(vkCreateImageView(device, &array_view, NULL, &view) == VK_SUCCESS);
        vkDestroyImageView(device, view, NULL);
        array_view.subresourceRange = (VkImageSubresourceRange){
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, 3};
        assert(vkCreateImageView(device, &array_view, NULL, &view) == VK_ERROR_UNKNOWN);
        array_view.subresourceRange = (VkImageSubresourceRange){
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 3, 1};
        assert(vkCreateImageView(device, &array_view, NULL, &view) == VK_ERROR_UNKNOWN);
        /* One layer still behaves exactly as before. */
        VkImage single = make_image(VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, NULL);
        VkMemoryRequirements single_requirements;
        vkGetImageMemoryRequirements(device, single, &single_requirements);
        assert(ps5vk_native_layered_storage(VK_FORMAT_R8G8B8A8_UNORM, WIDTH, HEIGHT, 1u,
            &stride, &alignment, &bytes) == VK_SUCCESS);
        assert(single_requirements.size == bytes &&
               single_requirements.alignment == alignment);
        vkDestroyImage(device, single, NULL);
        /* Depth attachments take layers under the same model with the depth
         * role's own alignment. */
        layered.format = VK_FORMAT_D32_SFLOAT;
        layered.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        VkImage depth_array = VK_NULL_HANDLE;
        assert(vkCreateImage(device, &layered, NULL, &depth_array) == VK_SUCCESS);
        VkMemoryRequirements depth_requirements;
        vkGetImageMemoryRequirements(device, depth_array, &depth_requirements);
        VkMemoryAllocateInfo depth_allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = depth_requirements.size, .memoryTypeIndex = 0u};
        VkDeviceMemory depth_memory = VK_NULL_HANDLE;
        assert(vkAllocateMemory(device, &depth_allocation, NULL, &depth_memory) == VK_SUCCESS);
        assert(vkBindImageMemory(device, depth_array, depth_memory, 0) == VK_SUCCESS);
        assert(ps5vk_native_layered_storage(VK_FORMAT_D32_SFLOAT, WIDTH, HEIGHT, 3u,
            &stride, &alignment, &bytes) == VK_SUCCESS);
        assert(depth_requirements.size == bytes && depth_requirements.alignment == alignment);
        vkDestroyImage(device, depth_array, NULL);
        vkFreeMemory(device, depth_memory, NULL);
        vkDestroyImage(device, array, NULL);
        vkFreeMemory(device, layered_memory, NULL);
    }

    /* A colour attachment that is also a transfer source AND a transfer
     * destination is still a readback target. The readback role in
     * src/vk_command.c is written for an image that is only a colour
     * attachment and a transfer source, so it excludes TRANSFER_DST; an
     * upstream case that clears or uploads through the same image and then
     * reads it back declares all three, fell through to the initialise-only
     * rule of the colour-transfer role and had its command buffer invalidated
     * at the readback transition. Measured as
     * dEQP-VK.draw.renderpass.scissor.* failing with vkEndCommandBuffer ->
     * VK_ERROR_UNKNOWN (2026-09-20 measurement run, eboot 749756aa). */
    {
        VkImage readback = make_image(VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT, NULL);
        assert(ps5vk_colour_transfer_image(readback));
        const VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier to_source = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = readback, .subresourceRange = whole};

        readback->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkCommandBuffer command = begin();
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_source);
        assert(command->state == PS5VK_RECORDING);
        assert(vkEndCommandBuffer(command) == VK_SUCCESS);

        /* and the return to rendering the same role already accepted. */
        VkImageMemoryBarrier back_to_colour = to_source;
        back_to_colour.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back_to_colour.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        back_to_colour.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        back_to_colour.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        readback->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        command = begin();
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &back_to_colour);
        assert(command->state == PS5VK_RECORDING);
        assert(vkEndCommandBuffer(command) == VK_SUCCESS);

        /* The widening is exact, not a layout wildcard: the same image and the
         * same pair of layouts with any other access scope stays refused. The
         * colour attachment that is only a transfer destination cannot widen
         * anything either, because the backend refuses that usage combination
         * at vkCreateImage (src/texture_format.c: each role contributes an
         * exact combination). */
        VkImageMemoryBarrier wrong_access = to_source;
        wrong_access.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        readback->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        command = begin();
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &wrong_access);
        assert(command->state == PS5VK_INVALID);
        assert(vkEndCommandBuffer(command) == VK_ERROR_UNKNOWN);

        VkImageCreateInfo no_source_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {WIDTH, HEIGHT, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage no_source = VK_NULL_HANDLE;
        assert(vkCreateImage(device, &no_source_info, NULL, &no_source) != VK_SUCCESS);

        vkDestroyImage(device, readback, NULL);
    }

    vkDestroyBuffer(device, alias_buffer, NULL);
    vkDestroyImage(device, alias_destination, NULL);
    vkDestroyImage(device, alias_source, NULL);
    vkDestroyImage(device, attachment, NULL);
    vkDestroyImage(device, clear_only, NULL);
    vkDestroyImage(device, depth_dst, NULL);
    vkDestroyImage(device, depth, NULL);
    vkDestroyImage(device, destination, NULL);
    vkDestroyImage(device, source, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    input_attachment_shape();
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    puts("image copy, colour clear, depth clear and the input-attachment shape: pass");
    return 0;
}
