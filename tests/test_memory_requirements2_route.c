/* The Vulkan 1.0 KHR routes DXVK uses for memory requirements and binding:
 * VK_KHR_get_memory_requirements2, VK_KHR_dedicated_allocation (which needs
 * VK_KHR_get_memory_requirements2 in the pinned registry) and
 * VK_KHR_bind_memory2. Enumeration, device creation and command lookup must
 * follow the platform bits and those dependencies; the commands must report
 * exactly the Vulkan 1.0 requirements and bind exactly as the 1.0 commands
 * do. Only platform discovery, the memory backend and image requirements are
 * mocked. */
#include "vk_internal.h"
#include "vk_image.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t platform_features_t09;
static VkBool32 maintenance4_diagnostic;
static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = calloc(1, size); *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult cache(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, cache, cache};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }
VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
        .max_allocation = 65536, .queue_flags = VK_QUEUE_COMPUTE_BIT,
        .supported_features_t09 = platform_features_t09,
        .maintenance4_diagnostic_on_vulkan_1_0 = maintenance4_diagnostic};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU", .heap_size = 65536,
        .allocation_granularity = 1, .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

static const char *const MR2 = VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME;
static const char *const DEDICATED = VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME;
static const char *const BIND2 = VK_KHR_BIND_MEMORY_2_EXTENSION_NAME;
static const char *const COMMANDS[5] = {
    "vkGetBufferMemoryRequirements2KHR", "vkGetImageMemoryRequirements2KHR",
    "vkGetImageSparseMemoryRequirements2KHR", "vkBindBufferMemory2KHR",
    "vkBindImageMemory2KHR"};

static VkPhysicalDevice physical(VkInstance *i)
{
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&info, NULL, i) == VK_SUCCESS);
    uint32_t count = 1;
    VkPhysicalDevice p = VK_NULL_HANDLE;
    assert(vkEnumeratePhysicalDevices(*i, &count, &p) == VK_SUCCESS && p);
    /* Exercise legacy dependency rules after validating platform discovery. */
    p->platform.properties.apiVersion = VK_API_VERSION_1_0;
    return p;
}
static uint32_t spec_version(const char *name)
{
    if (!strcmp(name, MR2)) return VK_KHR_GET_MEMORY_REQUIREMENTS_2_SPEC_VERSION;
    if (!strcmp(name, DEDICATED)) return VK_KHR_DEDICATED_ALLOCATION_SPEC_VERSION;
    return VK_KHR_BIND_MEMORY_2_SPEC_VERSION;
}
static int lists(VkPhysicalDevice p, const char *name)
{
    uint32_t count = 0;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS);
    VkExtensionProperties properties[20];
    assert(count <= 20);
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, properties) == VK_SUCCESS);
    int found = 0;
    for (uint32_t n = 0; n < count; ++n)
        if (!strcmp(properties[n].extensionName, name)) {
            assert(properties[n].specVersion == spec_version(name));
            ++found;
        }
    assert(found <= 1);
    return found;
}
static VkResult create(VkPhysicalDevice p, const char *const *names, uint32_t count,
                       VkDevice *out)
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &q,
        .enabledExtensionCount = count, .ppEnabledExtensionNames = names};
    *out = VK_NULL_HANDLE;
    return vkCreateDevice(p, &info, NULL, out);
}

