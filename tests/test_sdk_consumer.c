#include <ps5vk/ps5vk.h>
#include <ps5vk/ps5vk_present.h>
#include <stdio.h>
#include <assert.h>

int main(void)
{
    /* Verify public SDK header macros */
    assert(PS5VK_SDK_VERSION_MAJOR == 1);
    assert(PS5VK_SDK_VERSION_MINOR == 0);

    /* Verify Vulkan API types and symbols compile cleanly without private vk_internal.h */
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .pApplicationInfo = NULL
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkResult res = vkCreateInstance(&ici, NULL, &instance);
    /* In this host harness or mock, check result or verify signature availability */
    if (res == VK_SUCCESS) {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, NULL);
        if (count > 0) {
            VkPhysicalDevice dev;
            count = 1;
            vkEnumeratePhysicalDevices(instance, &count, &dev);
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(dev, &props);
            printf("SDK Consumer: found device '%s' (API %u.%u)\n",
                   props.deviceName,
                   VK_VERSION_MAJOR(props.apiVersion),
                   VK_VERSION_MINOR(props.apiVersion));
        }
        vkDestroyInstance(instance, NULL);
    }

    /* Verify presentation functions compile and link cleanly */
    struct ps5vk_present_config pcfg = {
        .width = 1920,
        .height = 1080,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .buffer_count = 2
    };
    ps5vk_present_surface surf = NULL;
    assert(ps5vkCreatePresentSurface(VK_NULL_HANDLE, &pcfg, 2, NULL, &surf) != VK_SUCCESS);
    assert(ps5vkPresentFrame(NULL, 0, 0) != VK_SUCCESS);
    ps5vkDestroyPresentSurface(NULL);

    puts("Public SDK consumer contracts: pass (clean headers, no private symbols required)");
    return 0;
}
