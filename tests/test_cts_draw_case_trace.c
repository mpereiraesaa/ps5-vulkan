/*
 * Host trace of the pinned upstream draw-parameter case sequence.
 *
 * The promotion's first hardware launches each discovered one more CTS
 * convention, so this fixture walks the whole sequence the pinned draw module
 * uses - colour target creation, the UNDEFINED-to-GENERAL transfer-write
 * transition, the clear in GENERAL, the buffer-to-image upload in GENERAL, the
 * memory barrier into the colour-attachment stages, the GENERAL render pass
 * begin/end - and records where the profile stops. The final block pins the
 * one remaining boundary: the readback staging image the CTS creates with
 * VK_IMAGE_TILING_LINEAR, which this profile refuses because every image it
 * accepts is tiled optimal. Extending the profile there is a separate,
 * separately-evidenced step; this test exists so that gap cannot be
 * rediscovered one launch at a time.
 *
 * It is a recording-layer trace: no GPU work, no submission and no readback are
 * performed, so the pipeline/draw/readback execution gates remain covered by
 * tests/test_vk_graphics_pipeline.c, the emitter tests and the hardware session.
 */
#include "vk_internal.h"
#include "vk_command.h"
#include "vk_framebuffer.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

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

enum { WIDTH = 256, HEIGHT = 256 };
static VkDevice device;
static VkCommandPool pool;

static VkImage make_image(VkImageTiling tiling, VkImageUsageFlags usage)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = VK_FORMAT_R8G8B8A8_UNORM,
                              .extent = {WIDTH, HEIGHT, 1},
                              .mipLevels = 1, .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = tiling, .usage = usage,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage image = VK_NULL_HANDLE;
    assert(vkCreateImage(device, &info, NULL, &image) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = requirements.size,
                                       .memoryTypeIndex = 0u};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    assert(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, image, memory, 0) == VK_SUCCESS);
    return image;
}

static VkCommandBuffer begin(void)
{
    VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1};
    VkCommandBuffer command = VK_NULL_HANDLE;
    assert(vkAllocateCommandBuffers(device, &info, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(command, &begin_info) == VK_SUCCESS);
    return command;
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

    /* 1. The colour target the pinned draw module creates. */
    VkImage target = make_image(VK_IMAGE_TILING_OPTIMAL,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    assert(ps5vk_colour_transfer_image(target));

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = target,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view = VK_NULL_HANDLE;
    assert(vkCreateImageView(device, &view_info, NULL, &view) == VK_SUCCESS);

    /* 2. The GENERAL render pass and framebuffer the pinned draw module uses. */
    VkAttachmentDescription attachment = {
        .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
        .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1, .pColorAttachments = &color};
    VkRenderPassCreateInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass};
    VkRenderPass pass = VK_NULL_HANDLE;
    assert(vkCreateRenderPass(device, &pass_info, NULL, &pass) == VK_SUCCESS);
    VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
        .attachmentCount = 1, .pAttachments = &view,
        .width = WIDTH, .height = HEIGHT, .layers = 1};
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    assert(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer) == VK_SUCCESS);

    /* 3. The pre-render sequence: UNDEFINED to GENERAL for a transfer write,
     *    the clear in GENERAL, an upload in GENERAL, then a memory barrier into
     *    the colour-attachment stages. */
    VkCommandBuffer command = begin();
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier to_general = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = target, .subresourceRange = range};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
    assert(command->state == PS5VK_RECORDING);
    VkClearColorValue clear_value = {0};
    clear_value.float32[3] = 1.0f;
    vkCmdClearColorImage(command, target, VK_IMAGE_LAYOUT_GENERAL, &clear_value, 1, &range);
    assert(command->state == PS5VK_RECORDING);
    VkBufferImageCopy region = {
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0}, .imageExtent = {WIDTH, HEIGHT, 1}};
    VkBuffer source = VK_NULL_HANDLE;
    VkBufferCreateInfo source_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size = WIDTH * HEIGHT * 4,
                                      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    assert(vkCreateBuffer(device, &source_info, NULL, &source) == VK_SUCCESS);
    VkMemoryRequirements source_requirements;
    vkGetBufferMemoryRequirements(device, source, &source_requirements);
    VkMemoryAllocateInfo source_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = source_requirements.size, .memoryTypeIndex = 0u};
    VkDeviceMemory source_memory = VK_NULL_HANDLE;
    assert(vkAllocateMemory(device, &source_allocation, NULL, &source_memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, source, source_memory, 0) == VK_SUCCESS);
    vkCmdCopyBufferToImage(command, source, target, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    assert(command->state == PS5VK_RECORDING);
    VkMemoryBarrier memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &memory, 0, NULL, 0, NULL);
    assert(command->state == PS5VK_RECORDING);
    VkClearValue clear_values[1] = {0};
    VkRenderPassBeginInfo pass_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass, .framebuffer = framebuffer,
        .renderArea = {{0, 0}, {WIDTH, HEIGHT}},
        .clearValueCount = 1, .pClearValues = clear_values};
    vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    assert(command->state == PS5VK_RECORDING && command->render_pass == pass);
    vkCmdEndRenderPass(command);
    assert(command->state == PS5VK_RECORDING && !command->render_pass);
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);

    /* 4. The one remaining boundary: the pinned readback stages through a
     *    VK_IMAGE_TILING_LINEAR image, which this profile refuses because every
     *    image it accepts is tiled optimal. When that role is implemented this
     *    assertion becomes the creation of the staging image plus the
     *    GENERAL-to-GENERAL image copy the module issues. */
    VkImageCreateInfo staging_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {WIDTH, HEIGHT, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage staging = VK_NULL_HANDLE;
    assert(vkCreateImage(device, &staging_info, NULL, &staging) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !staging);

    puts("Pinned draw-case trace: sequence accepted up to the linear readback staging");
    return 0;
}
