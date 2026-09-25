/* Public Vulkan WSI lifecycle exercised with a controlled display backend.
 * The backend records completed flips; it does not pretend to be a GPU. */
#include "vk_internal.h"
#include "vk_image.h"
#include "vk_queue.h"
#include "wsi_present_backend.h"
#include "physical_device_profile.h"
#include "device_profile_report.h"
#include "graphics_formats.h"
#include "image_layout_state.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ps5vk_wsi_present { VkDevice device; VkImage images[2]; };
static unsigned opens, frames, closes;
static uint32_t last_slot;
static uint64_t last_token;
static int displayed = -1;
static uint64_t fake_now;
static uint64_t clock_ns(void *ctx) { (void)ctx; return fake_now; }
static void pause_ns(void *ctx, uint64_t remaining)
{ (void)ctx; (void)remaining; fake_now += UINT64_C(1000000000); }
static VkResult never_complete(VkDevice d, void *job, uint64_t *completed)
{ (void)d; (void)job; *completed = 0; return VK_SUCCESS; }
static void release_job(VkDevice d, void *job) { (void)d; (void)job; }

VkBool32 ps5vk_wsi_present_available(void) { return VK_TRUE; }
VkResult ps5vk_wsi_present_open(VkDevice device, const VkImage images[2],
                               struct ps5vk_wsi_present **out)
{
    assert(device && images && images[0] && images[1] && images[0] != images[1]);
    struct ps5vk_wsi_present *p = calloc(1, sizeof(*p));
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->device = device;
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
    assert(p);
    if (displayed >= 0) p->images[displayed]->display_busy = VK_FALSE;
    displayed = -1;
    --p->images[0]->pending;
    --p->images[1]->pending;
    ++closes;
    free(p);
    return VK_SUCCESS;
}

static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx;
    *address = calloc(1, (size_t)size);
    *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult cache(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_memory(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, cache, cache};
    return VK_SUCCESS;
}
static void close_memory(struct ps5vk_memory_backend *backend) { (void)backend; }
static VkResult image_requirements(VkDevice d, const VkImageCreateInfo *info,
                                   VkMemoryRequirements *out)
{
    (void)d;
    if (!info || info->format != VK_FORMAT_B8G8R8A8_UNORM ||
        info->extent.width != 1920 || info->extent.height != 1080)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out = (VkMemoryRequirements){.size = 16u << 20,
                                  .alignment = 128u << 10,
                                  .memoryTypeBits = 1};
    return VK_SUCCESS;
}
static void configure(VkDevice d)
{
    d->graphics_enabled = VK_TRUE;
    d->graphics_submit_enabled = VK_TRUE;
    d->image_requirements = image_requirements;
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

static VkPhysicalDevice physical(VkInstance instance)
{
    uint32_t count = 1;
    VkPhysicalDevice p = VK_NULL_HANDLE;
    assert(vkEnumeratePhysicalDevices(instance, &count, &p) == VK_SUCCESS);
    assert(count == 1 && p);
    return p;
}

static VkSurfaceKHR surface(VkInstance instance, VkPhysicalDevice p)
{
    uint32_t count = 1;
    VkDisplayPropertiesKHR display = {0};
    assert(vkGetPhysicalDeviceDisplayPropertiesKHR(p, &count, &display) == VK_SUCCESS);
    assert(count == 1 && display.display);
    count = 1;
    VkDisplayModePropertiesKHR mode = {0};
    assert(vkGetDisplayModePropertiesKHR(p, display.display, &count, &mode) == VK_SUCCESS);
    assert(count == 1 && mode.displayMode);
    VkDisplaySurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = mode.displayMode,
        .planeIndex = 0, .planeStackIndex = 0,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha = 1.0f,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = {1920, 1080},
    };
    VkSurfaceKHR s = VK_NULL_HANDLE;
    assert(vkCreateDisplayPlaneSurfaceKHR(instance, &info, NULL, &s) == VK_SUCCESS && s);
    return s;
}

static VkDevice device(VkPhysicalDevice p, VkQueue *queue)
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    const char *extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
    VkDevice d = VK_NULL_HANDLE;
    assert(vkCreateDevice(p, &ci, NULL, &d) == VK_SUCCESS && d);
    vkGetDeviceQueue(d, 0, 0, queue);
    assert(*queue);
    return d;
}

/* mode 0 imports an acquired image, 1 releases it for presentation, and 2
 * mirrors DXVK's first-frame discard transition before it has contents. */