/* The shipping state: no bit, nothing listed, nothing accepted, no command. */
static void closed(void)
{
    platform_features_t09 = 0;
    VkInstance i; VkPhysicalDevice p = physical(&i);
    assert(!lists(p, MR2) && !lists(p, DEDICATED) && !lists(p, BIND2));
    VkDevice d;
    const char *const all[3] = {MR2, DEDICATED, BIND2};
    for (uint32_t n = 0; n < 3; ++n)
        assert(create(p, all + n, 1, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    assert(create(p, NULL, 0, &d) == VK_SUCCESS);
    for (size_t n = 0; n < 5; ++n) assert(!vkGetDeviceProcAddr(d, COMMANDS[n]));
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);

    /* The dedicated bit alone does not list or accept the dependent route. */
    platform_features_t09 = PS5VK_T09_FEATURE_DEDICATED_ALLOCATION;
    p = physical(&i);
    assert(!lists(p, MR2) && !lists(p, DEDICATED));
    const char *const pair[2] = {MR2, DEDICATED};
    assert(create(p, pair, 2, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);
}

static void open_route(void)
{
    platform_features_t09 = PS5VK_T09_FEATURE_GET_MEMORY_REQUIREMENTS2 |
        PS5VK_T09_FEATURE_DEDICATED_ALLOCATION | PS5VK_T09_FEATURE_BIND_MEMORY2;
    VkInstance i; VkPhysicalDevice p = physical(&i);
    assert(lists(p, MR2) && lists(p, DEDICATED) && lists(p, BIND2));
    VkDevice d;
    /* VUID-vkCreateDevice-ppEnabledExtensionNames-01387. */
    assert(create(p, &DEDICATED, 1, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    const char *const twice[2] = {MR2, MR2};
    assert(create(p, twice, 2, &d) != VK_SUCCESS && !d);

    /* Each route independently: only its own commands appear. */
    assert(create(p, &MR2, 1, &d) == VK_SUCCESS);
    assert(d->memory_requirements2_extension_enabled &&
           !d->dedicated_allocation_extension_enabled && !d->bind_memory2_extension_enabled);
    for (size_t n = 0; n < 3; ++n) assert(vkGetDeviceProcAddr(d, COMMANDS[n]));
    for (size_t n = 3; n < 5; ++n) assert(!vkGetDeviceProcAddr(d, COMMANDS[n]));
    vkDestroyDevice(d, NULL);
    assert(create(p, &BIND2, 1, &d) == VK_SUCCESS);
    for (size_t n = 0; n < 3; ++n) assert(!vkGetDeviceProcAddr(d, COMMANDS[n]));
    assert(vkGetDeviceProcAddr(d, COMMANDS[3]) == (PFN_vkVoidFunction)vkBindBufferMemory2KHR);
    assert(vkGetDeviceProcAddr(d, COMMANDS[4]) == (PFN_vkVoidFunction)vkBindImageMemory2KHR);
    vkDestroyDevice(d, NULL);

    const char *const all[3] = {MR2, DEDICATED, BIND2};
    assert(create(p, all, 3, &d) == VK_SUCCESS);
    assert(d->memory_requirements2_extension_enabled &&
           d->dedicated_allocation_extension_enabled && d->bind_memory2_extension_enabled);
    assert(vkGetDeviceProcAddr(d, COMMANDS[0]) ==
           (PFN_vkVoidFunction)vkGetBufferMemoryRequirements2KHR);
    assert(vkGetDeviceProcAddr(d, COMMANDS[1]) ==
           (PFN_vkVoidFunction)vkGetImageMemoryRequirements2KHR);
    assert(vkGetDeviceProcAddr(d, COMMANDS[2]) ==
           (PFN_vkVoidFunction)vkGetImageSparseMemoryRequirements2KHR);
    /* The device reports Vulkan 1.0: no core-1.1 name without the suffix. */
    assert(!vkGetDeviceProcAddr(d, "vkGetBufferMemoryRequirements2") &&
           !vkGetDeviceProcAddr(d, "vkGetImageMemoryRequirements2") &&
           !vkGetDeviceProcAddr(d, "vkGetImageSparseMemoryRequirements2") &&
           !vkGetDeviceProcAddr(d, "vkBindBufferMemory2") &&
           !vkGetDeviceProcAddr(d, "vkBindImageMemory2"));

    /* A real device: the 2KHR buffer query equals the 1.0 one. */
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 100, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b;
    assert(vkCreateBuffer(d, &buffer_info, NULL, &b) == VK_SUCCESS);
    VkMemoryRequirements v1;
    vkGetBufferMemoryRequirements(d, b, &v1);
    VkMemoryDedicatedRequirements dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
        .prefersDedicatedAllocation = 7, .requiresDedicatedAllocation = 7};
    VkMemoryRequirements2 v2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
                                .pNext = &dedicated};
    VkBufferMemoryRequirementsInfo2 query = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = b};
    vkGetBufferMemoryRequirements2KHR(d, &query, &v2);
    assert(!memcmp(&v1, &v2.memoryRequirements, sizeof(v1)));
    assert(v1.memoryTypeBits == 1 && v1.size >= 100 && v1.size % v1.alignment == 0);
    assert(dedicated.prefersDedicatedAllocation == VK_FALSE &&
           dedicated.requiresDedicatedAllocation == VK_FALSE);
    vkDestroyBuffer(d, b, NULL);
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
}

/* Hand-built device for the command contracts, with graphics objects so
 * images exist on the host. */
static VkResult image_requirements(VkDevice d, const VkImageCreateInfo *info,
                                   VkMemoryRequirements *out)
{
    (void)d;
    *out = (VkMemoryRequirements){(VkDeviceSize)info->extent.width * info->extent.height * 4u,
                                  1024u, 1u};
    return VK_SUCCESS;
}
static struct VkDevice_T device(void)
{
    return (struct VkDevice_T){.memory = {NULL, alloc_memory, free_memory, cache, cache},
        .buffer_alignment = 256, .noncoherent_atom = 64, .max_allocation = 65536,
        .image_requirements = image_requirements, .graphics_enabled = VK_TRUE,
        .memory_requirements2_extension_enabled = VK_TRUE,
        .dedicated_allocation_extension_enabled = VK_TRUE,
        .bind_memory2_extension_enabled = VK_TRUE};
}
static VkImage image(VkDevice d, uint32_t width)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {width, 16, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage out;
    assert(vkCreateImage(d, &info, NULL, &out) == VK_SUCCESS);
    return out;
}
static VkBuffer buffer(VkDevice d, VkDeviceSize size)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer out;
    assert(vkCreateBuffer(d, &info, NULL, &out) == VK_SUCCESS);
    return out;
}
static VkResult allocate(VkDevice d, VkDeviceSize size, VkImage dedicated_image,
                         VkBuffer dedicated_buffer, VkDeviceMemory *out)
{
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = dedicated_image, .buffer = dedicated_buffer};
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated, .allocationSize = size};
    return vkAllocateMemory(d, &info, NULL, out);
}

