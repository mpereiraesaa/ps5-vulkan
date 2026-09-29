// SPDX-License-Identifier: MIT
/* Win32 Vulkan CPU-map ABI control for the DXVK 2.6.2 Prospero Win path.
 * Load Wine's Vulkan thunk dynamically, then prove that a guest-visible map
 * can be written by the CPU and read back after a bounded GPU buffer copy. */
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { PROBE_BYTES = 1024 };

struct probe {
    HMODULE loader;
    PFN_vkGetInstanceProcAddr gip;
    PFN_vkGetDeviceProcAddr gdp;
    PFN_vkDestroyInstance vkDestroyInstance;
    PFN_vkDestroyDevice vkDestroyDevice;
    PFN_vkDestroyBuffer vkDestroyBuffer;
    PFN_vkFreeMemory vkFreeMemory;
    PFN_vkUnmapMemory vkUnmapMemory;
    PFN_vkDestroyCommandPool vkDestroyCommandPool;
    PFN_vkDestroyFence vkDestroyFence;
    VkInstance instance;
    VkDevice device;
    VkBuffer buffer[2];
    VkDeviceMemory memory[2];
    void *mapped[2];
    VkCommandPool pool;
    VkFence fence;
};

static uint8_t pattern(unsigned i)
{
    return (uint8_t)((i * 73u + 19u) % 251u);
}

static void retire(struct probe *p, int submitted_without_fence)
{
    /* A timed-out GPU job may still use its allocations. Let process teardown
     * reclaim them instead of waiting forever or freeing live GPU memory. */
    if (submitted_without_fence) return;
    if (p->device) {
        if (p->fence && p->vkDestroyFence)
            p->vkDestroyFence(p->device, p->fence, NULL);
        if (p->pool && p->vkDestroyCommandPool)
            p->vkDestroyCommandPool(p->device, p->pool, NULL);
        for (unsigned n = 0; n < 2; ++n) {
            if (p->mapped[n] && p->vkUnmapMemory)
                p->vkUnmapMemory(p->device, p->memory[n]);
            if (p->buffer[n] && p->vkDestroyBuffer)
                p->vkDestroyBuffer(p->device, p->buffer[n], NULL);
            if (p->memory[n] && p->vkFreeMemory)
                p->vkFreeMemory(p->device, p->memory[n], NULL);
        }
        if (p->vkDestroyDevice) p->vkDestroyDevice(p->device, NULL);
    }
    if (p->instance && p->vkDestroyInstance)
        p->vkDestroyInstance(p->instance, NULL);
    if (p->loader) FreeLibrary(p->loader);
}

