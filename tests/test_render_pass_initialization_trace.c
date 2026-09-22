/*
 * Host trace of the pinned upstream render-pass module's attachment
 * initialization sequence, for the two-attachment write-mask leaves that
 * REQUIRE independentBlend (vktRenderPassTests.cpp:6504).
 *
 * Every value in this fixture is read from the module that runs those leaves:
 *
 *   1. the attachments. initializeAttachmentImageUsage
 *      (vktRenderPassTests.cpp:5307) derives the usage of each attachment from
 *      the reported format features: the colour-attachment role from
 *      COLOR_ATTACHMENT_OPTIMAL, the transfer-destination role because the
 *      pass clears the attachment, the transfer-source role because the
 *      attachment is not transient, and the sampled role because the format
 *      publishes that feature. The leaf draws into R8G8B8A8_UINT and
 *      R8G8B8A8_UNORM (:6444) at 64x64 (:6523).
 *   2. the first barrier. pushImageInitializationCommands (:3150) records ONE
 *      vkCmdPipelineBarrier holding one barrier per attachment, from the
 *      transfer stage to ALL_COMMANDS | HOST, with no source access and a
 *      destination of getAllMemoryReadFlags() | VK_ACCESS_TRANSFER_WRITE_BIT
 *      (:457, :3167).
 *   3. the clear of each attachment through its transfer-destination layout.
 *   4. the second barrier: same stages, TRANSFER_DST_OPTIMAL to the
 *      attachment's initial layout, from the transfer write to
 *      getAllMemoryReadFlags() | the layout's own access (:3249).
 *
 * On the console this was the gate that stopped the leaves: the profile's
 * graphics access scope did not know three read bits the module names
 * (INDEX_READ, UNIFORM_READ, INPUT_ATTACHMENT_READ) and the colour readback
 * profile accepted a destination of TRANSFER_WRITE alone. This fixture pins
 * the widened but still bounded surface: the module's sequence is recorded
 * without invalidating the command buffer, while a foreign access bit, a
 * missing write, a wrong stage or a layout the module does not use still
 * refuses.
 *
 * The two roles that exist only in the build serving the integer-target
 * measurement (the sampled colour readback shape and the integer format) are
 * asserted in both directions: the measurement build records the module's own
 * usage set, and the shipping build refuses the shape it does not serve. The
 * Makefile compiles this one file for each of those builds.
 */
#include "vk_internal.h"
#include "vk_command.h"
#include "color_barrier.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = *backing = calloc(1, (size_t)size);
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult sync_call(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, sync_call, sync_call};
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

/* The leaf's target size (vktRenderPassTests.cpp:6523). */
enum { WIDTH = 64, HEIGHT = 64 };

/* The module's own usage set for an attachment of this leaf, and the same
 * attachment without the sampled role. */
static const VkImageUsageFlags pinned_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT;
static const VkImageUsageFlags readback_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

/* The stages the module records its two barriers with (:3187, :3268). */
static const VkPipelineStageFlags pinned_source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
static const VkPipelineStageFlags pinned_destination_stage =
    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT;

static VkDevice device;
static VkCommandPool pool;

static VkResult create_image(VkFormat format, VkImageUsageFlags usage, VkImage *image_out,
                             VkDeviceMemory *memory_out)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {WIDTH, HEIGHT, 1},
                              .mipLevels = 1, .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage image = VK_NULL_HANDLE;
    VkResult rc = vkCreateImage(device, &info, NULL, &image);
    if (rc != VK_SUCCESS) return rc;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = requirements.size,
                                       .memoryTypeIndex = 0u};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    assert(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, image, memory, 0) == VK_SUCCESS);
    *image_out = image; *memory_out = memory;
    return VK_SUCCESS;
}
static VkImage make_image(VkFormat format, VkImageUsageFlags usage, VkDeviceMemory *memory_out)
{
    VkImage image = VK_NULL_HANDLE;
    assert(create_image(format, usage, &image, memory_out) == VK_SUCCESS);
    return image;
}
static VkBuffer make_buffer(VkDeviceSize size, VkDeviceMemory *memory_out)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer = VK_NULL_HANDLE;
    assert(vkCreateBuffer(device, &info, NULL, &buffer) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = requirements.size,
                                       .memoryTypeIndex = 0u};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    assert(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);
    *memory_out = memory;
    return buffer;
}
static VkCommandBuffer begin(void)
{
    VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1};
    VkCommandBuffer command = VK_NULL_HANDLE;
    assert(vkAllocateCommandBuffers(device, &info, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT};
    assert(vkBeginCommandBuffer(command, &begin_info) == VK_SUCCESS);
    return command;
}