static void requirement_queries(void)
{
    struct VkDevice_T d = device();
    VkImage img = image(&d, 16);
    VkMemoryRequirements v1;
    vkGetImageMemoryRequirements(&d, img, &v1);
    assert(v1.size == 1024 && v1.alignment == 1024 && v1.memoryTypeBits == 1);
    VkMemoryDedicatedRequirements dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
        .prefersDedicatedAllocation = 5, .requiresDedicatedAllocation = 5};
    VkMemoryRequirements2 v2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
                                .pNext = &dedicated};
    VkImageMemoryRequirementsInfo2 query = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = img};
    vkGetImageMemoryRequirements2KHR(&d, &query, &v2);
    assert(v2.memoryRequirements.size == 1024 && v2.memoryRequirements.alignment == 1024 &&
           v2.memoryRequirements.memoryTypeBits == 1);
    assert(!dedicated.prefersDedicatedAllocation && !dedicated.requiresDedicatedAllocation);

    /* A disjoint-plane query is not a shape this profile creates: zeroed. */
    VkImagePlaneMemoryRequirementsInfo plane = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
        .planeAspect = VK_IMAGE_ASPECT_PLANE_0_BIT};
    query.pNext = &plane;
    vkGetImageMemoryRequirements2KHR(&d, &query, &v2);
    assert(!v2.memoryRequirements.size && !v2.memoryRequirements.memoryTypeBits);
    query.pNext = NULL;

    /* Without the dedicated extension its output structure is not touched. */
    d.dedicated_allocation_extension_enabled = VK_FALSE;
    dedicated.prefersDedicatedAllocation = 5;
    vkGetImageMemoryRequirements2KHR(&d, &query, &v2);
    assert(v2.memoryRequirements.size == 1024 && dedicated.prefersDedicatedAllocation == 5);
    /* Without get_memory_requirements2 the query answers nothing. */
    d.memory_requirements2_extension_enabled = VK_FALSE;
    vkGetImageMemoryRequirements2KHR(&d, &query, &v2);
    assert(!v2.memoryRequirements.size && !v2.memoryRequirements.alignment &&
           !v2.memoryRequirements.memoryTypeBits);
    d.memory_requirements2_extension_enabled = VK_TRUE;

    /* Wrong structure types are refused and zeroed, never read as valid. */
    query.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
    vkGetImageMemoryRequirements2KHR(&d, &query, &v2);
    assert(!v2.memoryRequirements.size);

    /* Sparse: no requirements for any image. */
    uint32_t count = 9;
    VkImageSparseMemoryRequirementsInfo2 sparse = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2, .image = img};
    vkGetImageSparseMemoryRequirements2KHR(&d, &sparse, &count, NULL);
    assert(count == 0);
    vkDestroyImage(&d, img, NULL);
    assert(!d.images);
}

