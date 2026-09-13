#include <ps5vk/ps5vk.h>
#include <ps5vk/ps5vk_present.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void)
{
    /* 1. Verify SDK version macros */
    assert(PS5VK_SDK_VERSION_MAJOR == 1);
    assert(PS5VK_SDK_VERSION_MINOR == 0);

    /* 2. Enumerate physical device - verify it reports native PS5 GFX1013 profile, NOT host */
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .pApplicationInfo = NULL
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkResult res = vkCreateInstance(&ici, NULL, &instance);
    assert(res == VK_SUCCESS);
    assert(instance != VK_NULL_HANDLE);

    uint32_t count = 0;
    res = vkEnumeratePhysicalDevices(instance, &count, NULL);
    assert(res == VK_SUCCESS && count > 0);

    VkPhysicalDevice dev = VK_NULL_HANDLE;
    count = 1;
    res = vkEnumeratePhysicalDevices(instance, &count, &dev);
    assert(res == VK_SUCCESS && dev != VK_NULL_HANDLE);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev, &props);
    printf("Native SDK Consumer: physical device '%s' (API %u.%u, Driver %u)\n",
           props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion),
           props.driverVersion);

    /* Must identify PS5 GFX1013 profile and not the host harness */
    assert(strstr(props.deviceName, "gfx1013") != NULL);
    assert(strstr(props.deviceName, "host") == NULL);
    uint32_t family_count=1;
    VkQueueFamilyProperties family={0};
    vkGetPhysicalDeviceQueueFamilyProperties(dev,&family_count,&family);
    assert(family_count==1);
    assert((family.queueFlags & (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) ==
           (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT));
    /* Force linkage of the public graphics entrypoint without private setup. */
    VkPipeline pipeline=VK_NULL_HANDLE;
    assert(vkCreateGraphicsPipelines(VK_NULL_HANDLE,VK_NULL_HANDLE,0,NULL,NULL,&pipeline)!=VK_SUCCESS);

    /* Force linkage of the newly exposed Vulkan 1.0 bookkeeping entry points. */
    uint32_t layer_count = 0;
    assert(vkEnumerateDeviceLayerProperties(dev, &layer_count, NULL) == VK_SUCCESS);
    assert(layer_count == 0);
    VkDeviceSize committed = 1234;
    vkGetDeviceMemoryCommitment(VK_NULL_HANDLE, VK_NULL_HANDLE, &committed);
    assert(committed == 0);
    VkExtent2D granularity = {99, 99};
    vkGetRenderAreaGranularity(VK_NULL_HANDLE, VK_NULL_HANDLE, &granularity);
    assert(granularity.width == 0 && granularity.height == 0);
    VkSubresourceLayout layout = {.rowPitch = 1234};
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    vkGetImageSubresourceLayout(VK_NULL_HANDLE, VK_NULL_HANDLE, &sub, &layout);
    assert(layout.rowPitch == 0);
    assert(vkResetDescriptorPool(VK_NULL_HANDLE, VK_NULL_HANDLE, 0) != VK_SUCCESS);

    /* 3. Verify public presentation API functions from <ps5vk/ps5vk_present.h> */
    struct ps5vk_present_config pconfig = {
        .width = 1920,
        .height = 1080,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .buffer_count = 2
    };
    ps5vk_present_surface surface = NULL;

    /* Verify parameter validation without open device */
    VkResult pres = ps5vkCreatePresentSurface(VK_NULL_HANDLE, &pconfig, 2, NULL, &surface);
    assert(pres != VK_SUCCESS);

    pres = ps5vkPresentFrame(NULL, 0, 0);
    assert(pres != VK_SUCCESS);

    ps5vkDestroyPresentSurface(NULL);

    vkDestroyInstance(instance, NULL);
    puts("Native SDK consumer contracts: pass (PS5 native runtime, GFX1013 device, presentation API)");
    return 0;
}