/* The module's acquire: UNDEFINED to the transfer-destination layout, naming
 * the destination scope it passes (:3163). */
static VkImageMemoryBarrier acquire_barrier(VkImage image, VkAccessFlags destination)
{
    return (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = image,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcAccessMask = 0, .dstAccessMask = destination,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
}
/* The module's handover of the cleared attachment to its attachment layout
 * (:3245). */
static VkImageMemoryBarrier handover_barrier(VkImage image, VkAccessFlags destination)
{
    VkImageMemoryBarrier barrier = acquire_barrier(image, destination);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    return barrier;
}

/* The module's readback barrier (pushReadImagesToBuffers,
 * vktRenderPassTests.cpp:3582): the attachment is already in its final
 * layout, so the transition is the identity, and the scope names every memory
 * write plus the copy's own read. */
static VkImageMemoryBarrier readback_barrier(VkImage image, VkAccessFlags source,
                                             VkAccessFlags destination)
{
    VkImageMemoryBarrier barrier = acquire_barrier(image, destination);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = source;
    return barrier;
}

/* getAllPipelineStageFlags() (vktRenderPassTests.cpp:494): the stage pair the
 * module records both its readback barriers with. */
static const VkPipelineStageFlags pinned_readback_stage =
    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
    VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT;

/* One image barrier in its own command buffer: the profile refuses by
 * invalidating the command buffer where the call was made, so this asserts the
 * refusal instead of only the appended work. */
static void assert_barrier_refused(VkPipelineStageFlags source, VkPipelineStageFlags destination,
                                   const VkImageMemoryBarrier *barrier)
{
    VkCommandBuffer command = begin();
    vkCmdPipelineBarrier(command, source, destination, 0, 0, NULL, 0, NULL, 1, barrier);
    assert(command->state != PS5VK_RECORDING);
}
/* The same for the graphics access scope, through a memory barrier. */
static void assert_scope_refused(VkPipelineStageFlags source, VkPipelineStageFlags destination,
                                 VkAccessFlags destination_access)
{
    const VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                     .srcAccessMask = 0, .dstAccessMask = destination_access};
    VkCommandBuffer command = begin();
    vkCmdPipelineBarrier(command, source, destination, 0, 1, &barrier, 0, NULL, 0, NULL);
    assert(command->state != PS5VK_RECORDING);
}
static void assert_scope_accepted(VkPipelineStageFlags source, VkPipelineStageFlags destination,
                                  VkAccessFlags destination_access)
{
    const VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                     .srcAccessMask = 0, .dstAccessMask = destination_access};
    VkCommandBuffer command = begin();
    vkCmdPipelineBarrier(command, source, destination, 0, 1, &barrier, 0, NULL, 0, NULL);
    assert(command->state == PS5VK_RECORDING);
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
}

