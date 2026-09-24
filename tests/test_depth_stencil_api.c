/*
 * T09 public-API contracts for the combined D32_SFLOAT_S8_UINT attachment and
 * per-aspect (separateDepthStencilLayouts) recording. Built twice: the
 * shipping build, where the format does not exist, and the private
 * PS5VK_DEPTH_STENCIL_DIAGNOSTIC build, where it and the per-aspect barriers
 * are accepted. Host only.
 */
#include "vk_internal.h"
#include "vk_command.h"
#include "vk_image.h"
#include "vk_render_pass.h"
#include "graphics_formats.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PS5VK_DEPTH_STENCIL_DIAGNOSTIC) && PS5VK_DEPTH_STENCIL_DIAGNOSTIC
#define DIAGNOSTIC 1
#else
#define DIAGNOSTIC 0
#endif

static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = aligned_alloc(65536, ((size_t)size + 65535u) & ~(size_t)65535u); *backing = *address;
    if (*address) memset(*address, 0, (size_t)size);
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult cache_noop(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, cache_noop, cache_noop};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }
VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
        .max_allocation = 1u << 22, .queue_flags = VK_QUEUE_COMPUTE_BIT};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU", .vendor_id = 0x1002u, .heap_size = 1u << 22,
        .allocation_granularity = 1, .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

static VkDevice device;
static VkCommandPool pool;

static VkResult make_image(VkImageUsageFlags usage, uint32_t mips, VkImage *image,
    VkDeviceMemory *memory)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT_S8_UINT,
        .extent = {64, 64, 1}, .mipLevels = mips, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult rc = vkCreateImage(device, &info, NULL, image);
    if (rc != VK_SUCCESS) return rc;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, *image, &req);
    assert(req.size == 131072u && req.alignment == 65536u);
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = 0};
    assert(vkAllocateMemory(device, &mi, NULL, memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, *image, *memory, 0) == VK_SUCCESS);
    return VK_SUCCESS;
}

static VkCommandBuffer begin(void)
{
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer c;
    assert(vkAllocateCommandBuffers(device, &ai, &c) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(c, &bi) == VK_SUCCESS);
    return c;
}

/* One barrier in a fresh command buffer; returns whether recording stayed
 * valid. */
