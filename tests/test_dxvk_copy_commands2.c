/* VK_KHR_copy_commands2 as the pinned DXVK 2.6.2 uses it (DXVK262-T10).
 *
 * DXVK reads a render target back to a staging texture with one
 * vkCmdCopyImageToBuffer2 region (measured on its first frame against a host
 * driver: bufferOffset = the staging slice's offset in a shared 4 MiB buffer,
 * bufferRowLength 64, bufferImageHeight 64, mip 0, layer 0, the full 64x64
 * extent; dxvk_context.cpp:3611,3664-3671) and uploads textures with
 * vkCmdCopyBufferToImage2. On this Vulkan 1.0 device those are the KHR names.
 *
 * Pinned here: enumeration, creation and proc lookup follow the platform bit
 * and the registry dependency; every version-2 command records EXACTLY the
 * operations its version-1 twin records for the same regions (the version-1
 * recording is the positive control, compared field by field); a pNext on the
 * info or on a region, a wrong structure type, an empty or oversized region
 * array, and use without the extension all poison the buffer and record
 * nothing. Only platform discovery is mocked. */
#include "vk_internal.h"
#include "vk_command.h"
#include "graphics_formats.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t platform_t09;
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
        .max_allocation = 1u << 24, .queue_flags = VK_QUEUE_COMPUTE_BIT,
        .supported_features_t09 = platform_t09};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU", .heap_size = 1u << 24,
        .allocation_granularity = 1, .buffer_image_granularity = 1};
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

static const char *const COPY2 = VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME;
static VkInstance instance;
static VkDevice device;
static VkCommandPool pool;

static VkInstance make_instance(int features2)
{
    const char *extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = features2 ? 1u : 0u, .ppEnabledExtensionNames = &extension};
    VkInstance i = VK_NULL_HANDLE;
    assert(vkCreateInstance(&info, NULL, &i) == VK_SUCCESS);
    return i;
}
static VkPhysicalDevice physical(VkInstance i)
{
    uint32_t count = 1;
    VkPhysicalDevice p = VK_NULL_HANDLE;
    assert(vkEnumeratePhysicalDevices(i, &count, &p) == VK_SUCCESS && p);
    return p;
}
static int lists(VkPhysicalDevice p)
{
    uint32_t count = 0;
    VkExtensionProperties properties[32];
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS && count <= 32);
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, properties) == VK_SUCCESS);
    for (uint32_t n = 0; n < count; ++n)
        if (!strcmp(properties[n].extensionName, COPY2)) {
            assert(properties[n].specVersion == VK_KHR_COPY_COMMANDS_2_SPEC_VERSION);
            return 1;
        }
    return 0;
}
static VkResult create(VkPhysicalDevice p, int extension, VkDevice *out)
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = extension ? 1u : 0u, .ppEnabledExtensionNames = &COPY2};
    return vkCreateDevice(p, &info, NULL, out);
}

static const char *const names[] = {"vkCmdCopyBuffer2KHR", "vkCmdCopyImage2KHR",
    "vkCmdCopyBufferToImage2KHR", "vkCmdCopyImageToBuffer2KHR", "vkCmdBlitImage2KHR",
    "vkCmdResolveImage2KHR"};