static void dedicated_allocation(void)
{
    struct VkDevice_T d = device();
    VkImage img = image(&d, 16), other = image(&d, 16);
    VkBuffer b = buffer(&d, 300);
    VkDeviceMemory m;
    /* Both resources, a wrong size, an unknown handle: refused. */
    assert(allocate(&d, 1024, img, b, &m) != VK_SUCCESS && !m);
    assert(allocate(&d, 2048, img, VK_NULL_HANDLE, &m) != VK_SUCCESS && !m);
    assert(allocate(&d, 256, VK_NULL_HANDLE, b, &m) != VK_SUCCESS && !m);
    assert(allocate(&d, 1024, (VkImage)(uintptr_t)&d, VK_NULL_HANDLE, &m) != VK_SUCCESS && !m);
    /* Without the extension the structure is unknown. */
    d.dedicated_allocation_extension_enabled = VK_FALSE;
    assert(allocate(&d, 1024, img, VK_NULL_HANDLE, &m) == VK_ERROR_FEATURE_NOT_PRESENT);
    d.dedicated_allocation_extension_enabled = VK_TRUE;
    /* Both handles null: an ordinary allocation. */
    assert(allocate(&d, 4096, VK_NULL_HANDLE, VK_NULL_HANDLE, &m) == VK_SUCCESS);
    assert(vkBindImageMemory(&d, other, m, 1024) == VK_SUCCESS);
    VkDeviceMemory image_memory, buffer_memory;
    assert(allocate(&d, 1024, img, VK_NULL_HANDLE, &image_memory) == VK_SUCCESS);
    assert(allocate(&d, 512, VK_NULL_HANDLE, b, &buffer_memory) == VK_SUCCESS);
    /* Dedicated memory binds only its own resource, at offset 0. */
    VkBuffer stranger = buffer(&d, 300);
    assert(vkBindBufferMemory(&d, stranger, image_memory, 0) != VK_SUCCESS);
    assert(vkBindBufferMemory(&d, stranger, buffer_memory, 0) != VK_SUCCESS);
    assert(vkBindBufferMemory(&d, b, buffer_memory, 256) != VK_SUCCESS);
    assert(vkBindBufferMemory(&d, b, buffer_memory, 0) == VK_SUCCESS);
    assert(vkBindImageMemory(&d, img, buffer_memory, 0) != VK_SUCCESS);
    assert(vkBindImageMemory(&d, img, image_memory, 0) == VK_SUCCESS);
    /* A bound resource cannot become another allocation's dedicated owner. */
    VkDeviceMemory again;
    assert(allocate(&d, 1024, img, VK_NULL_HANDLE, &again) != VK_SUCCESS && !again);
    /* After its image is gone the memory stays dedicated: nothing else binds. */
    vkDestroyImage(&d, img, NULL);
    VkImage late = image(&d, 16);
    assert(vkBindImageMemory(&d, late, image_memory, 0) != VK_SUCCESS);
    vkDestroyImage(&d, late, NULL);
    vkDestroyImage(&d, other, NULL);
    vkDestroyBuffer(&d, b, NULL); vkDestroyBuffer(&d, stranger, NULL);
    vkFreeMemory(&d, m, NULL); vkFreeMemory(&d, image_memory, NULL);
    vkFreeMemory(&d, buffer_memory, NULL);
    assert(!d.memories && !d.buffers && !d.images);
}

/* Buffer objects are private to src/vk_memory.c: observe a binding through
 * the span the frontend resolves, relative to the allocation's base. */