static int record_barrier(VkImage image, VkImageAspectFlags aspects, VkImageLayout old,
    VkImageLayout next, VkAccessFlags src_access, VkAccessFlags dst_access,
    VkPipelineStageFlags src, VkPipelineStageFlags dst)
{
    VkCommandBuffer c = begin();
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .oldLayout = old, .newLayout = next,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {aspects, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(c, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
    const int valid = vkEndCommandBuffer(c) == VK_SUCCESS;
    vkFreeCommandBuffers(device, pool, 1, &c);
    return valid;
}

static int record_copy(VkImage image, VkImageAspectFlags aspect, VkBuffer buffer)
{
    VkCommandBuffer c = begin();
    VkBufferImageCopy region = {.imageSubresource = {aspect, 0, 0, 1},
        .imageExtent = {64, 64, 1}};
    vkCmdCopyImageToBuffer(c, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    const int valid = vkEndCommandBuffer(c) == VK_SUCCESS;
    vkFreeCommandBuffers(device, pool, 1, &c);
    return valid;
}

#define LATE VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
#define XFER VK_PIPELINE_STAGE_TRANSFER_BIT
#define DSW VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
#define TR VK_ACCESS_TRANSFER_READ_BIT
#define D VK_IMAGE_ASPECT_DEPTH_BIT
#define S VK_IMAGE_ASPECT_STENCIL_BIT

static void render_pass_layouts(void)
{
    VkAttachmentDescription a = {.format = VK_FORMAT_D32_SFLOAT_S8_UINT,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .pDepthStencilAttachment = &ref};
    VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &a, .subpassCount = 1, .pSubpasses = &sp};
    VkRenderPass pass;
    /* Version 1: one combined layout for both aspects. */
    assert(vkCreateRenderPass(device, &info, NULL, &pass) == VK_SUCCESS);
    VkImageLayout id, is, rd, rs, fd, fs;
    assert(ps5vk_render_pass_depth_stencil_layouts(pass, 0, 0, &id, &is, &rd, &rs, &fd, &fs));
    assert(id == VK_IMAGE_LAYOUT_UNDEFINED && is == VK_IMAGE_LAYOUT_UNDEFINED);
    assert(rd == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL && rs == rd);
    assert(fd == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL && fs == fd);
    vkDestroyRenderPass(device, pass, NULL);
    /* Version 1 cannot name a separate layout for a combined attachment. */
    ref.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    assert(vkCreateRenderPass(device, &info, NULL, &pass) != VK_SUCCESS);
    ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    a.initialLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    assert(vkCreateRenderPass(device, &info, NULL, &pass) != VK_SUCCESS);

    /* The explicit stencil table (the version-2 structures' plumbing). */
    struct ps5vk_render_pass_stencil_layouts stencil = {
        .initial = {VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL},
        .final = {VK_IMAGE_LAYOUT_GENERAL},
        .reference = {VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL}};
    a.initialLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ref.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    assert(ps5vk_render_pass_create(device, &info, &stencil, NULL, &pass) == VK_SUCCESS);
    assert(ps5vk_render_pass_depth_stencil_layouts(pass, 0, 0, &id, &is, &rd, &rs, &fd, &fs));
    assert(id == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL &&
           is == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL);
    assert(rd == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL &&
           rs == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL);
    assert(fd == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && fs == VK_IMAGE_LAYOUT_GENERAL);
    vkDestroyRenderPass(device, pass, NULL);
    /* Mixed layouts are depth-aspect layouts too. */
    ref.layout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    assert(ps5vk_render_pass_create(device, &info, &stencil, NULL, &pass) == VK_SUCCESS);
    vkDestroyRenderPass(device, pass, NULL);
    ref.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    /* The stencil table never takes a combined or DEPTH_* layout ... */
    const VkImageLayout wrong[] = {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    for (unsigned i = 0; i < sizeof(wrong) / sizeof(wrong[0]); ++i) {
        struct ps5vk_render_pass_stencil_layouts bad = stencil;
        bad.reference[0] = wrong[i];
        assert(ps5vk_render_pass_create(device, &info, &bad, NULL, &pass) != VK_SUCCESS);
        bad = stencil; bad.initial[0] = wrong[i];
        assert(ps5vk_render_pass_create(device, &info, &bad, NULL, &pass) != VK_SUCCESS);
    }
    /* ... a final layout is never UNDEFINED, a LOAD needs contents, and the
     * depth half still cannot name a STENCIL_* layout. */
    struct ps5vk_render_pass_stencil_layouts bad = stencil;
    bad.final[0] = VK_IMAGE_LAYOUT_UNDEFINED;
    assert(ps5vk_render_pass_create(device, &info, &bad, NULL, &pass) != VK_SUCCESS);
    bad = stencil; bad.initial[0] = VK_IMAGE_LAYOUT_UNDEFINED;
    assert(ps5vk_render_pass_create(device, &info, &bad, NULL, &pass) != VK_SUCCESS);
    ref.layout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
    assert(ps5vk_render_pass_create(device, &info, &stencil, NULL, &pass) != VK_SUCCESS);
    /* A depth-only attachment keeps its single-aspect rules even with a
     * table: its reference stays DEPTH_STENCIL_ATTACHMENT or GENERAL. */
    a.format = VK_FORMAT_D32_SFLOAT;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ref.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    assert(ps5vk_render_pass_create(device, &info, &stencil, NULL, &pass) != VK_SUCCESS);
    ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    assert(ps5vk_render_pass_create(device, &info, &stencil, NULL, &pass) == VK_SUCCESS);
    assert(pass->stencil.reference[0] == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    vkDestroyRenderPass(device, pass, NULL);
}

int main(void)
{
    VkInstance instance;
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1;
    VkPhysicalDevice physical;
    assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS);

    /* Format properties follow the witnessed row: the native platform's
     * queries are these two functions. */
    VkFormatProperties props;
    ps5vk_graphics_format_properties(VK_FORMAT_D32_SFLOAT_S8_UINT, &props);
    assert(props.optimalTilingFeatures == (DIAGNOSTIC ?
        (VkFormatFeatureFlags)(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                               VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) : 0u));
    ps5vk_graphics_format_properties(VK_FORMAT_D24_UNORM_S8_UINT, &props);
    assert(!props.optimalTilingFeatures);
    VkImageFormatProperties image_props;
    assert(ps5vk_graphics_image_properties(VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0,
        1u << 22, &image_props) == (DIAGNOSTIC ? VK_SUCCESS : VK_ERROR_FORMAT_NOT_SUPPORTED));
    if (DIAGNOSTIC)
        assert(image_props.maxMipLevels == 1 && image_props.maxArrayLayers == 1 &&
               image_props.sampleCounts == VK_SAMPLE_COUNT_1_BIT);
    assert(ps5vk_graphics_image_properties(VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0,
        1u << 22, &image_props) == VK_ERROR_FORMAT_NOT_SUPPORTED);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    assert(vkCreateDevice(physical, &dci, NULL, &device) == VK_SUCCESS);
    device->graphics_enabled = VK_TRUE;
    device->image_requirements = ps5vk_native_image_requirements;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    assert(vkCreateCommandPool(device, &pci, NULL, &pool) == VK_SUCCESS);

    render_pass_layouts();

    VkImage image; VkDeviceMemory memory;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    /* No transfer destination and no mip chain in either build. */
    assert(make_image(usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, &image, &memory) != VK_SUCCESS);
    assert(make_image(usage, 2, &image, &memory) != VK_SUCCESS);
    assert(make_image(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 1, &image, &memory) != VK_SUCCESS);
    if (!DIAGNOSTIC) {
        assert(make_image(usage, 1, &image, &memory) == VK_ERROR_FORMAT_NOT_SUPPORTED);
        puts("Depth/stencil API: the shipping build publishes no combined format");
        return 0;
    }
    assert(make_image(usage, 1, &image, &memory) == VK_SUCCESS);
    assert(image->layout == VK_IMAGE_LAYOUT_UNDEFINED &&
           image->stencil_layout == VK_IMAGE_LAYOUT_UNDEFINED);

    /* Views: the attachment view names both aspects; a one-aspect view is
     * unsupported (no sampled role), a colour one invalid. */
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT_S8_UINT,
        .subresourceRange = {D | S, 0, 1, 0, 1}};
    VkImageView view;
    assert(vkCreateImageView(device, &vi, NULL, &view) == VK_SUCCESS);
    vkDestroyImageView(device, view, NULL);
    vi.subresourceRange.aspectMask = D;
    assert(vkCreateImageView(device, &vi, NULL, &view) == VK_ERROR_FEATURE_NOT_PRESENT);
    vi.subresourceRange.aspectMask = S;
    assert(vkCreateImageView(device, &vi, NULL, &view) == VK_ERROR_FEATURE_NOT_PRESENT);
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    assert(vkCreateImageView(device, &vi, NULL, &view) != VK_SUCCESS);

    /* Barrier recording: combined and per-aspect forms. */
    assert(record_barrier(image, D | S, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, DSW,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT));
    assert(record_barrier(image, D, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, DSW, TR, LATE, XFER));
    assert(record_barrier(image, S, VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, DSW, TR, LATE, XFER));
    assert(record_barrier(image, D | S, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, DSW,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, LATE,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT));
    /* Refused: layouts that name the other aspect, a colour aspect, a partial
     * mip range, an access in the wrong stage and a transfer destination. */
    assert(!record_barrier(image, S, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, DSW, TR, LATE, XFER));
    assert(!record_barrier(image, D, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL, 0, 0, LATE, XFER));
    assert(!record_barrier(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, TR, LATE, XFER));
    assert(!record_barrier(image, D, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, DSW, TR, XFER, XFER));
    assert(!record_barrier(image, S, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, LATE, XFER));

    /* Copies: one aspect at a time. */
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 64 * 64 * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer; VkDeviceMemory buffer_memory;
    assert(vkCreateBuffer(device, &bi, NULL, &buffer) == VK_SUCCESS);
    VkMemoryRequirements breq;
    vkGetBufferMemoryRequirements(device, buffer, &breq);
    VkMemoryAllocateInfo bmi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = breq.size, .memoryTypeIndex = 0};
    assert(vkAllocateMemory(device, &bmi, NULL, &buffer_memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, buffer, buffer_memory, 0) == VK_SUCCESS);
    assert(record_copy(image, D, buffer));
    assert(record_copy(image, S, buffer));
    assert(!record_copy(image, D | S, buffer));
    assert(!record_copy(image, VK_IMAGE_ASPECT_COLOR_BIT, buffer));

    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, buffer_memory, NULL);
    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    puts("Depth/stencil API: combined image, views, render-pass stencil layouts, per-aspect barriers and copies (host only)");
    return 0;
}
