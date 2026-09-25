/* Bounded public-SDK display witness: clear three acquired images, submit,
 * wait for the named fence, present, and retire every Vulkan object. */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The linked DXVK PS5 WSI adapter owns display/mode/plane selection. */
extern VkResult dxvk_witness_create_surface(VkInstance instance,
                                             VkSurfaceKHR *surface);

#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { \
    failed = #call; goto cleanup; } } while (0)
#define REQUIRE(test, reason) do { if (!(test)) { \
    result = VK_ERROR_UNKNOWN; failed = reason; goto cleanup; } } while (0)

static int run_witness(void)
{
    VkResult result = VK_SUCCESS;
    const char *failed = "none";
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkImage images[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView views[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFramebuffer framebuffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkRenderPass pass = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkBool32 pending = VK_FALSE;

    const char *instance_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = instance_extensions};
    TRY(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 1;
    TRY(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical, "one native physical device");
    TRY(dxvk_witness_create_surface(instance, &surface));
    REQUIRE(surface, "adapter-created display surface");
    ps5log_printf(PS5LOG_MARK, "WSI_WITNESS_ADAPTER surface=created");
    VkSurfaceCapabilitiesKHR caps = {0};
    TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    REQUIRE(caps.minImageCount <= 2 && caps.maxImageCount == 2 &&
            caps.currentExtent.width == 1920 && caps.currentExtent.height == 1080 &&
            (caps.supportedUsageFlags &
             (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ==
             (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
            "honest fixed surface capabilities");
    ps5log_printf(PS5LOG_MARK,
        "WSI_WITNESS_START display=1920x1080 refresh=60000 image_count=2 usage=%u",
        (unsigned)(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    const char *swapchain_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &swapchain_extension};
    TRY(vkCreateDevice(physical, &dci, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "graphics queue");
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface,
        .minImageCount = 2, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = {1920, 1080}, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
    };
    TRY(vkCreateSwapchainKHR(device, &ci, NULL, &swapchain));
    count = 2;
    TRY(vkGetSwapchainImagesKHR(device, swapchain, &count, images));
    REQUIRE(count == 2 && images[0] && images[1] && images[0] != images[1],
            "two owned swapchain images");

    VkAttachmentDescription attachment = {
        .format = ci.imageFormat, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference};
    VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass};
    TRY(vkCreateRenderPass(device, &rci, NULL, &pass));
    for (unsigned i = 0; i < 2; ++i) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = ci.imageFormat,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        TRY(vkCreateImageView(device, &vi, NULL, &views[i]));
        VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = pass, .attachmentCount = 1, .pAttachments = &views[i],
            .width = 1920, .height = 1080, .layers = 1};
        TRY(vkCreateFramebuffer(device, &fi, NULL, &framebuffers[i]));
    }
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    TRY(vkCreateCommandPool(device, &pci, NULL, &pool));
    VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    TRY(vkCreateSemaphore(device, &semi, NULL, &acquired));
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fci, NULL, &fence));
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    TRY(vkAllocateCommandBuffers(device, &ai, &commands));

    for (unsigned frame = 0; frame < 3; ++frame) {
        uint32_t slot = UINT32_MAX;
        TRY(vkAcquireNextImageKHR(device, swapchain, UINT64_C(300000000),
                                   acquired, VK_NULL_HANDLE, &slot));
        REQUIRE(slot < 2, "bounded acquire index");
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        TRY(vkBeginCommandBuffer(commands, &bi));
        VkClearValue clear = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}};
        clear.color.float32[frame % 3] = 1.0f;
        VkRenderPassBeginInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = pass, .framebuffer = framebuffers[slot],
            .renderArea = {{0, 0}, {1920, 1080}},
            .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(commands, &ri, VK_SUBPASS_CONTENTS_INLINE);
        VkClearAttachment color_clear = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
            .clearValue = clear};
        VkClearRect full_rect = {
            .rect = {{0, 0}, {1920, 1080}},
            .baseArrayLayer = 0, .layerCount = 1};
        vkCmdClearAttachments(commands, 1, &color_clear, 1, &full_rect);
        vkCmdEndRenderPass(commands);
        TRY(vkEndCommandBuffer(commands));
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired,
            .pWaitDstStageMask = &stage, .commandBufferCount = 1,
            .pCommandBuffers = &commands};
        TRY(vkQueueSubmit(queue, 1, &submit, fence));
        pending = VK_TRUE;
        TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(300000000)));
        pending = VK_FALSE;
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &slot};
        TRY(vkQueuePresentKHR(queue, &pi));
        ps5log_printf(PS5LOG_MARK,
            "WSI_WITNESS_FRAME frame=%u slot=%u color=%u fence=complete present=complete",
            frame, slot, frame % 3);
        TRY(vkResetFences(device, 1, &fence));
        TRY(vkResetCommandBuffer(commands, 0));
    }
cleanup:
    if (pending) {
        ps5log_printf(PS5LOG_ERR,
            "WSI_WITNESS_FAILURE call=%s result=%d retirement=pending",
            failed, (int)result);
        return 1;
    }
    if (commands && pool) vkFreeCommandBuffers(device, pool, 1, &commands);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (acquired) vkDestroySemaphore(device, acquired, NULL);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    for (unsigned i = 0; i < 2; ++i) {
        if (framebuffers[i]) vkDestroyFramebuffer(device, framebuffers[i], NULL);
        if (views[i]) vkDestroyImageView(device, views[i], NULL);
    }
    if (pass) vkDestroyRenderPass(device, pass, NULL);
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, NULL);
    if (device) vkDestroyDevice(device, NULL);
    if (surface) vkDestroySurfaceKHR(instance, surface, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS)
        ps5log_printf(PS5LOG_MARK, "WSI_WITNESS_RETIRED frames=3 resources=clean");
    else
        ps5log_printf(PS5LOG_ERR, "WSI_WITNESS_FAILURE call=%s result=%d",
                      failed, (int)result);
    return result == VK_SUCCESS ? 0 : 1;
}

int main(void)
{
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t boot = (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
    ps5log_config config;
    const char *loaded = NULL, *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&config);
    if (ps5log_load_config(paths, 1, &config, &loaded)) _exit(1);
    config.udp = 0;
    if (ps5log_init(&config, "PPSA99994", "ps5vk", boot)) _exit(1);
    const int failed = run_witness();
    ps5log_close(failed ? "wsi-witness-failed" : "wsi-witness-end");
    for (;;) sleep(1);
}
