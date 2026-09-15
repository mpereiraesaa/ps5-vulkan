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
#include <assert.h>
#include <math.h>
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

static VkImage make_image(VkFormat format, VkImageUsageFlags usage, void **mapped)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {WIDTH, HEIGHT, 1},
                              .mipLevels = 1, .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
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
    struct VkImageView_T active_view = {.device = device, .image = attachment};
    VkAttachmentDescription active_attachments[1] = {
        {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE}};
    struct ps5vk_subpass active_subpasses[1] = {
        {.color = {.attachment = 0}, .depth = {.attachment = VK_ATTACHMENT_UNUSED}}};
    struct VkRenderPass_T active_pass = {.device = device, .attachment_count = 1,
        .subpass_count = 1, .attachments = active_attachments,
        .subpasses = active_subpasses};
    struct VkFramebuffer_T active_fb = {.device = device, .width = WIDTH, .height = HEIGHT,
        .attachment_count = 1, .attachments = {&active_view},
        .formats = {VK_FORMAT_R8G8B8A8_UNORM}, .samples = {VK_SAMPLE_COUNT_1_BIT},
        .color_attachment = 0, .depth_attachment = VK_ATTACHMENT_UNUSED};
    VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = &active_pass, .framebuffer = &active_fb,
        .renderArea = {.extent = {WIDTH, HEIGHT}}};
    VkClearAttachment ca = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0};
    VkClearRect cr = {.rect = {.extent = {WIDTH, HEIGHT}}, .layerCount = 1};
    bad = begin();
    vkCmdBeginRenderPass(bad, &rp, VK_SUBPASS_CONTENTS_INLINE);
    assert(bad->state == PS5VK_RECORDING && bad->render_pass == &active_pass);
    vkCmdClearAttachments(bad, 1, &ca, 1, &cr);
    assert(bad->state == PS5VK_INVALID);

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
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    puts("image copy, colour clear and whole-subresource depth clear: pass");
    return 0;
}
