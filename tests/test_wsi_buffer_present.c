/* A CPU-composed linear BGRA8 frame reaches a WSI swapchain image through the
 * public transfer route: acquire, PRESENT_SRC -> TRANSFER_DST, buffer copy,
 * TRANSFER_DST -> PRESENT_SRC, submit, fence, present.  The display backend
 * is a controlled mock that records flips; the copy itself runs through the
 * queue executor and is checked texel by texel after detiling.  This is the
 * contract a software (GDI-style) compositor needs, not native evidence. */
#include "vk_internal.h"
#include "vk_image.h"
#include "vk_command.h"
#include "vk_image_transfer.h"
#include "wsi_present_backend.h"
#include "physical_device_profile.h"
#include "device_profile_report.h"
#include "graphics_formats.h"
#include "color_detile.h"
#include "image_layout_state.h"
#include "vk_queue.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 1920, HEIGHT = 1080 };

struct ps5vk_wsi_present { VkImage images[2]; };
static unsigned opens, frames, closes;
static uint32_t last_slot;
static uint64_t last_token;
static int displayed = -1;
static uint64_t fake_now;
static uint64_t clock_ns(void *ctx) { (void)ctx; return fake_now; }
static void pause_ns(void *ctx, uint64_t remaining)
{ (void)ctx; (void)remaining; fake_now += UINT64_C(1000000); }

VkBool32 ps5vk_wsi_present_available(void) { return VK_TRUE; }
VkResult ps5vk_wsi_present_open(VkDevice device, const VkImage images[2],
                               struct ps5vk_wsi_present **out)
{
    (void)device;
    struct ps5vk_wsi_present *p = calloc(1, sizeof(*p));
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(p->images, images, sizeof(p->images));
    ++images[0]->pending;
    ++images[1]->pending;
    *out = p;
    ++opens;
    return VK_SUCCESS;
}
VkResult ps5vk_wsi_present_frame(struct ps5vk_wsi_present *p, uint32_t slot,
                                uint64_t token)
{
    assert(p && slot < 2 && token > last_token);
    if (displayed >= 0) p->images[displayed]->display_busy = VK_FALSE;
    p->images[slot]->display_busy = VK_TRUE;
    displayed = (int)slot;
    last_slot = slot;
    last_token = token;
    ++frames;
    return VK_SUCCESS;
}
VkResult ps5vk_wsi_present_close(struct ps5vk_wsi_present *p)
{
    if (displayed >= 0) p->images[displayed]->display_busy = VK_FALSE;
    displayed = -1;
    --p->images[0]->pending;
    --p->images[1]->pending;
    ++closes;
    free(p);
    return VK_SUCCESS;
}

static unsigned flushes;
static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx;
    *address = calloc(1, (size_t)size);
    *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult flush(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; ++flushes; return VK_SUCCESS; }
static VkResult invalidate(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_memory(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, flush, invalidate};
    return VK_SUCCESS;
}
static void close_memory(struct ps5vk_memory_backend *backend) { (void)backend; }
VkResult ps5vk_native_image_requirements(VkDevice d, const VkImageCreateInfo *info,
                                         VkMemoryRequirements *out);
static void configure(VkDevice d)
{
    d->graphics_enabled = VK_TRUE;
    d->graphics_submit_enabled = VK_TRUE;
    d->image_requirements = ps5vk_native_image_requirements;
}
VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p = (struct ps5vk_platform){
        .open = open_memory, .close = close_memory,
        .max_allocation = UINT64_C(268435456),
        .queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
        .format_properties = ps5vk_graphics_format_properties,
        .image_properties = ps5vk_graphics_image_properties,
        .configure = configure,
        .progress = {NULL, NULL, clock_ns, pause_ns},
    };
    ps5vk_device_profile_init(&p->properties, &p->memory_properties,
                              VK_TRUE, VK_TRUE, 0);
    return VK_SUCCESS;
}

/* The native graphics queue owns image barriers: it commits their layouts
 * and signals the exact serial.  This stand-in does only that, so the host
 * executor can run the frontend buffer copy between the two barriers. */
struct barrier_job { uint64_t serial; };
static unsigned native_segments;
static VkResult barrier_prepare(VkDevice d, const struct ps5vk_submission *s, void **out)
{
    (void)d;
    struct ps5vk_layout_state layouts = {0};
    for (uint32_t j = 0; j < s->count; ++j) {
        const VkCommandBuffer c = s->buffers[j];
        const uint32_t first = ps5vk_submission_first_operation(s, j);
        const uint32_t end = first + ps5vk_submission_operation_count(s, j);
        for (uint32_t k = first; k < end; ++k) {
            const struct ps5vk_operation *op = &c->operations[k];
            if (op->type != PS5VK_IMAGE_BARRIER) return VK_ERROR_FEATURE_NOT_PRESENT;
            if (ps5vk_layout_transition(&layouts, op->image_barrier.image,
                    op->image_barrier.oldLayout, op->image_barrier.newLayout) != VK_SUCCESS)
                return VK_ERROR_UNKNOWN;
        }
    }
    struct barrier_job *job = calloc(1, sizeof(*job));
    if (!job) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (ps5vk_layout_commit(&layouts) != VK_SUCCESS) { free(job); return VK_ERROR_UNKNOWN; }
    job->serial = s->serial;
    ++native_segments;
    *out = job;
    return VK_SUCCESS;
}
static VkResult barrier_launch(VkDevice d, void *job) { (void)d; (void)job; return VK_SUCCESS; }
static VkResult barrier_poll(VkDevice d, void *job, uint64_t *completed)
{ (void)d; *completed = ((struct barrier_job *)job)->serial; return VK_SUCCESS; }
static void barrier_release(VkDevice d, void *job) { (void)d; free(job); }

