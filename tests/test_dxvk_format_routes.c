/* The format routes the first DXVK 2.6.2 D3D11 workload depends on, with the
 * exact shapes the pinned DXVK uses:
 *
 *   - DxvkAdapter::getFormatFeatures (dxvk_adapter.cpp:80-90) reads format
 *     support ONLY from VkFormatProperties3 chained under
 *     vkGetPhysicalDeviceFormatProperties2 (VK_KHR_format_feature_flags2);
 *   - D3D11CommonTexture (d3d11_texture.cpp:79-90, 506-530) queries a
 *     DXGI_FORMAT_R8G8B8A8_UNORM render target as R8G8B8A8_UNORM, 2D,
 *     OPTIMAL, usage TRANSFER_SRC|TRANSFER_DST|COLOR_ATTACHMENT (0x13), flags
 *     MUTABLE_FORMAT, with no format list, and then creates it with
 *     VkImageFormatListCreateInfo {UNORM, SRGB} (dxvk_image.cpp:160-187);
 *   - the render-target view is UNORM (d3d11_texture.cpp:339-358 checks the
 *     view format's COLOR_ATTACHMENT feature).
 *
 * Both extensions stay unadvertised until the platform sets their bits; only
 * platform discovery and the memory backend are mocked. */
#include "vk_image.h"
#include "graphics_formats.h"
#include "device_profile_report.h"
#include "texture_descriptor.h"
#include "texture_layout.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint32_t platform_features_t09;
static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = aligned_alloc(256, (size + 255) & ~(VkDeviceSize)255);
    *backing = *address;
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
    /* The console's graphics profile tables, as tools/dump_device_reporting.c
     * installs them on the host. */
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
        .max_allocation = ps5vk_device_profile_heap_bytes(VK_TRUE),
        .queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
        .format_properties = ps5vk_graphics_format_properties,
        .image_properties = ps5vk_graphics_image_properties,
        .supported_features_t09 = platform_features_t09};
    ps5vk_device_profile_init(&p->properties, &p->memory_properties, VK_TRUE, VK_TRUE, 0);
    return VK_SUCCESS;
}

static const VkImageUsageFlags RT_USAGE = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
static const VkFormat RGBA8_FAMILY[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};

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
static uint32_t listed(VkPhysicalDevice p, const char *name)
{
    uint32_t count = 0;
    VkExtensionProperties properties[20];
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS);
    assert(count <= 20);
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, properties) == VK_SUCCESS);
    for (uint32_t n = 0; n < count; ++n)
        if (!strcmp(properties[n].extensionName, name)) return properties[n].specVersion;
    return 0;
}
static VkResult create_device(VkPhysicalDevice p, const char *const *names, uint32_t count,
                              VkDevice *out)
{
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = count, .ppEnabledExtensionNames = names};
    *out = VK_NULL_HANDLE;
    return vkCreateDevice(p, &info, NULL, out);
}
static VkResult dxvk_limits(VkPhysicalDevice p, VkFormat format, VkImageCreateFlags flags,
                            const void *next, VkImageFormatProperties *out)
{
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = next,
        .format = format, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = RT_USAGE, .flags = flags};
    VkImageFormatProperties2 properties = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    VkResult result = vkGetPhysicalDeviceImageFormatProperties2KHR(p, &info, &properties);
    *out = properties.imageFormatProperties;
    return result;
}