static VkResult record_present_barrier(VkDevice d, VkImage image, unsigned mode)
{
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0};
    VkCommandPool pool = VK_NULL_HANDLE;
    assert(vkCreateCommandPool(d, &pci, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    VkCommandBuffer commands = VK_NULL_HANDLE;
    assert(vkAllocateCommandBuffers(d, &ai, &commands) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(commands, &bi) == VK_SUCCESS);
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = mode == 1 ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0,
        .dstAccessMask = mode == 1 ? VK_ACCESS_MEMORY_READ_BIT :
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = mode == 1 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                     mode == 2 ? VK_IMAGE_LAYOUT_UNDEFINED :
                                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .newLayout = mode == 1 ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR :
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(commands,
                         mode == 1 ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT :
                                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         mode == 1 ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT :
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);
    VkResult result = vkEndCommandBuffer(commands);
    vkFreeCommandBuffers(d, pool, 1, &commands);
    vkDestroyCommandPool(d, pool, NULL);
    return result;
}

int main(void)
{
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions};
    VkInstance instance = VK_NULL_HANDLE;
    assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS && instance);
    VkPhysicalDevice p = physical(instance);
    VkSurfaceKHR s = surface(instance, p);
    VkBool32 supported = VK_FALSE;
    assert(vkGetPhysicalDeviceSurfaceSupportKHR(p, 0, s, &supported) == VK_SUCCESS);
    uint32_t extension_count = 0;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &extension_count, NULL) ==
           VK_SUCCESS && extension_count <= 32);
    VkExtensionProperties device_extensions[32] = {0};
    uint32_t listed = extension_count;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &listed,
                                               device_extensions) == VK_SUCCESS);
    VkBool32 advertised = VK_FALSE;
    for (uint32_t n = 0; n < listed; ++n)
        if (!strcmp(device_extensions[n].extensionName,
                    VK_KHR_SWAPCHAIN_EXTENSION_NAME)) advertised = VK_TRUE;
    assert(!!supported == !!advertised);
    if (!supported) {
        /* Core may expose the display surface route before the native
         * presentation extension is ready; that state must remain honest. */
        assert(!opens && !frames && !closes);
        vkDestroySurfaceKHR(instance, s, NULL);
        vkDestroyInstance(instance, NULL);
        puts("WSI lifecycle: swapchain route remains unadvertised");
        return 0;
    }
    VkSurfaceCapabilitiesKHR caps = {0};
    assert(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p, s, &caps) == VK_SUCCESS);
    assert(caps.minImageCount <= 2 && caps.maxImageCount == 2);
    assert(caps.currentExtent.width == 1920 && caps.currentExtent.height == 1080);
    assert((caps.supportedUsageFlags & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ==
           (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    VkQueue q = VK_NULL_HANDLE;
    VkDevice d = device(p, &q);
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = s,
        .minImageCount = 2, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = {1920, 1080}, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
    };
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSwapchainCreateInfoKHR rejected = ci;
    rejected.imageUsage = 0;
    assert(vkCreateSwapchainKHR(d, &rejected, NULL, &swapchain) != VK_SUCCESS && !swapchain);
    rejected = ci;
    rejected.minImageCount = 3;
    assert(vkCreateSwapchainKHR(d, &rejected, NULL, &swapchain) != VK_SUCCESS && !swapchain);
    rejected = ci; rejected.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    assert(vkCreateSwapchainKHR(d, &rejected, NULL, &swapchain) != VK_SUCCESS && !swapchain);
    rejected = ci; rejected.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    assert(vkCreateSwapchainKHR(d, &rejected, NULL, &swapchain) != VK_SUCCESS && !swapchain);
    rejected = ci; rejected.imageExtent.width = 1280;
    assert(vkCreateSwapchainKHR(d, &rejected, NULL, &swapchain) != VK_SUCCESS && !swapchain);
    rejected = ci; rejected.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    assert(vkCreateSwapchainKHR(d, &rejected, NULL, &swapchain) != VK_SUCCESS && !swapchain);
    assert(!opens);
    /* Every nonempty subset of the advertised two usage bits must create
     * exactly that image role; unsupported bits remain rejected above. */
    const VkImageUsageFlags subsets[] = {
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };
    for (unsigned n = 0; n < 2; ++n) {
        VkSwapchainCreateInfoKHR subset = ci;
        subset.imageUsage = subsets[n];
        assert(vkCreateSwapchainKHR(d, &subset, NULL, &swapchain) == VK_SUCCESS && swapchain);
        uint32_t subset_count = 2;
        VkImage subset_images[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        assert(vkGetSwapchainImagesKHR(d, swapchain, &subset_count,
                                       subset_images) == VK_SUCCESS);
        assert(subset_count == 2 && subset_images[0] && subset_images[1]);
        assert(subset_images[0]->info.usage == subsets[n] &&
               subset_images[1]->info.usage == subsets[n]);
        vkDestroySwapchainKHR(d, swapchain, NULL);
        swapchain = VK_NULL_HANDLE;
        assert(opens == n + 1 && closes == n + 1);
    }
    assert(vkCreateSwapchainKHR(d, &ci, NULL, &swapchain) == VK_SUCCESS && swapchain);
    assert(opens == 3 && closes == 2);
    uint32_t count = 0;
    assert(vkGetSwapchainImagesKHR(d, swapchain, &count, NULL) == VK_SUCCESS && count == 2);
    VkImage images[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    count = 2;
    assert(vkGetSwapchainImagesKHR(d, swapchain, &count, images) == VK_SUCCESS);
    assert(count == 2 && images[0] && images[1] && images[0] != images[1]);
    assert(images[0]->layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
           images[1]->layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    assert(record_present_barrier(d, images[0], 0) == VK_SUCCESS);
    assert(record_present_barrier(d, images[0], 1) == VK_SUCCESS);
    assert(record_present_barrier(d, images[0], 2) == VK_SUCCESS);
    struct ps5vk_layout_state layouts = {0};
    assert(ps5vk_layout_transition(&layouts, images[0],
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) == VK_SUCCESS);
    assert(ps5vk_layout_transition(&layouts, images[0],
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) == VK_SUCCESS);
    assert(ps5vk_layout_commit(&layouts) == VK_SUCCESS &&
           images[0]->layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VkImageCreateInfo ordinary_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = {1920, 1080, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage ordinary = VK_NULL_HANDLE;
    assert(vkCreateImage(d, &ordinary_info, NULL, &ordinary) == VK_SUCCESS);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 16u << 20, .memoryTypeIndex = 0};
    VkDeviceMemory ordinary_memory = VK_NULL_HANDLE;
    assert(vkAllocateMemory(d, &mai, NULL, &ordinary_memory) == VK_SUCCESS);
    assert(vkBindImageMemory(d, ordinary, ordinary_memory, 0) == VK_SUCCESS);
    assert(record_present_barrier(d, ordinary, 0) != VK_SUCCESS);
    assert(record_present_barrier(d, ordinary, 1) != VK_SUCCESS);
    assert(ps5vk_layout_transition(&layouts, ordinary,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) != VK_SUCCESS && !layouts.count);
    vkDestroyImage(d, ordinary, NULL);
    vkFreeMemory(d, ordinary_memory, NULL);
    uint32_t index = UINT32_MAX;
    assert(vkAcquireNextImageKHR(d, swapchain, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &index)
           != VK_SUCCESS);
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    assert(vkCreateSemaphore(d, &si, NULL, &semaphore) == VK_SUCCESS);
    assert(vkAcquireNextImageKHR(d, swapchain, UINT64_C(100000000), semaphore,
                                 VK_NULL_HANDLE, &index) == VK_SUCCESS && index < 2);
    uint32_t second = UINT32_MAX;
    assert(vkAcquireNextImageKHR(d, swapchain, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &second)
           != VK_SUCCESS);
    /* The companion command-barrier contract proves the transition route;
     * emulate its completed state here so this test isolates WSI ownership. */
    images[index]->layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &semaphore,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &index};
    /* A queued GPU job that never reports completion must return in bounded
     * time without consuming the acquire semaphore or claiming a flip. */
    struct ps5vk_submission *stuck = calloc(1, sizeof(*stuck));
    assert(stuck);
    stuck->waits_consumed = VK_TRUE;
    stuck->count = 1;
    stuck->backend_job = (void *)(uintptr_t)1;
    d->submission = stuck;
    d->submit_backend.poll = never_complete;
    d->submit_backend.release = release_job;
    fake_now = 0;
    assert(vkQueuePresentKHR(q, &pi) == VK_ERROR_DEVICE_LOST);
    assert(fake_now == UINT64_C(3000000000) && !frames &&
           semaphore->signaled && !d->lost);
    d->submission = NULL;
    free(stuck);
    assert(vkQueuePresentKHR(q, &pi) == VK_SUCCESS);
    assert(frames == 1 && last_slot == index && last_token);
    assert(vkAcquireNextImageKHR(d, swapchain, UINT64_C(100000000), semaphore,
                                 VK_NULL_HANDLE, &second) == VK_SUCCESS);
    assert(second < 2 && second != index);
    images[second]->layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pi.pImageIndices = &second;
    assert(vkQueuePresentKHR(q, &pi) == VK_SUCCESS);
    assert(frames == 2 && last_slot == second && last_token == 2);
    assert(vkAcquireNextImageKHR(d, swapchain, UINT64_C(100000000), semaphore,
                                 VK_NULL_HANDLE, &index) == VK_SUCCESS);
    assert(index != second);
    images[index]->layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pi.pImageIndices = &index;
    assert(vkQueuePresentKHR(q, &pi) == VK_SUCCESS);
    assert(frames == 3 && last_token == 3);
    vkDestroySemaphore(d, semaphore, NULL);
    vkDestroySwapchainKHR(d, swapchain, NULL);
    assert(closes == 3);
    vkDestroyDevice(d, NULL);
    vkDestroySurfaceKHR(instance, s, NULL);
    vkDestroyInstance(instance, NULL);
    puts("WSI lifecycle: exact two-image DXVK shape, acquire, present, close");
    return 0;
}