static VkDevice device;
static VkQueue queue;
static VkCommandPool pool;

static VkImageMemoryBarrier barrier(VkImage image, VkImageLayout from, VkImageLayout to,
                                    VkAccessFlags src, VkAccessFlags dst)
{
    return (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src, .dstAccessMask = dst, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
}

/* One frame of the software presenter: import the acquired image for a
 * transfer write, copy the linear rows and release it back to the display
 * layout. */
static void upload_frame(VkImage image, VkBuffer staging, const VkBufferImageCopy *region)
{
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer commands = VK_NULL_HANDLE;
    assert(vkAllocateCommandBuffers(device, &ai, &commands) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    assert(vkBeginCommandBuffer(commands, &bi) == VK_SUCCESS);
    VkImageMemoryBarrier import = barrier(image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &import);
    vkCmdCopyBufferToImage(commands, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, region);
    VkImageMemoryBarrier release = barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &release);
    assert(commands->state == PS5VK_RECORDING);
    assert(vkEndCommandBuffer(commands) == VK_SUCCESS);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence done = VK_NULL_HANDLE;
    assert(vkCreateFence(device, &fi, NULL, &done) == VK_SUCCESS);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &commands};
    assert(vkQueueSubmit(queue, 1, &si, done) == VK_SUCCESS);
    assert(vkWaitForFences(device, 1, &done, VK_TRUE, UINT64_C(1000000000)) == VK_SUCCESS);
    vkDestroyFence(device, done, NULL);
    vkFreeCommandBuffers(device, pool, 1, &commands);
    assert(image->layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

static uint32_t acquire(VkSwapchainKHR swapchain)
{
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence acquired = VK_NULL_HANDLE;
    assert(vkCreateFence(device, &fi, NULL, &acquired) == VK_SUCCESS);
    uint32_t index = UINT32_MAX;
    assert(vkAcquireNextImageKHR(device, swapchain, UINT64_C(100000000), VK_NULL_HANDLE,
                                 acquired, &index) == VK_SUCCESS && index < 2);
    assert(vkWaitForFences(device, 1, &acquired, VK_TRUE, 0) == VK_SUCCESS);
    vkDestroyFence(device, acquired, NULL);
    return index;
}

static void present(VkSwapchainKHR swapchain, uint32_t index)
{
    VkResult result = VK_ERROR_UNKNOWN;
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &index,
        .pResults = &result};
    assert(vkQueuePresentKHR(queue, &pi) == VK_SUCCESS && result == VK_SUCCESS);
}

static uint32_t texel(const void *mapped, uint32_t x, uint32_t y)
{
    uint32_t value;
    memcpy(&value, (const uint8_t *)mapped + ps5vk_color_64k_rx_offset(4, x, y, WIDTH), 4);
    return value;
}

static uint32_t pattern(uint32_t x, uint32_t y)
{
    return 0xff000000u | ((y * 7u + x * 13u) & 0xffffffu);
}

int main(void)
{
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions};
    VkInstance instance = VK_NULL_HANDLE;
    assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS && physical);
    count = 1;
    VkDisplayPropertiesKHR display = {0};
    assert(vkGetPhysicalDeviceDisplayPropertiesKHR(physical, &count, &display) == VK_SUCCESS);
    count = 1;
    VkDisplayModePropertiesKHR mode = {0};
    assert(vkGetDisplayModePropertiesKHR(physical, display.display, &count, &mode) == VK_SUCCESS);
    VkDisplaySurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = mode.displayMode, .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha = 1.0f, .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = {WIDTH, HEIGHT}};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    assert(vkCreateDisplayPlaneSurfaceKHR(instance, &sci, NULL, &surface) == VK_SUCCESS);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    const char *swapchain_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &swapchain_extension};
    assert(vkCreateDevice(physical, &dci, NULL, &device) == VK_SUCCESS);
    vkGetDeviceQueue(device, 0, 0, &queue);
    device->submit_backend = (struct ps5vk_queue_backend){barrier_prepare, barrier_launch,
                                                          barrier_poll, barrier_release};
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    assert(vkCreateCommandPool(device, &pci, NULL, &pool) == VK_SUCCESS);

    /* A software presenter needs only the transfer-destination role. */
    VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface, .minImageCount = 2, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = {WIDTH, HEIGHT}, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    assert(vkCreateSwapchainKHR(device, &ci, NULL, &swapchain) == VK_SUCCESS && opens == 1);
    VkImage images[2];
    count = 2;
    assert(vkGetSwapchainImagesKHR(device, swapchain, &count, images) == VK_SUCCESS);
    void *image_maps[2];
    for (unsigned n = 0; n < 2; ++n) {
        assert(ps5vk_bgra8_transfer_target(images[n]));
        VkDeviceSize bytes = 0;
        assert(ps5vk_image_span(device, images[n], &image_maps[n], &bytes) == VK_SUCCESS);
        assert(bytes >= ps5vk_color_64k_rx_surface_size(4, WIDTH, HEIGHT));
    }

    /* Host-visible, non-coherent staging: the producer writes, then flushes. */
    const VkDeviceSize staging_bytes = (VkDeviceSize)WIDTH * HEIGHT * 4u;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = staging_bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer staging = VK_NULL_HANDLE;
    assert(vkCreateBuffer(device, &bci, NULL, &staging) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, staging, &requirements);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = 0};
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    assert(vkAllocateMemory(device, &mai, NULL, &staging_memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, staging, staging_memory, 0) == VK_SUCCESS);
    uint8_t *linear = NULL;
    assert(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, (void **)&linear) ==
           VK_SUCCESS);
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = staging_memory, .offset = 0, .size = VK_WHOLE_SIZE};

    /* Frame 1: a whole composed 1920x1080 frame, rows top-down. */
    for (uint32_t y = 0; y < HEIGHT; ++y)
        for (uint32_t x = 0; x < WIDTH; ++x) {
            const uint32_t value = pattern(x, y);
            memcpy(linear + ((size_t)y * WIDTH + x) * 4u, &value, 4);
        }
    const unsigned before = flushes;
    assert(vkFlushMappedMemoryRanges(device, 1, &range) == VK_SUCCESS && flushes > before);
    const uint32_t first = acquire(swapchain);
    VkBufferImageCopy whole = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {WIDTH, HEIGHT, 1}};
    upload_frame(images[first], staging, &whole);
    for (uint32_t y = 0; y < HEIGHT; ++y)
        for (uint32_t x = 0; x < WIDTH; ++x)
            assert(texel(image_maps[first], x, y) == pattern(x, y));
    present(swapchain, first);
    assert(frames == 1 && last_slot == first && last_token == 1);
    assert(images[first]->display_busy);

    /* Frame 2: the producer's own row stride is kept (bufferRowLength) and
     * the window is placed by imageOffset; texels outside the region are not
     * written.  A background clear is native GPU work, not this route. */
    enum { SURFACE_W = 600, SURFACE_H = 416, STRIDE_PIXELS = 608, LEFT = 660, TOP = 332 };
    memset(linear, 0, (size_t)staging_bytes);
    for (uint32_t y = 0; y < SURFACE_H; ++y)
        for (uint32_t x = 0; x < SURFACE_W; ++x) {
            const uint32_t value = 0xff000000u | (y << 12) | x;
            memcpy(linear + ((size_t)y * STRIDE_PIXELS + x) * 4u, &value, 4);
        }
    assert(vkFlushMappedMemoryRanges(device, 1, &range) == VK_SUCCESS);
    const uint32_t second = acquire(swapchain);
    assert(second != first);
    VkBufferImageCopy window = {.bufferRowLength = STRIDE_PIXELS,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {LEFT, TOP, 0}, .imageExtent = {SURFACE_W, SURFACE_H, 1}};
    upload_frame(images[second], staging, &window);
    for (uint32_t y = 0; y < HEIGHT; ++y)
        for (uint32_t x = 0; x < WIDTH; ++x) {
            const int inside = x >= LEFT && x < LEFT + SURFACE_W &&
                               y >= TOP && y < TOP + SURFACE_H;
            const uint32_t expected = inside ?
                0xff000000u | ((y - TOP) << 12) | (x - LEFT) : 0u;
            assert(texel(image_maps[second], x, y) == expected);
        }
    /* The displayed image is untouched while the other one is written. */
    assert(texel(image_maps[first], 0, 0) == pattern(0, 0));
    present(swapchain, second);
    assert(frames == 2 && last_slot == second && last_token == 2);
    /* Each frame ran exactly two native barrier segments around the copy. */
    assert(native_segments == 4);
    assert(!images[first]->display_busy && images[second]->display_busy);

    /* The next acquire returns the image that is no longer displayed. */
    assert(acquire(swapchain) == first);

    vkUnmapMemory(device, staging_memory);
    vkDestroyBuffer(device, staging, NULL);
    vkFreeMemory(device, staging_memory, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroySwapchainKHR(device, swapchain, NULL);
    assert(closes == 1 && !device->lifetime_errors);
    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);
    puts("WSI buffer present: linear BGRA8 frames reach alternating swapchain images");
    return 0;
}
