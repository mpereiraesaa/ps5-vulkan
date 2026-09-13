#include <ps5vk/ps5vk.h>
#include <ps5vk/ps5vk_present.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

static void test_public_buffer_transfers(VkPhysicalDevice physical)
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
    };
    VkDevice device = VK_NULL_HANDLE;
    assert(vkCreateDevice(physical, &device_info, NULL, &device) == VK_SUCCESS);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);
    assert(queue != VK_NULL_HANDLE);

    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 256,
        .memoryTypeIndex = 0,
    };
    VkDeviceMemory source_memory = VK_NULL_HANDLE;
    VkDeviceMemory destination_memory = VK_NULL_HANDLE;
    assert(vkAllocateMemory(device, &allocation, NULL, &source_memory) == VK_SUCCESS);
    assert(vkAllocateMemory(device, &allocation, NULL, &destination_memory) == VK_SUCCESS);
    VkBufferCreateInfo source_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 32,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBufferCreateInfo destination_info = source_info;
    destination_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer source = VK_NULL_HANDLE;
    VkBuffer destination = VK_NULL_HANDLE;
    assert(vkCreateBuffer(device, &source_info, NULL, &source) == VK_SUCCESS);
    assert(vkCreateBuffer(device, &destination_info, NULL, &destination) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, source, source_memory, 0) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, destination, destination_memory, 0) == VK_SUCCESS);

    unsigned char *source_bytes = NULL;
    unsigned char *destination_bytes = NULL;
    assert(vkMapMemory(device, source_memory, 0, VK_WHOLE_SIZE, 0,
        (void **)&source_bytes) == VK_SUCCESS);
    assert(vkMapMemory(device, destination_memory, 0, VK_WHOLE_SIZE, 0,
        (void **)&destination_bytes) == VK_SUCCESS);
    for (unsigned j = 0; j < 32; ++j) source_bytes[j] = (unsigned char)(j + 1);
    memset(destination_bytes, 0x5a, 32);
    VkMappedMemoryRange source_range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = source_memory,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    assert(vkFlushMappedMemoryRanges(device, 1, &source_range) == VK_SUCCESS);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    assert(vkCreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command = VK_NULL_HANDLE;
    assert(vkAllocateCommandBuffers(device, &command_info, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    VkBufferCopy copy = {.srcOffset = 1, .dstOffset = 0, .size = 7};
    const uint32_t update = 0x11223344u;
    vkCmdCopyBuffer(command, source, destination, 1, &copy);
    vkCmdUpdateBuffer(command, destination, 8, sizeof(update), &update);
    vkCmdFillBuffer(command, destination, 12, 4, 0xaabbccddu);
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command,
    };
    assert(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(vkQueueWaitIdle(queue) == VK_SUCCESS);

    VkMappedMemoryRange destination_range = source_range;
    destination_range.memory = destination_memory;
    assert(vkInvalidateMappedMemoryRanges(device, 1, &destination_range) == VK_SUCCESS);
    assert(!memcmp(destination_bytes, (unsigned char[]){2, 3, 4, 5, 6, 7, 8}, 7));
    assert(destination_bytes[7] == 0x5a);
    assert(*(uint32_t *)(destination_bytes + 8) == update);
    assert(*(uint32_t *)(destination_bytes + 12) == 0xaabbccddu);
    assert(destination_bytes[16] == 0x5a);

    vkDestroyCommandPool(device, pool, NULL);
    vkUnmapMemory(device, source_memory);
    vkUnmapMemory(device, destination_memory);
    vkDestroyBuffer(device, source, NULL);
    vkDestroyBuffer(device, destination, NULL);
    vkFreeMemory(device, source_memory, NULL);
    vkFreeMemory(device, destination_memory, NULL);
    vkDestroyDevice(device, NULL);
}

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

            /* Verify newly exposed Vulkan 1.0 bookkeeping entry points compile and link */
            uint32_t layer_count = 10;
            assert(vkEnumerateDeviceLayerProperties(dev, &layer_count, NULL) == VK_SUCCESS);
            assert(layer_count == 0);

            VkDeviceSize commitment = 1234;
            vkGetDeviceMemoryCommitment(VK_NULL_HANDLE, VK_NULL_HANDLE, &commitment);
            assert(commitment == 0);

            VkExtent2D granularity = {99, 99};
            vkGetRenderAreaGranularity(VK_NULL_HANDLE, VK_NULL_HANDLE, &granularity);
            assert(granularity.width == 0 && granularity.height == 0);

            VkSubresourceLayout layout = {.rowPitch = 1234};
            VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
            vkGetImageSubresourceLayout(VK_NULL_HANDLE, VK_NULL_HANDLE, &sub, &layout);
            assert(layout.rowPitch == 0);

            assert(vkResetDescriptorPool(VK_NULL_HANDLE, VK_NULL_HANDLE, 0) != VK_SUCCESS);
            test_public_buffer_transfers(dev);
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