static void negotiation(void)
{
    VkDevice d = VK_NULL_HANDLE;
    platform_t09 = 0;
    VkInstance i = make_instance(1);
    VkPhysicalDevice p = physical(i);
    assert(!lists(p));
    assert(create(p, 1, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);

    platform_t09 = PS5VK_T09_FEATURE_COPY_COMMANDS2;
    i = make_instance(0);
    p = physical(i);
    assert(lists(p));
    /* The registry dependency on a 1.0 instance. */
    assert(create(p, 1, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);
    i = make_instance(1);
    p = physical(i);
    assert(create(p, 1, &d) == VK_SUCCESS && d && d->copy_commands2_extension_enabled);
    for (unsigned n = 0; n < 6; ++n) assert(vkGetDeviceProcAddr(d, names[n]));
    /* Vulkan 1.0 has no core names for them. */
    assert(!vkGetDeviceProcAddr(d, "vkCmdCopyImageToBuffer2"));
    vkDestroyDevice(d, NULL);
    d = VK_NULL_HANDLE;
    assert(create(p, 0, &d) == VK_SUCCESS && d && !d->copy_commands2_extension_enabled);
    for (unsigned n = 0; n < 6; ++n) assert(!vkGetDeviceProcAddr(d, names[n]));
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
}

static VkCommandBuffer begin(void)
{
    VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer c = VK_NULL_HANDLE;
    assert(vkAllocateCommandBuffers(device, &info, &c) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(c, &begin_info) == VK_SUCCESS);
    return c;
}
static VkBuffer make_buffer(VkBufferUsageFlags usage, VkDeviceSize size, VkDeviceMemory *memory)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = usage};
    VkBuffer buffer = VK_NULL_HANDLE;
    assert(vkCreateBuffer(device, &info, NULL, &buffer) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size};
    assert(vkAllocateMemory(device, &allocation, NULL, memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(device, buffer, *memory, 0) == VK_SUCCESS);
    return buffer;
}
/* The two recordings must be the same operations, field by field. */
static void same_operations(VkCommandBuffer a, VkCommandBuffer b)
{
    assert(a->state == PS5VK_RECORDING && b->state == PS5VK_RECORDING);
    assert(a->operation_count == b->operation_count && a->operation_count);
    for (unsigned n = 0; n < a->operation_count; ++n) {
        const struct ps5vk_operation *x = &a->operations[n], *y = &b->operations[n];
        assert(x->type == y->type && x->copy_source == y->copy_source &&
               x->copy_destination == y->copy_destination && x->copy_image == y->copy_image &&
               x->copy_layout == y->copy_layout &&
               !memcmp(&x->copy_region, &y->copy_region, sizeof(x->copy_region)) &&
               !memcmp(&x->buffer_copy, &y->buffer_copy, sizeof(x->buffer_copy)));
    }
}

static void recording(void)
{
    VkDeviceMemory source_memory, destination_memory, image_memory;
    /* The shared staging chunk DXVK suballocates from. */
    VkBuffer source = make_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1u << 20, &source_memory);
    VkBuffer staging = make_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1u << 20, &destination_memory);

    /* vkCmdCopyBuffer2KHR against vkCmdCopyBuffer, two regions. */
    const VkBufferCopy v1_copies[2] = {{0, 64, 256}, {4096, 8192, 128}};
    VkBufferCopy2 v2_copies[2];
    for (unsigned n = 0; n < 2; ++n)
        v2_copies[n] = (VkBufferCopy2){VK_STRUCTURE_TYPE_BUFFER_COPY_2, NULL,
            v1_copies[n].srcOffset, v1_copies[n].dstOffset, v1_copies[n].size};
    VkCopyBufferInfo2 buffer_info = {VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2, NULL, source, staging,
        2, v2_copies};
    VkCommandBuffer control = begin(), converted = begin();
    vkCmdCopyBuffer(control, source, staging, 2, v1_copies);
    vkCmdCopyBuffer2KHR(converted, &buffer_info);
    same_operations(control, converted);
    assert(converted->operation_count == 2 && converted->operations[1].buffer_copy.size == 128);
    vkFreeCommandBuffers(device, pool, 1, &control);
    vkFreeCommandBuffers(device, pool, 1, &converted);

    /* The readback DXVK records: its colour render target, one region. */
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImage image = VK_NULL_HANDLE;
    assert(vkCreateImage(device, &image_info, NULL, &image) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size};
    assert(vkAllocateMemory(device, &allocation, NULL, &image_memory) == VK_SUCCESS);
    assert(vkBindImageMemory(device, image, image_memory, 0) == VK_SUCCESS);
    /* The whole-surface shape the version-1 recorder accepts today; DXVK's
     * sub-allocated offset is checked for identical refusal below and belongs
     * to the readback generalization. */
    const VkBufferImageCopy v1_region = {.bufferOffset = 0, .bufferRowLength = 64,
        .bufferImageHeight = 64, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0}, .imageExtent = {64, 64, 1}};
    VkBufferImageCopy2 v2_region = {VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, NULL,
        v1_region.bufferOffset, v1_region.bufferRowLength, v1_region.bufferImageHeight,
        v1_region.imageSubresource, v1_region.imageOffset, v1_region.imageExtent};
    VkCopyImageToBufferInfo2 readback = {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, NULL,
        image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &v2_region};
    control = begin(); converted = begin();
    vkCmdCopyImageToBuffer(control, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &v1_region);
    vkCmdCopyImageToBuffer2KHR(converted, &readback);
    same_operations(control, converted);
    assert(converted->operations[0].type == PS5VK_COPY_IMAGE_BUFFER &&
           converted->operations[0].copy_region.bufferRowLength == 64);
    vkFreeCommandBuffers(device, pool, 1, &control);
    vkFreeCommandBuffers(device, pool, 1, &converted);
    /* Whatever the version-1 recorder decides for a region, the version-2
     * command decides the same: DXVK's nonzero staging offset. */
    {
        VkBufferImageCopy offset_v1 = v1_region;
        offset_v1.bufferOffset = 16384;
        VkBufferImageCopy2 offset_v2 = v2_region;
        offset_v2.bufferOffset = 16384;
        VkCopyImageToBufferInfo2 offset_info = readback;
        offset_info.pRegions = &offset_v2;
        control = begin(); converted = begin();
        vkCmdCopyImageToBuffer(control, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
            &offset_v1);
        vkCmdCopyImageToBuffer2KHR(converted, &offset_info);
        assert(control->state == converted->state &&
               control->operation_count == converted->operation_count);
        if (control->state == PS5VK_RECORDING) same_operations(control, converted);
        vkFreeCommandBuffers(device, pool, 1, &control);
        vkFreeCommandBuffers(device, pool, 1, &converted);
    }

    /* The upload direction, into the same colour image. */
    VkCopyBufferToImageInfo2 upload = {VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, NULL,
        source, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &v2_region};
    control = begin(); converted = begin();
    vkCmdCopyBufferToImage(control, source, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &v1_region);
    vkCmdCopyBufferToImage2KHR(converted, &upload);
    assert(control->state == converted->state);
    if (control->state == PS5VK_RECORDING) same_operations(control, converted);
    vkFreeCommandBuffers(device, pool, 1, &control);
    vkFreeCommandBuffers(device, pool, 1, &converted);

    /* Resolve: the version-1 command refuses every call on this 1x-only
     * device, and the version-2 one refuses exactly as it does. */
    VkImageResolve2 resolve_region = {VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2, NULL,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        {0, 0, 0}, {64, 64, 1}};
    VkResolveImageInfo2 resolve = {VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2, NULL, image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
        &resolve_region};
    converted = begin();
    vkCmdResolveImage2KHR(converted, &resolve);
    assert(converted->state == PS5VK_INVALID && !converted->operation_count);
    vkFreeCommandBuffers(device, pool, 1, &converted);

    /* Malformed version-2 structures: each poisons and records nothing. */
    VkBufferImageCopy2 chained = v2_region;
    chained.pNext = &chained;
    VkBufferImageCopy2 wrong = v2_region;
    wrong.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
    const VkCopyImageToBufferInfo2 bad[] = {
        {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, &readback, image,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &v2_region},
        {VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, NULL, image,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &v2_region},
        {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, NULL, image,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &chained},
        {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, NULL, image,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &wrong},
        {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, NULL, image,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 0, &v2_region},
        {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, NULL, image,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, NULL},
        {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, NULL, image,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, PS5VK_MAX_OPERATIONS + 1u, &v2_region},
    };
    for (unsigned n = 0; n < sizeof(bad) / sizeof(bad[0]); ++n) {
        VkCommandBuffer c = begin();
        vkCmdCopyImageToBuffer2KHR(c, &bad[n]);
        assert(c->state == PS5VK_INVALID && !c->operation_count);
        vkFreeCommandBuffers(device, pool, 1, &c);
    }
    VkCommandBuffer c = begin();
    vkCmdCopyImageToBuffer2KHR(c, NULL);
    assert(c->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &c);

    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, image_memory, NULL);
    vkDestroyBuffer(device, source, NULL);
    vkDestroyBuffer(device, staging, NULL);
    vkFreeMemory(device, source_memory, NULL);
    vkFreeMemory(device, destination_memory, NULL);
}

static void open_device(int extension)
{
    instance = make_instance(1);
    assert(create(physical(instance), extension, &device) == VK_SUCCESS);
    device->graphics_enabled = VK_TRUE;
    device->image_requirements = ps5vk_native_image_requirements;
    VkCommandPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    assert(vkCreateCommandPool(device, &info, NULL, &pool) == VK_SUCCESS);
}
static void close_device(void)
{
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
}

int main(void)
{
    negotiation();
    platform_t09 = PS5VK_T09_FEATURE_COPY_COMMANDS2;
    open_device(1);
    recording();
    close_device();
    /* Without the extension every command refuses. */
    open_device(0);
    VkBufferCopy2 region = {VK_STRUCTURE_TYPE_BUFFER_COPY_2, NULL, 0, 0, 4};
    VkCopyBufferInfo2 info = {VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2, NULL, VK_NULL_HANDLE,
        VK_NULL_HANDLE, 1, &region};
    VkCommandBuffer c = begin();
    vkCmdCopyBuffer2KHR(c, &info);
    assert(c->state == PS5VK_INVALID);
    vkFreeCommandBuffers(device, pool, 1, &c);
    close_device();
    puts("dxvk copy_commands2 tests passed");
    return 0;
}