/* Physical-device answers and device creation, with and without the bits. */
static void physical_routes(void)
{
    const char *const FF2 = VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME;
    const char *const IFL = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME;
    const VkImageFormatListCreateInfo family = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = 2, .pViewFormats = RGBA8_FAMILY};
    const VkFormat uint_pair[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT};
    const VkImageFormatListCreateInfo uint_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = 2, .pViewFormats = uint_pair};
    VkImageFormatProperties immutable, limits;

    /* Shipping state: nothing reported, nothing accepted, structures untouched. */
    platform_features_t09 = 0;
    VkInstance i = make_instance(1);
    VkPhysicalDevice p = physical(i);
    assert(!listed(p, FF2) && !listed(p, IFL));
    VkFormatProperties3 sentinel = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
        .linearTilingFeatures = 0xabcdu, .optimalTilingFeatures = 0xabcdu,
        .bufferFeatures = 0xabcdu};
    VkFormatProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
                                       .pNext = &sentinel};
    vkGetPhysicalDeviceFormatProperties2KHR(p, VK_FORMAT_R8G8B8A8_UNORM, &properties2);
    assert(properties2.formatProperties.optimalTilingFeatures &
           VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    assert(sentinel.optimalTilingFeatures == 0xabcdu && sentinel.bufferFeatures == 0xabcdu);
    assert(dxvk_limits(p, VK_FORMAT_R8G8B8A8_UNORM, 0, NULL, &immutable) == VK_SUCCESS);
    /* The measured DXVK refusal: MUTABLE on the 0x13 render target. */
    assert(dxvk_limits(p, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                       NULL, &limits) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    VkDevice d;
    assert(create_device(p, &FF2, 1, &d) == VK_ERROR_EXTENSION_NOT_PRESENT);
    assert(create_device(p, &IFL, 1, &d) == VK_ERROR_EXTENSION_NOT_PRESENT);
    vkDestroyInstance(i, NULL);

    platform_features_t09 = PS5VK_T09_FEATURE_FORMAT_FEATURE_FLAGS2 |
                            PS5VK_T09_FEATURE_IMAGE_FORMAT_LIST;
    i = make_instance(1);
    p = physical(i);
    assert(listed(p, FF2) == VK_KHR_FORMAT_FEATURE_FLAGS_2_SPEC_VERSION);
    assert(listed(p, IFL) == VK_KHR_IMAGE_FORMAT_LIST_SPEC_VERSION);

    /* VkFormatProperties3 is the 32-bit answer zero-extended, for every row of
     * the capability table and for formats outside it (DXVK probes
     * D24_UNORM_S8_UINT and A8_UNORM_KHR at DXGI init and must see zero). No
     * 64-bit-only bit appears anywhere. */
    const VkFormatFeatureFlags2 only64 = ~(VkFormatFeatureFlags2)UINT32_MAX;
    for (unsigned n = 0; n <= ps5vk_texture_format_count() + 2; ++n) {
        const VkFormat format = n < ps5vk_texture_format_count() ?
            ps5vk_texture_format_at(n)->format :
            n == ps5vk_texture_format_count() ? VK_FORMAT_D24_UNORM_S8_UINT :
            n == ps5vk_texture_format_count() + 1 ? VK_FORMAT_A8_UNORM_KHR :
            VK_FORMAT_B8G8R8A8_SRGB;
        VkFormatProperties3 p3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
            .linearTilingFeatures = only64, .optimalTilingFeatures = only64,
            .bufferFeatures = only64};
        VkFormatProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &p3};
        VkFormatProperties p1;
        vkGetPhysicalDeviceFormatProperties2KHR(p, format, &p2);
        vkGetPhysicalDeviceFormatProperties(p, format, &p1);
        assert(!memcmp(&p1, &p2.formatProperties, sizeof(p1)));
        assert(p3.linearTilingFeatures == p1.linearTilingFeatures);
        assert(p3.optimalTilingFeatures == p1.optimalTilingFeatures);
        assert(p3.bufferFeatures == p1.bufferFeatures);
        assert(!((p3.linearTilingFeatures | p3.optimalTilingFeatures | p3.bufferFeatures) &
                 (only64 | VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT |
                  VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT)));
        if (format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_A8_UNORM_KHR)
            assert(!p3.linearTilingFeatures && !p3.optimalTilingFeatures && !p3.bufferFeatures);
    }
    /* The exact bits DXVK's render-target path reads for R8G8B8A8_UNORM:
     * RTV (COLOR_ATTACHMENT), clear (TRANSFER_DST), CopyResource
     * (TRANSFER_SRC). SRGB has no colour-attachment role. */
    {
        VkFormatProperties3 unorm = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &unorm};
        vkGetPhysicalDeviceFormatProperties2KHR(p, VK_FORMAT_R8G8B8A8_UNORM, &p2);
        const VkFormatFeatureFlags2 rt = VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
        assert((unorm.optimalTilingFeatures & rt) == rt);
        VkFormatProperties3 srgb = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        p2.pNext = &srgb;
        vkGetPhysicalDeviceFormatProperties2KHR(p, VK_FORMAT_R8G8B8A8_SRGB, &p2);
        assert(srgb.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT);
        assert(!(srgb.optimalTilingFeatures & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT));
    }

    /* DXVK's exact query now succeeds with the immutable twin's limits. */
    assert(dxvk_limits(p, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                       NULL, &limits) == VK_SUCCESS);
    assert(!memcmp(&limits, &immutable, sizeof(limits)));
    assert(dxvk_limits(p, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                       &family, &limits) == VK_SUCCESS);
    assert(!memcmp(&limits, &immutable, sizeof(limits)));
    /* Refused: an unimplemented pair, a two-entry list without MUTABLE, a
     * format with no reinterpretation, linear tiling, the 1.0 query too. */
    assert(dxvk_limits(p, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                       &uint_list, &limits) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    assert(!limits.maxExtent.width);
    assert(dxvk_limits(p, VK_FORMAT_R8G8B8A8_UNORM, 0, &family, &limits) ==
           VK_ERROR_FORMAT_NOT_SUPPORTED);
    assert(dxvk_limits(p, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                       NULL, &limits) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    assert(vkGetPhysicalDeviceImageFormatProperties(p, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, &limits) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    assert(vkGetPhysicalDeviceImageFormatProperties(p, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, RT_USAGE,
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, &limits) == VK_SUCCESS);
    /* A sampled SRGB texture may be mutable too (D3D11 UNORM_SRGB family). */
    assert(vkGetPhysicalDeviceImageFormatProperties(p, VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, &limits) == VK_SUCCESS);

    /* Device creation: format_feature_flags2 needs the Features2 instance
     * route; image_format_list has no dependency. */
    const char *const both[2] = {FF2, IFL};
    assert(create_device(p, both, 2, &d) == VK_SUCCESS);
    assert(d->image_format_list_extension_enabled && d->mutable_format_views);
    vkDestroyDevice(d, NULL);
    assert(create_device(p, NULL, 0, &d) == VK_SUCCESS);
    assert(!d->image_format_list_extension_enabled && d->mutable_format_views);
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
    i = make_instance(0);
    p = physical(i);
    assert(create_device(p, &FF2, 1, &d) == VK_ERROR_EXTENSION_NOT_PRESENT);
    assert(create_device(p, &IFL, 1, &d) == VK_SUCCESS);
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
    platform_features_t09 = 0;
}

/* Object routes on a hand-built device with the native image backend. Every
 * created image and memory object is recorded so the test frees them all. */
static int requirement_calls;
static VkImage created_images[32];
static VkDeviceMemory created_memories[32];
static unsigned image_count, memory_count;
static VkResult checked_requirements(VkDevice d, const VkImageCreateInfo *info,
                                     VkMemoryRequirements *out)
{
    /* The backend sees the storage shape only: no mutable flag, no chain. */
    assert(!info->pNext && !(info->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT));
    ++requirement_calls;
    return ps5vk_native_image_requirements(d, info, out);
}
static int same_requirements(const VkMemoryRequirements *a, const VkMemoryRequirements *b)
{
    return a->size == b->size && a->alignment == b->alignment &&
        a->memoryTypeBits == b->memoryTypeBits;
}
static VkImage image_with(VkDevice d, VkFormat format, VkImageUsageFlags usage,
                          VkImageCreateFlags flags, const void *next, VkResult expect)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = next,
        .flags = flags, .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkImage image = (VkImage)(uintptr_t)1;
    assert(vkCreateImage(d, &info, NULL, &image) == expect);
    assert((expect == VK_SUCCESS) == (image != VK_NULL_HANDLE));
    if (image) { assert(image_count < 32); created_images[image_count++] = image; }
    return image;
}
static void bind(VkDevice d, VkImage image)
{
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image->requirements.size};
    VkDeviceMemory memory;
    assert(vkAllocateMemory(d, &ai, NULL, &memory) == VK_SUCCESS);
    assert(vkBindImageMemory(d, image, memory, 0) == VK_SUCCESS);
    assert(memory_count < 32);
    created_memories[memory_count++] = memory;
}
static VkResult view_with(VkDevice d, VkImage image, VkFormat format,
                          VkImageUsageFlags narrowed, VkImageView *out)
{
    VkImageViewUsageCreateInfo usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                        .usage = narrowed};
    VkImageViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = narrowed ? &usage : NULL, .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(d, &info, NULL, out);
}