static long long buffer_offset(VkDevice d, VkBuffer b, const void *base)
{
    void *span; VkDeviceSize size;
    if (ps5vk_buffer_span(d, b, 0, VK_WHOLE_SIZE, &span, &size) != VK_SUCCESS) return -1;
    return (long long)((const unsigned char *)span - (const unsigned char *)base);
}
static void bind_memory2(void)
{
    struct VkDevice_T d = device();
    VkDeviceMemory m;
    assert(allocate(&d, 8192, VK_NULL_HANDLE, VK_NULL_HANDLE, &m) == VK_SUCCESS);
    void *base;
    assert(vkMapMemory(&d, m, 0, VK_WHOLE_SIZE, 0, &base) == VK_SUCCESS);
    VkBuffer a = buffer(&d, 300), b = buffer(&d, 300);
    VkBindBufferMemoryInfo binds[2] = {
        {.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO, .buffer = a, .memory = m,
         .memoryOffset = 0},
        {.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO, .buffer = b, .memory = m,
         .memoryOffset = 100}};
    /* One bad element refuses the call and binds nothing. */
    assert(vkBindBufferMemory2KHR(&d, 2, binds) != VK_SUCCESS);
    assert(buffer_offset(&d, a, base) < 0 && buffer_offset(&d, b, base) < 0);
    /* The same buffer twice in one call is refused. */
    binds[1].buffer = a; binds[1].memoryOffset = 512;
    assert(vkBindBufferMemory2KHR(&d, 2, binds) != VK_SUCCESS && buffer_offset(&d, a, base) < 0);
    binds[1].buffer = b;
    /* A group structure is unknown without VK_KHR_device_group. */
    uint32_t index = 0;
    VkBindBufferMemoryDeviceGroupInfo group = {
        .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO,
        .deviceIndexCount = 1, .pDeviceIndices = &index};
    binds[1].pNext = &group;
    assert(vkBindBufferMemory2KHR(&d, 2, binds) == VK_ERROR_FEATURE_NOT_PRESENT);
    d.device_group_extension_enabled = VK_TRUE;
    index = 1;
    assert(vkBindBufferMemory2KHR(&d, 2, binds) != VK_SUCCESS && buffer_offset(&d, b, base) < 0);
    index = 0;
    assert(vkBindBufferMemory2KHR(&d, 2, binds) == VK_SUCCESS);
    assert(buffer_offset(&d, a, base) == 0 && buffer_offset(&d, b, base) == 512);
    /* Rebinding is refused, as for the 1.0 command. */
    assert(vkBindBufferMemory2KHR(&d, 1, binds) != VK_SUCCESS);
    /* Without the extension the command refuses. */
    d.bind_memory2_extension_enabled = VK_FALSE;
    VkBuffer c = buffer(&d, 16);
    VkBindBufferMemoryInfo late = {.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
        .buffer = c, .memory = m, .memoryOffset = 1024};
    assert(vkBindBufferMemory2KHR(&d, 1, &late) != VK_SUCCESS && buffer_offset(&d, c, base) < 0);
    d.bind_memory2_extension_enabled = VK_TRUE;
    assert(vkBindBufferMemory2KHR(&d, 1, &late) == VK_SUCCESS && buffer_offset(&d, c, base) == 1024);

    VkImage i0 = image(&d, 16), i1 = image(&d, 16);
    VkBindImageMemoryInfo images[2] = {
        {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .image = i0, .memory = m,
         .memoryOffset = 2048},
        {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .image = i1, .memory = m,
         .memoryOffset = 3000}};
    assert(vkBindImageMemory2KHR(&d, 2, images) != VK_SUCCESS && !i0->ever_bound);
    VkRect2D region = {{0, 0}, {16, 16}};
    VkBindImageMemoryDeviceGroupInfo image_group = {
        .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO,
        .splitInstanceBindRegionCount = 1, .pSplitInstanceBindRegions = &region};
    images[1].memoryOffset = 3072; images[1].pNext = &image_group;
    assert(vkBindImageMemory2KHR(&d, 2, images) != VK_SUCCESS && !i0->ever_bound);
    image_group.splitInstanceBindRegionCount = 0;
    VkBindImagePlaneMemoryInfo plane = {
        .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,
        .planeAspect = VK_IMAGE_ASPECT_PLANE_0_BIT};
    images[0].pNext = &plane;
    assert(vkBindImageMemory2KHR(&d, 2, images) == VK_ERROR_FEATURE_NOT_PRESENT);
    images[0].pNext = NULL;
    assert(vkBindImageMemory2KHR(&d, 2, images) == VK_SUCCESS);
    assert(i0->memory == m && i0->offset == 2048 && i1->offset == 3072);
    void *span; VkDeviceSize bytes;
    assert(ps5vk_image_span(&d, i1, &span, &bytes) == VK_SUCCESS && bytes == 1024);
    assert(vkBindBufferMemory2KHR(&d, 0, binds) != VK_SUCCESS);
    vkDestroyImage(&d, i0, NULL); vkDestroyImage(&d, i1, NULL);
    vkDestroyBuffer(&d, a, NULL); vkDestroyBuffer(&d, b, NULL); vkDestroyBuffer(&d, c, NULL);
    vkFreeMemory(&d, m, NULL);
    assert(!d.memories && !d.buffers && !d.images);
}

