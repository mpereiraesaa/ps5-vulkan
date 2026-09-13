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

    /* depth clears and in-render-pass attachment clears fail closed */
    VkImage depth = make_image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, NULL);
    bad = begin();
    vkCmdClearDepthStencilImage(bad, depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &(VkClearDepthStencilValue){.depth = 1.0f, .stencil = 0}, 1,
                                &(VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1});
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0);
    bad = begin();
    vkCmdClearAttachments(bad, 1, &(VkClearAttachment){VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color.float32 = {0, 0, 0, 1}}},
                          1, &(VkClearRect){.rect = {{0, 0}, {WIDTH, HEIGHT}}, .baseArrayLayer = 0, .layerCount = 1});
    assert(bad->state == PS5VK_INVALID && bad->operation_count == 0); /* no active render pass */

    /* Closest otherwise-valid boundary: an active one-subpass render pass. */
    struct VkImageView_T active_view = {.device = device, .image = attachment};
    struct VkRenderPass_T active_pass = {.device = device, .attachment_count = 1,
        .color = {.attachment = 0}, .depth = {.attachment = VK_ATTACHMENT_UNUSED},
        .attachments = {{.format = VK_FORMAT_R8G8B8A8_UNORM,
            .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE}}};
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

    vkDestroyBuffer(device, alias_buffer, NULL);
    vkDestroyImage(device, alias_destination, NULL);
    vkDestroyImage(device, alias_source, NULL);
    vkDestroyImage(device, attachment, NULL);
    vkDestroyImage(device, depth, NULL);
    vkDestroyImage(device, destination, NULL);
    vkDestroyImage(device, source, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    puts("image copy and colour clear: pass (clear, copy, guards, padding, fail-closed)");
    return 0;
}
