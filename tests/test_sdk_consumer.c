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

    puts("Public SDK consumer contracts: pass (clean headers, no private symbols required)");
    return 0;
}