/* VK_KHR_maintenance4 needs a Vulkan 1.1 device: with the platform bit set
 * the 1.0 device still lists, reports and accepts nothing. */
static void maintenance4_closed_on_1_0(void)
{
    platform_features_t09 = PS5VK_T09_FEATURE_MAINTENANCE4 |
        PS5VK_T09_FEATURE_GET_MEMORY_REQUIREMENTS2;
    VkInstance i; VkPhysicalDevice p = physical(&i);
    uint32_t count = 0;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS);
    VkExtensionProperties properties[21];
    assert(count <= 21);
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, properties) == VK_SUCCESS);
    for (uint32_t n = 0; n < count; ++n)
        assert(strcmp(properties[n].extensionName, VK_KHR_MAINTENANCE_4_EXTENSION_NAME));
    VkPhysicalDeviceMaintenance4Features feature = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, .maintenance4 = 7};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = &feature};
    vkGetPhysicalDeviceFeatures2KHR(p, &features);
    assert(feature.maintenance4 == VK_FALSE);
    VkPhysicalDeviceMaintenance4Properties limit = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                         .pNext = &limit};
    vkGetPhysicalDeviceProperties2KHR(p, &props);
    const VkDeviceSize alignment = props.properties.limits.minStorageBufferOffsetAlignment;
    assert(alignment && limit.maxBufferSize == (65536u & ~(alignment - 1)));
    VkDevice d;
    const char *const m4 = VK_KHR_MAINTENANCE_4_EXTENSION_NAME;
    assert(create(p, &m4, 1, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    feature.maintenance4 = VK_TRUE;
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &feature, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q};
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    feature.maintenance4 = VK_FALSE;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
    assert(!d->maintenance4_extension_enabled && !d->enabled_features_t09);
    assert(!vkGetDeviceProcAddr(d, "vkGetDeviceBufferMemoryRequirementsKHR"));
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
}

/* The DIAGNOSTIC platform switch alone opens the route on the 1.0 device. */
static void maintenance4_diagnostic_route(void)
{
    platform_features_t09 = PS5VK_T09_FEATURE_MAINTENANCE4;
    maintenance4_diagnostic = VK_TRUE;
    VkInstance i; VkPhysicalDevice p = physical(&i);
    VkPhysicalDeviceMaintenance4Features feature = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = &feature};
    vkGetPhysicalDeviceFeatures2KHR(p, &features);
    assert(feature.maintenance4 == VK_TRUE);
    const char *const m4 = VK_KHR_MAINTENANCE_4_EXTENSION_NAME;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &feature, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &m4};
    VkDevice d;
    assert(vkCreateDevice(p, &info, NULL, &d) == VK_SUCCESS);
    assert(d->maintenance4_extension_enabled &&
           d->enabled_features_t09 == PS5VK_T09_FEATURE_MAINTENANCE4);
    assert(vkGetDeviceProcAddr(d, "vkGetDeviceBufferMemoryRequirementsKHR") ==
           (PFN_vkVoidFunction)vkGetDeviceBufferMemoryRequirementsKHR);
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
    maintenance4_diagnostic = VK_FALSE;
}