static void object_routes(void)
{
    struct VkDevice_T d = {.graphics_enabled = VK_TRUE,
        .image_requirements = checked_requirements,
        .memory = {.allocate = alloc_memory, .release = free_memory, .flush = cache,
                   .invalidate = cache},
        .max_allocation = 1u << 26, .noncoherent_atom = 64, .buffer_alignment = 256,
        .maintenance2_extension_enabled = VK_TRUE};
    const VkImageFormatListCreateInfo family = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = 2, .pViewFormats = RGBA8_FAMILY};
    const VkImageCreateFlags MUTABLE = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

    /* A device whose platform does not serve the route refuses the flag and
     * the list before the backend is asked anything. */
    image_with(&d, VK_FORMAT_R8G8B8A8_UNORM, RT_USAGE, MUTABLE, NULL,
               VK_ERROR_FEATURE_NOT_PRESENT);
    d.mutable_format_views = VK_TRUE;
    image_with(&d, VK_FORMAT_R8G8B8A8_UNORM, RT_USAGE, MUTABLE, &family,
               VK_ERROR_FEATURE_NOT_PRESENT);
    assert(!requirement_calls);
    d.image_format_list_extension_enabled = VK_TRUE;

    /* DXVK's render target: MUTABLE + {UNORM, SRGB}, identical storage. */
    VkImage plain = image_with(&d, VK_FORMAT_R8G8B8A8_UNORM, RT_USAGE, 0, NULL, VK_SUCCESS);
    VkImage rt = image_with(&d, VK_FORMAT_R8G8B8A8_UNORM, RT_USAGE, MUTABLE, &family,
                            VK_SUCCESS);
    assert(requirement_calls == 2);
    assert(!rt->info.flags && !rt->info.pNext && rt->mutable_format);
    assert(same_requirements(&rt->requirements, &plain->requirements));
    assert(rt->view_format_count == 2 && rt->view_formats[0] == VK_FORMAT_R8G8B8A8_UNORM &&
           rt->view_formats[1] == VK_FORMAT_R8G8B8A8_SRGB);
    assert(ps5vk_colour_readback_image(rt) == ps5vk_colour_readback_image(plain));
    bind(&d, rt);
    VkImageView view;
    /* The RTV DXVK creates: UNORM, narrowed to the colour attachment. */
    assert(view_with(&d, rt, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &view) == VK_SUCCESS);
    vkDestroyImageView(&d, view, NULL);
    /* An SRGB view that inherits or asks for COLOR_ATTACHMENT is refused:
     * SRGB has no colour-attachment role. A transfer-only view names no
     * reinterpreted operation and is accepted. */
    assert(view_with(&d, rt, VK_FORMAT_R8G8B8A8_SRGB, 0, &view) ==
           VK_ERROR_FEATURE_NOT_PRESENT);
    assert(view_with(&d, rt, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                     &view) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(view_with(&d, rt, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     &view) == VK_SUCCESS);
    vkDestroyImageView(&d, view, NULL);
    /* Outside the admitted set and outside the implemented family. */
    assert(view_with(&d, rt, VK_FORMAT_R8G8B8A8_UINT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     &view) == VK_ERROR_FEATURE_NOT_PRESENT);
    /* The immutable twin never takes another view format. */
    bind(&d, plain);
    assert(view_with(&d, plain, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     &view) == VK_ERROR_FEATURE_NOT_PRESENT);

    /* Mutable without a list admits the implemented family. */
    VkImage implicit = image_with(&d, VK_FORMAT_R8G8B8A8_UNORM, RT_USAGE, MUTABLE, NULL,
                                  VK_SUCCESS);
    assert(implicit->view_format_count == 2);
    /* Duplicate entries collapse; a one-entry list admits that entry only. */
    const VkFormat dup[3] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                             VK_FORMAT_R8G8B8A8_UNORM};
    VkImageFormatListCreateInfo list = family;
    list.viewFormatCount = 3; list.pViewFormats = dup;
    VkImage dupe = image_with(&d, VK_FORMAT_R8G8B8A8_UNORM, RT_USAGE, MUTABLE, &list,
                              VK_SUCCESS);
    assert(dupe->view_format_count == 2 && dupe->view_formats[0] == VK_FORMAT_R8G8B8A8_SRGB);
    list.viewFormatCount = 1; list.pViewFormats = RGBA8_FAMILY;
    VkImage unorm_only = image_with(&d, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, MUTABLE, &list,
        VK_SUCCESS);
    bind(&d, unorm_only);
    assert(view_with(&d, unorm_only, VK_FORMAT_R8G8B8A8_SRGB, 0, &view) ==
           VK_ERROR_FEATURE_NOT_PRESENT);
    /* Without MUTABLE a list holds at most the image format. */
    list.viewFormatCount = 1; list.pViewFormats = RGBA8_FAMILY;
    assert(image_with(&d, VK_FORMAT_R8G8B8A8_UNORM, RT_USAGE, 0, &list, VK_SUCCESS));
    list.viewFormatCount = 1; list.pViewFormats = RGBA8_FAMILY + 1;
    image_with(&d, VK_FORMAT_R8G8B8A8_UNORM, RT_USAGE, 0, &list, VK_ERROR_UNKNOWN);
    list.viewFormatCount = 2; list.pViewFormats = RGBA8_FAMILY;
    image_with(&d, VK_FORMAT_R8G8B8A8_UNORM, RT_USAGE, 0, &list, VK_ERROR_UNKNOWN);
    /* Unimplemented pairs, formats without a reinterpretation, a second or
     * unknown chained structure. */
    const VkFormat uint_pair[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT};
    list.pViewFormats = uint_pair;
    image_with(&d, VK_FORMAT_R8G8B8A8_UNORM, RT_USAGE, MUTABLE, &list,
               VK_ERROR_FEATURE_NOT_PRESENT);
    const VkFormat bgra[2] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB};
    list.pViewFormats = bgra;
    image_with(&d, VK_FORMAT_B8G8R8A8_UNORM,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
               MUTABLE, &list, VK_ERROR_FEATURE_NOT_PRESENT);
    image_with(&d, VK_FORMAT_B8G8R8A8_UNORM,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
               MUTABLE, NULL, VK_ERROR_FEATURE_NOT_PRESENT);
    VkImageFormatListCreateInfo second = family;
    list = family; list.pNext = &second;
    image_with(&d, VK_FORMAT_R8G8B8A8_UNORM, RT_USAGE, MUTABLE, &list,
               VK_ERROR_FEATURE_NOT_PRESENT);
    list.viewFormatCount = 2; list.pViewFormats = NULL; list.pNext = NULL;
    image_with(&d, VK_FORMAT_R8G8B8A8_UNORM, RT_USAGE, MUTABLE, &list, VK_ERROR_UNKNOWN);

    /* Sampling through a reinterpreted view: the uploaded RGBA8 texture read
     * as SRGB. The layout is the same byte-for-byte, and the descriptor
     * differs from the UNORM one only in word 1's data-format field
     * (0x038 -> 0x082), matching an immutable SRGB texture's word. */
    const VkImageUsageFlags SAMPLED = VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    struct ps5vk_texture_layout unorm_layout = {0}, srgb_layout = {0};
    assert(!ps5vk_texture_layout_for_format(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, &unorm_layout));
    assert(!ps5vk_texture_layout_for_format(VK_FORMAT_R8G8B8A8_SRGB, 64, 64, &srgb_layout));
    assert(unorm_layout.row_pitch == srgb_layout.row_pitch &&
           unorm_layout.bytes == srgb_layout.bytes &&
           unorm_layout.alignment == srgb_layout.alignment &&
           unorm_layout.slice_pitch == srgb_layout.slice_pitch);
    VkImage texture = image_with(&d, VK_FORMAT_R8G8B8A8_UNORM, SAMPLED, MUTABLE, &family,
                                 VK_SUCCESS);
    VkImage srgb_texture = image_with(&d, VK_FORMAT_R8G8B8A8_SRGB, SAMPLED, 0, NULL,
                                      VK_SUCCESS);
    assert(same_requirements(&texture->requirements, &srgb_texture->requirements));
    bind(&d, texture); bind(&d, srgb_texture);
    VkImageView unorm_view, srgb_view, native_srgb_view;
    assert(view_with(&d, texture, VK_FORMAT_R8G8B8A8_UNORM, 0, &unorm_view) == VK_SUCCESS);
    assert(view_with(&d, texture, VK_FORMAT_R8G8B8A8_SRGB, 0, &srgb_view) == VK_SUCCESS);
    assert(view_with(&d, srgb_texture, VK_FORMAT_R8G8B8A8_SRGB, 0, &native_srgb_view) ==
           VK_SUCCESS);
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler sampler;
    assert(vkCreateSampler(&d, &si, NULL, &sampler) == VK_SUCCESS);
    uint32_t unorm_words[12], srgb_words[12], native_words[12];
    assert(ps5vk_texture_descriptor(&d, unorm_view, sampler, unorm_words) == VK_SUCCESS);
    assert(ps5vk_texture_descriptor(&d, srgb_view, sampler, srgb_words) == VK_SUCCESS);
    assert(ps5vk_texture_descriptor(&d, native_srgb_view, sampler, native_words) == VK_SUCCESS);
    const uint32_t format_field = UINT32_C(0x1ff) << 20;
    assert((unorm_words[1] & format_field) == UINT32_C(0x038) << 20);
    assert((srgb_words[1] & format_field) == UINT32_C(0x082) << 20);
    assert((native_words[1] & format_field) == UINT32_C(0x082) << 20);
    assert(srgb_words[0] == unorm_words[0]);
    assert((srgb_words[1] & ~format_field) == (unorm_words[1] & ~format_field));
    assert(!memcmp(srgb_words + 2, unorm_words + 2, 10 * sizeof(uint32_t)));
    assert((native_words[1] & ~(format_field | 0xffu)) ==
           (srgb_words[1] & ~(format_field | 0xffu)));
    assert(!memcmp(native_words + 2, srgb_words + 2, 10 * sizeof(uint32_t)));
    /* The immutable twin still refuses a foreign view format at encode time,
     * even if a view were forged past creation. */
    struct VkImageView_T forged = *native_srgb_view;
    forged.format = VK_FORMAT_R8G8B8A8_UNORM;
    assert(ps5vk_texture_descriptor(&d, &forged, sampler, native_words) ==
           VK_ERROR_FEATURE_NOT_PRESENT);

    vkDestroySampler(&d, sampler, NULL);
    vkDestroyImageView(&d, unorm_view, NULL);
    vkDestroyImageView(&d, srgb_view, NULL);
    vkDestroyImageView(&d, native_srgb_view, NULL);
    for (unsigned n = 0; n < image_count; ++n) vkDestroyImage(&d, created_images[n], NULL);
    for (unsigned n = 0; n < memory_count; ++n) vkFreeMemory(&d, created_memories[n], NULL);
    assert(!d.images && !d.memories && !d.graphics_objects && !d.lifetime_errors);
}

int main(void)
{
    physical_routes();
    object_routes();
    return 0;
}