static void exercise_device(void)
{
    /* The destination scope the module names is its own getAllMemoryReadFlags()
     * (vktRenderPassTests.cpp:457): the ten reads listed there, no more and no
     * fewer. The profile spells it once, and this pins it. */
    const VkAccessFlags reads = ps5vk_attachment_initialization_read_mask();
    assert(reads == (VkAccessFlags)(VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
        VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
        VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
        VK_ACCESS_HOST_READ_BIT));

    /* The module's own source side names no access at all, and its destination
     * scope is the one above plus the write the clear performs. */
    assert_scope_accepted(pinned_source_stage, pinned_destination_stage, 0);
    assert_scope_accepted(pinned_source_stage, pinned_destination_stage,
                          reads | VK_ACCESS_TRANSFER_WRITE_BIT);

    /* The three reads the module names that this scope did not know. Each is
     * accepted with the stage that performs it and refused with a stage that
     * cannot perform it. */
    assert_scope_accepted(pinned_source_stage, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                          VK_ACCESS_INDEX_READ_BIT);
    assert_scope_accepted(pinned_source_stage, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                          VK_ACCESS_UNIFORM_READ_BIT);
    assert_scope_accepted(pinned_source_stage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_INPUT_ATTACHMENT_READ_BIT);
    assert_scope_refused(pinned_source_stage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_INDEX_READ_BIT);
    assert_scope_refused(pinned_source_stage, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_UNIFORM_READ_BIT);
    assert_scope_refused(pinned_source_stage, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_INPUT_ATTACHMENT_READ_BIT);
    /* Widening the known reads must not turn the scope into a wildcard: an
     * access this profile still has no path for stays refused. */
    assert_scope_refused(pinned_source_stage, pinned_destination_stage,
                         VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT);

    /* The colour readback shape this profile has always served. */
    VkDeviceMemory first_memory = VK_NULL_HANDLE, second_memory = VK_NULL_HANDLE;
    VkImage first = make_image(VK_FORMAT_R8G8B8A8_UNORM, readback_usage, &first_memory);
    VkImage second = make_image(VK_FORMAT_R8G8B8A8_UNORM, readback_usage, &second_memory);
    assert(ps5vk_colour_transfer_image(first) && ps5vk_colour_transfer_image(second));

    /* The whole sequence the module records, one call per step. */
    VkImageMemoryBarrier acquire[2] = {
        acquire_barrier(first, reads | VK_ACCESS_TRANSFER_WRITE_BIT),
        acquire_barrier(second, reads | VK_ACCESS_TRANSFER_WRITE_BIT)};
    VkImageMemoryBarrier handover[2] = {
        handover_barrier(first, reads | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
        handover_barrier(second, reads | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)};
    const VkClearColorValue clear_colour = {.float32 = {0.25f, 0.5f, 0.75f, 1.0f}};
    const VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkCommandBuffer initialization = begin();
    vkCmdPipelineBarrier(initialization, pinned_source_stage, pinned_destination_stage, 0,
                         0, NULL, 0, NULL, 2, acquire);
    assert(initialization->state == PS5VK_RECORDING);
    vkCmdClearColorImage(initialization, first, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear_colour, 1, &whole);
    assert(initialization->state == PS5VK_RECORDING);
    vkCmdClearColorImage(initialization, second, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear_colour, 1, &whole);
    assert(initialization->state == PS5VK_RECORDING);
    vkCmdPipelineBarrier(initialization, pinned_source_stage, pinned_destination_stage, 0,
                         0, NULL, 0, NULL, 2, handover);
    assert(initialization->state == PS5VK_RECORDING);
    assert(vkEndCommandBuffer(initialization) == VK_SUCCESS);

    /* The readback the module records next: one identity handover per
     * attachment, the copy of each attachment into its own buffer, and the
     * buffer scope that makes the copies visible to the host read. */
    const VkAccessFlags writes = ps5vk_attachment_initialization_write_mask();
    VkDeviceMemory first_buffer_memory = VK_NULL_HANDLE, second_buffer_memory = VK_NULL_HANDLE;
    VkBuffer first_buffer = make_buffer((VkDeviceSize)WIDTH * HEIGHT * 4u, &first_buffer_memory);
    VkBuffer second_buffer = make_buffer((VkDeviceSize)WIDTH * HEIGHT * 4u, &second_buffer_memory);
    VkImageMemoryBarrier readback[2] = {
        readback_barrier(first, writes | VK_ACCESS_TRANSFER_READ_BIT, reads),
        readback_barrier(second, writes | VK_ACCESS_TRANSFER_READ_BIT, reads)};
    const VkBufferImageCopy whole_surface = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {WIDTH, HEIGHT, 1}};
    const VkBufferMemoryBarrier buffered[2] = {
        {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = writes,
         .dstAccessMask = reads, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = first_buffer,
         .offset = 0, .size = VK_WHOLE_SIZE},
        {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = writes,
         .dstAccessMask = reads, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = second_buffer,
         .offset = 0, .size = VK_WHOLE_SIZE}};
    VkCommandBuffer readback_commands = begin();
    vkCmdPipelineBarrier(readback_commands, pinned_readback_stage, pinned_readback_stage, 0,
                         0, NULL, 0, NULL, 2, readback);
    assert(readback_commands->state == PS5VK_RECORDING);
    vkCmdCopyImageToBuffer(readback_commands, first, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           first_buffer, 1, &whole_surface);
    assert(readback_commands->state == PS5VK_RECORDING);
    vkCmdCopyImageToBuffer(readback_commands, second, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           second_buffer, 1, &whole_surface);
    assert(readback_commands->state == PS5VK_RECORDING);
    vkCmdPipelineBarrier(readback_commands, pinned_readback_stage, pinned_readback_stage, 0,
                         0, NULL, 2, buffered, 0, NULL);
    assert(readback_commands->state == PS5VK_RECORDING);
    assert(vkEndCommandBuffer(readback_commands) == VK_SUCCESS);

    /* The identity is not a wildcard: it needs the copy's own read and the
     * write the pass performed, and it lands in the attachment's final layout
     * or nowhere. */
    VkImageMemoryBarrier boundary = readback_barrier(first, writes | VK_ACCESS_TRANSFER_READ_BIT, reads);
    boundary.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    assert_barrier_refused(pinned_readback_stage, pinned_readback_stage, &boundary);
    boundary = readback_barrier(first, writes | VK_ACCESS_TRANSFER_READ_BIT, reads);
    boundary.srcAccessMask = writes | VK_ACCESS_MEMORY_WRITE_BIT;
    assert_barrier_refused(pinned_readback_stage, pinned_readback_stage, &boundary);
    boundary = readback_barrier(first, writes | VK_ACCESS_TRANSFER_READ_BIT,
                                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
    assert_barrier_refused(pinned_readback_stage, pinned_readback_stage, &boundary);
    boundary = readback_barrier(first, writes | VK_ACCESS_TRANSFER_READ_BIT, reads);
    boundary.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    assert_barrier_refused(pinned_readback_stage, pinned_readback_stage, &boundary);

    /* The two destination scopes the profile accepted before this slice stay
     * accepted, and the rest of the boundary holds. */
    VkImageMemoryBarrier exact_acquire = acquire_barrier(first, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageMemoryBarrier exact_handover = handover_barrier(first, VK_ACCESS_SHADER_WRITE_BIT);
    VkCommandBuffer exact = begin();
    vkCmdPipelineBarrier(exact, pinned_source_stage, pinned_destination_stage, 0,
                         0, NULL, 0, NULL, 1, &exact_acquire);
    assert(exact->state == PS5VK_RECORDING);
    vkCmdPipelineBarrier(exact, pinned_source_stage, pinned_destination_stage, 0,
                         0, NULL, 0, NULL, 1, &exact_handover);
    assert(exact->state == PS5VK_RECORDING);
    assert(vkEndCommandBuffer(exact) == VK_SUCCESS);

    /* No write in the destination scope. */
    boundary = acquire_barrier(first, reads);
    assert_barrier_refused(pinned_source_stage, pinned_destination_stage, &boundary);
    boundary = handover_barrier(first, reads | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
    assert_barrier_refused(pinned_source_stage, pinned_destination_stage, &boundary);
    /* A write the transition did not perform, or a foreign bit beside it. */
    boundary = acquire_barrier(first, reads | VK_ACCESS_TRANSFER_WRITE_BIT |
                                      VK_ACCESS_MEMORY_WRITE_BIT);
    assert_barrier_refused(pinned_source_stage, pinned_destination_stage, &boundary);
    boundary = handover_barrier(first, reads | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                       VK_ACCESS_MEMORY_WRITE_BIT);
    assert_barrier_refused(pinned_source_stage, pinned_destination_stage, &boundary);
    /* The acquire discards the old contents; it cannot name a source. */
    boundary = acquire_barrier(first, reads | VK_ACCESS_TRANSFER_WRITE_BIT);
    boundary.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    assert_barrier_refused(pinned_source_stage, pinned_destination_stage, &boundary);
    /* The handover is ordered by the transfer write the clear performed. */
    boundary = handover_barrier(first, reads | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    boundary.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    assert_barrier_refused(pinned_source_stage, pinned_destination_stage, &boundary);
    /* And it lands in the attachment layout, which is not a wildcard. */
    boundary = handover_barrier(first, reads | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    boundary.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    assert_barrier_refused(pinned_source_stage, pinned_destination_stage, &boundary);

    /* The shape the leaves actually create, and the integer format they draw
     * into, are part of the served capability set now that independentBlend is
     * promoted: the sampled colour readback shape and R8G8B8A8_UINT are what
     * the two upstream leaves that require the feature build. */
    const VkFormat pinned_formats[2] = {VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UNORM};
    VkDeviceMemory pinned_memory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImage pinned[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageMemoryBarrier pinned_acquire[2];
    VkImageMemoryBarrier pinned_handover[2];
    for (unsigned i = 0; i < 2; ++i) {
        pinned[i] = make_image(pinned_formats[i], pinned_usage, &pinned_memory[i]);
        assert(ps5vk_colour_transfer_image(pinned[i]));
        pinned_acquire[i] = acquire_barrier(pinned[i], reads | VK_ACCESS_TRANSFER_WRITE_BIT);
        pinned_handover[i] = handover_barrier(pinned[i], reads |
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    VkCommandBuffer measured = begin();
    vkCmdPipelineBarrier(measured, pinned_source_stage, pinned_destination_stage, 0,
                         0, NULL, 0, NULL, 2, pinned_acquire);
    assert(measured->state == PS5VK_RECORDING);
    for (unsigned i = 0; i < 2; ++i) {
        vkCmdClearColorImage(measured, pinned[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear_colour, 1, &whole);
        assert(measured->state == PS5VK_RECORDING);
    }
    vkCmdPipelineBarrier(measured, pinned_source_stage, pinned_destination_stage, 0,
                         0, NULL, 0, NULL, 2, pinned_handover);
    assert(measured->state == PS5VK_RECORDING);
    assert(vkEndCommandBuffer(measured) == VK_SUCCESS);
    /* The readback the module records for the same two attachments. */
    VkDeviceMemory pinned_buffer_memory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkBuffer pinned_buffer[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageMemoryBarrier pinned_readback[2];
    for (unsigned i = 0; i < 2; ++i) {
        pinned_buffer[i] = make_buffer((VkDeviceSize)WIDTH * HEIGHT * 4u, &pinned_buffer_memory[i]);
        pinned_readback[i] = readback_barrier(pinned[i], writes | VK_ACCESS_TRANSFER_READ_BIT, reads);
    }
    VkCommandBuffer measured_readback = begin();
    vkCmdPipelineBarrier(measured_readback, pinned_readback_stage, pinned_readback_stage, 0,
                         0, NULL, 0, NULL, 2, pinned_readback);
    assert(measured_readback->state == PS5VK_RECORDING);
    for (unsigned i = 0; i < 2; ++i) {
        vkCmdCopyImageToBuffer(measured_readback, pinned[i],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pinned_buffer[i], 1,
                               &whole_surface);
        assert(measured_readback->state == PS5VK_RECORDING);
    }
    assert(vkEndCommandBuffer(measured_readback) == VK_SUCCESS);
    for (unsigned i = 0; i < 2; ++i) {
        vkDestroyBuffer(device, pinned_buffer[i], NULL);
        vkFreeMemory(device, pinned_buffer_memory[i], NULL);
        vkDestroyImage(device, pinned[i], NULL);
        vkFreeMemory(device, pinned_memory[i], NULL);
    }

    vkDestroyImage(device, first, NULL);
    vkFreeMemory(device, first_memory, NULL);
    vkDestroyImage(device, second, NULL);
    vkFreeMemory(device, second_memory, NULL);
    vkDestroyBuffer(device, first_buffer, NULL);
    vkFreeMemory(device, first_buffer_memory, NULL);
    vkDestroyBuffer(device, second_buffer, NULL);
    vkFreeMemory(device, second_buffer_memory, NULL);
}

int main(void)
{
    VkInstance instance;
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&instance_info, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1;
    VkPhysicalDevice physical;
    assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                          .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                      .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
    assert(vkCreateDevice(physical, &device_info, NULL, &device) == VK_SUCCESS);
    device->graphics_enabled = VK_TRUE;
    device->image_requirements = ps5vk_native_image_requirements;
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    assert(vkCreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS);

    exercise_device();

    vkDestroyCommandPool(device, pool, NULL);
    puts("Render-pass initialization trace: the pinned module's attachment barriers, clears and handovers are recorded for the colour readback role, the graphics access scope carries the module's whole read mask with its own stage rules, and every foreign scope, missing write or wrong layout still refuses");
    return 0;
}