/* The description queries equal create-then-query and leave no object. */
static void maintenance4_queries(void)
{
    struct VkDevice_T d = device();
    d.maintenance4_extension_enabled = VK_TRUE;
    static const char *const names[3] = {"vkGetDeviceBufferMemoryRequirementsKHR",
        "vkGetDeviceImageMemoryRequirementsKHR", "vkGetDeviceImageSparseMemoryRequirementsKHR"};
    for (size_t n = 0; n < 3; ++n) assert(vkGetDeviceProcAddr(&d, names[n]));
    assert(!vkGetDeviceProcAddr(&d, "vkGetDeviceBufferMemoryRequirements"));

    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 65536 - 300, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b;
    assert(vkCreateBuffer(&d, &buffer_info, NULL, &b) == VK_SUCCESS);
    VkMemoryRequirements v1;
    vkGetBufferMemoryRequirements(&d, b, &v1);
    vkDestroyBuffer(&d, b, NULL);
    VkMemoryDedicatedRequirements dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
        .prefersDedicatedAllocation = 3, .requiresDedicatedAllocation = 3};
    VkMemoryRequirements2 out = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
                                 .pNext = &dedicated};
    VkDeviceBufferMemoryRequirements query = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS, .pCreateInfo = &buffer_info};
    vkGetDeviceBufferMemoryRequirementsKHR(&d, &query, &out);
    assert(!memcmp(&v1, &out.memoryRequirements, sizeof(v1)));
    assert(out.memoryRequirements.size == 65536 - 256 && out.memoryRequirements.alignment == 256 &&
           out.memoryRequirements.memoryTypeBits == 1);
    assert(!dedicated.prefersDedicatedAllocation && !dedicated.requiresDedicatedAllocation);
    assert(!d.buffers);
    /* A usage vkCreateBuffer refuses (transform feedback), or a size above
     * one allocation, has no requirements: memoryTypeBits 0. */
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkGetDeviceBufferMemoryRequirementsKHR(&d, &query, &out);
    assert(!out.memoryRequirements.size && !out.memoryRequirements.memoryTypeBits);
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.size = 65537;
    vkGetDeviceBufferMemoryRequirementsKHR(&d, &query, &out);
    assert(!out.memoryRequirements.memoryTypeBits && !d.buffers);
    buffer_info.size = 64;
    /* Without the extension the query answers nothing. */
    d.maintenance4_extension_enabled = VK_FALSE;
    vkGetDeviceBufferMemoryRequirementsKHR(&d, &query, &out);
    assert(!out.memoryRequirements.size);
    d.maintenance4_extension_enabled = VK_TRUE;
    vkGetDeviceBufferMemoryRequirementsKHR(&d, &query, &out);
    assert(out.memoryRequirements.size == 256);

    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {32, 16, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkDeviceImageMemoryRequirements image_query = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = &image_info};
    vkGetDeviceImageMemoryRequirementsKHR(&d, &image_query, &out);
    assert(out.memoryRequirements.size == 2048 && out.memoryRequirements.alignment == 1024 &&
           out.memoryRequirements.memoryTypeBits == 1);
    assert(!d.images && !d.graphics_objects);
    image_query.planeAspect = VK_IMAGE_ASPECT_PLANE_0_BIT;
    vkGetDeviceImageMemoryRequirementsKHR(&d, &image_query, &out);
    assert(!out.memoryRequirements.size && !out.memoryRequirements.memoryTypeBits);
    image_query.planeAspect = 0;
    image_info.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT;
    vkGetDeviceImageMemoryRequirementsKHR(&d, &image_query, &out);
    assert(!out.memoryRequirements.memoryTypeBits && !d.images);
    uint32_t count = 4;
    vkGetDeviceImageSparseMemoryRequirementsKHR(&d, &image_query, &count, NULL);
    assert(count == 0);
}

int main(void)
{
    closed();
    open_route();
    requirement_queries();
    dedicated_allocation();
    bind_memory2();
    maintenance4_closed_on_1_0();
    maintenance4_queries();
    maintenance4_diagnostic_route();
    puts("memory requirements2 route: get_memory_requirements2, dedicated_allocation and "
         "bind_memory2 follow the registry and the 1.0 contracts; maintenance4 stays closed on the 1.0 device (host only)");
    return 0;
}