int main(void)
{
    struct probe p = {0};
    const char *stage = "loader";
    VkResult rc = VK_ERROR_INITIALIZATION_FAILED;
    unsigned mismatches = PROBE_BYTES;
    int submitted_without_fence = 0;
    printf("PS5VK_PE_VKMAP_BEGIN bits=%u bytes=%u\n",
           (unsigned)(sizeof(void *) * 8), (unsigned)PROBE_BYTES);
    fflush(stdout);
    p.loader = LoadLibraryA("vulkan-1.dll");
    if (!p.loader) goto done;
    _Static_assert(sizeof(FARPROC) == sizeof(p.gip), "Win32 function pointer ABI");
    FARPROC entry = GetProcAddress(p.loader, "vkGetInstanceProcAddr");
    memcpy(&p.gip, &entry, sizeof p.gip);
    if (!p.gip) goto done;

#define INSTANCE(name) do { p.name = (PFN_##name)p.gip(p.instance, #name); \
    if (!p.name) { stage = #name; rc = VK_ERROR_INITIALIZATION_FAILED; goto done; } } while (0)
#define DEVICE(name) do { p.name = (PFN_##name)p.gdp(p.device, #name); \
    if (!p.name) { stage = #name; rc = VK_ERROR_INITIALIZATION_FAILED; goto done; } } while (0)
    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)p.gip(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create_instance) { stage = "vkCreateInstance-entry"; goto done; }
    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ps5vk-pe-vkmap-probe";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    stage = "vkCreateInstance";
    rc = create_instance(&ici, NULL, &p.instance);
    if (rc != VK_SUCCESS) goto done;
    INSTANCE(vkDestroyInstance);
    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)p.gip(p.instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queues =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)p.gip(
            p.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)p.gip(
            p.instance, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkCreateDevice create_device =
        (PFN_vkCreateDevice)p.gip(p.instance, "vkCreateDevice");
    p.gdp = (PFN_vkGetDeviceProcAddr)p.gip(p.instance, "vkGetDeviceProcAddr");
    if (!enumerate || !queues || !memory_properties || !create_device || !p.gdp) {
        stage = "instance-entrypoints";
        rc = VK_ERROR_INITIALIZATION_FAILED;
        goto done;
    }
    uint32_t count = 0;
    stage = "vkEnumeratePhysicalDevices";
    rc = enumerate(p.instance, &count, NULL);
    if (rc != VK_SUCCESS) goto done;
    if (count != 1) { rc = VK_ERROR_INITIALIZATION_FAILED; goto done; }
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    rc = enumerate(p.instance, &count, &physical);
    if (rc != VK_SUCCESS) goto done;
    if (!physical) { rc = VK_ERROR_INITIALIZATION_FAILED; goto done; }
    uint32_t family_count = 0;
    queues(physical, &family_count, NULL);
    if (!family_count || family_count > 8) {
        stage = "queue-count"; rc = VK_ERROR_INITIALIZATION_FAILED; goto done;
    }
    VkQueueFamilyProperties families[8] = {{0}};
    queues(physical, &family_count, families);
    uint32_t family = family_count;
    for (uint32_t n = 0; n < family_count; ++n)
        if (families[n].queueCount &&
            (families[n].queueFlags & (VK_QUEUE_TRANSFER_BIT |
                                       VK_QUEUE_GRAPHICS_BIT |
                                       VK_QUEUE_COMPUTE_BIT))) {
            /* Graphics and compute queues support transfers even when the
             * driver does not set the optional TRANSFER capability bit. */
            family = n;
            break;
        }
    if (family == family_count) {
        stage = "transfer-queue"; rc = VK_ERROR_INITIALIZATION_FAILED; goto done;
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    stage = "vkCreateDevice";
    rc = create_device(physical, &dci, NULL, &p.device);
    if (rc != VK_SUCCESS) goto done;
    DEVICE(vkDestroyDevice);
    DEVICE(vkDestroyBuffer);
    DEVICE(vkFreeMemory);
    DEVICE(vkUnmapMemory);
    DEVICE(vkDestroyCommandPool);
    DEVICE(vkDestroyFence);
    PFN_vkGetDeviceQueue get_queue =
        (PFN_vkGetDeviceQueue)p.gdp(p.device, "vkGetDeviceQueue");
    PFN_vkCreateBuffer create_buffer =
        (PFN_vkCreateBuffer)p.gdp(p.device, "vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements requirements =
        (PFN_vkGetBufferMemoryRequirements)p.gdp(
            p.device, "vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory allocate =
        (PFN_vkAllocateMemory)p.gdp(p.device, "vkAllocateMemory");
    PFN_vkBindBufferMemory bind =
        (PFN_vkBindBufferMemory)p.gdp(p.device, "vkBindBufferMemory");
    PFN_vkMapMemory map = (PFN_vkMapMemory)p.gdp(p.device, "vkMapMemory");
    PFN_vkFlushMappedMemoryRanges flush =
        (PFN_vkFlushMappedMemoryRanges)p.gdp(p.device, "vkFlushMappedMemoryRanges");
    PFN_vkInvalidateMappedMemoryRanges invalidate =
        (PFN_vkInvalidateMappedMemoryRanges)p.gdp(
            p.device, "vkInvalidateMappedMemoryRanges");
    PFN_vkCreateCommandPool create_pool =
        (PFN_vkCreateCommandPool)p.gdp(p.device, "vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers allocate_commands =
        (PFN_vkAllocateCommandBuffers)p.gdp(p.device, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer begin =
        (PFN_vkBeginCommandBuffer)p.gdp(p.device, "vkBeginCommandBuffer");
    PFN_vkCmdCopyBuffer copy =
        (PFN_vkCmdCopyBuffer)p.gdp(p.device, "vkCmdCopyBuffer");
    PFN_vkEndCommandBuffer end =
        (PFN_vkEndCommandBuffer)p.gdp(p.device, "vkEndCommandBuffer");
    PFN_vkCreateFence create_fence =
        (PFN_vkCreateFence)p.gdp(p.device, "vkCreateFence");
    PFN_vkQueueSubmit submit =
        (PFN_vkQueueSubmit)p.gdp(p.device, "vkQueueSubmit");
    PFN_vkWaitForFences wait =
        (PFN_vkWaitForFences)p.gdp(p.device, "vkWaitForFences");
    if (!get_queue || !create_buffer || !requirements || !allocate || !bind ||
        !map || !flush || !invalidate || !create_pool || !allocate_commands ||
        !begin || !copy || !end || !create_fence || !submit || !wait) {
        stage = "device-entrypoints";
        rc = VK_ERROR_INITIALIZATION_FAILED;
        goto done;
    }
    VkQueue queue = VK_NULL_HANDLE;
    get_queue(p.device, family, 0, &queue);
    if (!queue) { stage = "vkGetDeviceQueue"; rc = VK_ERROR_INITIALIZATION_FAILED; goto done; }
    VkPhysicalDeviceMemoryProperties memory_types;
    memory_properties(physical, &memory_types);
    for (unsigned n = 0; n < 2; ++n) {
        VkBufferCreateInfo bci = {0};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = PROBE_BYTES;
        bci.usage = n ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        stage = n ? "vkCreateBuffer-dst" : "vkCreateBuffer-src";
        rc = create_buffer(p.device, &bci, NULL, &p.buffer[n]);
        if (rc != VK_SUCCESS) goto done;
        VkMemoryRequirements req;
        requirements(p.device, p.buffer[n], &req);
        uint32_t type = memory_types.memoryTypeCount;
        for (uint32_t i = 0; i < memory_types.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (memory_types.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                type = i;
                break;
            }
        if (type == memory_types.memoryTypeCount)
            for (uint32_t i = 0; i < memory_types.memoryTypeCount; ++i)
                if ((req.memoryTypeBits & (1u << i)) &&
                    (memory_types.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                    type = i;
                    break;
                }
        if (type == memory_types.memoryTypeCount) {
            stage = "host-visible-memory-type"; rc = VK_ERROR_MEMORY_MAP_FAILED; goto done;
        }
        VkMemoryAllocateInfo mai = {0};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        stage = n ? "vkAllocateMemory-dst" : "vkAllocateMemory-src";
        rc = allocate(p.device, &mai, NULL, &p.memory[n]);
        if (rc != VK_SUCCESS) goto done;
        stage = n ? "vkBindBufferMemory-dst" : "vkBindBufferMemory-src";
        rc = bind(p.device, p.buffer[n], p.memory[n], 0);
        if (rc != VK_SUCCESS) goto done;
        stage = n ? "vkMapMemory-dst" : "vkMapMemory-src";
        rc = map(p.device, p.memory[n], 0, VK_WHOLE_SIZE, 0, &p.mapped[n]);
        printf("PS5VK_PE_VKMAP_MAP bits=%u buffer=%u result=%d pointer=0x%llx type=%u\n",
               (unsigned)(sizeof(void *) * 8), n, (int)rc,
               (unsigned long long)(uintptr_t)p.mapped[n], type);
        fflush(stdout);
        if (rc != VK_SUCCESS) goto done;
        if (!p.mapped[n] || (uintptr_t)p.mapped[n] < UINT32_C(0x10000)) {
            rc = VK_ERROR_MEMORY_MAP_FAILED;
            goto done;
        }
    }
    for (unsigned i = 0; i < PROBE_BYTES; ++i)
        ((uint8_t *)p.mapped[0])[i] = pattern(i);
    memset(p.mapped[1], 0xa5, PROBE_BYTES);
    for (unsigned n = 0; n < 2; ++n) {
        VkMappedMemoryRange range = {0};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = p.memory[n];
        range.size = VK_WHOLE_SIZE;
        stage = "vkFlushMappedMemoryRanges";
        rc = flush(p.device, 1, &range);
        if (rc != VK_SUCCESS) goto done;
    }
    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = family;
    stage = "vkCreateCommandPool";
    rc = create_pool(p.device, &pci, NULL, &p.pool);
    if (rc != VK_SUCCESS) goto done;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = p.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    stage = "vkAllocateCommandBuffers";
    rc = allocate_commands(p.device, &cai, &command);
    if (rc != VK_SUCCESS) goto done;
    VkCommandBufferBeginInfo cbi = {0};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    stage = "vkBeginCommandBuffer";
    rc = begin(command, &cbi);
    if (rc != VK_SUCCESS) goto done;
    VkBufferCopy region = {0, 0, PROBE_BYTES};
    copy(command, p.buffer[0], p.buffer[1], 1, &region);
    stage = "vkEndCommandBuffer";
    rc = end(command);
    if (rc != VK_SUCCESS) goto done;
    VkFenceCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    stage = "vkCreateFence";
    rc = create_fence(p.device, &fci, NULL, &p.fence);
    if (rc != VK_SUCCESS) goto done;
    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &command;
    stage = "vkQueueSubmit";
    rc = submit(queue, 1, &si, p.fence);
    if (rc != VK_SUCCESS) goto done;
    submitted_without_fence = 1;
    stage = "vkWaitForFences";
    rc = wait(p.device, 1, &p.fence, VK_TRUE, UINT64_C(300000000));
    if (rc != VK_SUCCESS) goto done;
    submitted_without_fence = 0;
    VkMappedMemoryRange dst_range = {0};
    dst_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    dst_range.memory = p.memory[1];
    dst_range.size = VK_WHOLE_SIZE;
    stage = "vkInvalidateMappedMemoryRanges";
    rc = invalidate(p.device, 1, &dst_range);
    if (rc != VK_SUCCESS) goto done;
    mismatches = 0;
    for (unsigned i = 0; i < PROBE_BYTES; ++i)
        mismatches += ((uint8_t *)p.mapped[1])[i] != pattern(i);
    stage = "gpu-copy-oracle";
    if (mismatches) { rc = VK_ERROR_UNKNOWN; goto done; }
    rc = VK_SUCCESS;
done:
    printf("PS5VK_PE_VKMAP_RESULT bits=%u stage=%s result=%d bytes=%u mismatches=%u retired=%s\n",
           (unsigned)(sizeof(void *) * 8), stage, (int)rc,
           (unsigned)PROBE_BYTES, mismatches,
           submitted_without_fence ? "deferred" : "attempted");
    fflush(stdout);
    retire(&p, submitted_without_fence);
    return rc == VK_SUCCESS && mismatches == 0 ? 0 : 1;
}
