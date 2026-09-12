#include "vk_internal.h"
#include "vk_image.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct VkDeviceMemory_T {
    VkDevice device;
    VkDeviceSize size, map_offset, map_size;
    void *address, *backing;
    VkBool32 mapped;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    struct VkDeviceMemory_T *next;
};
struct VkBuffer_T {
    VkDevice device;
    VkBufferUsageFlags usage;
    VkDeviceSize size, required_size, offset;
    VkDeviceMemory memory;
    VkBool32 ever_bound;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    struct VkBuffer_T *next;
};

/* Invalid-use diagnostics are defensive, not advertised Vulkan validation.
 * Vulkan requires callers to satisfy valid usage before entry. */
#define INVALID VK_ERROR_UNKNOWN

static int power_two(VkDeviceSize n) { return n && !(n & (n - 1)); }
static int range_in(VkDeviceSize total, VkDeviceSize offset, VkDeviceSize size,
                    VkDeviceSize *resolved)
{
    if (offset >= total) return 0;
    if (size == VK_WHOLE_SIZE) size = total - offset;
    if (!size || size > total - offset) return 0;
    *resolved = size;
    return 1;
}
static void *object_alloc(VkDevice device, const VkAllocationCallbacks *given,
                          size_t size, VkAllocationCallbacks *saved,
                          VkBool32 *custom)
{
    return ps5vk_object_alloc(device->custom_allocator ? &device->allocator : NULL,
        given, size, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, saved, custom);
}
static void object_free(void *object, const VkAllocationCallbacks *a, VkBool32 custom)
{
    ps5vk_object_free(object, a, custom);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice d,
    const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator,
    VkDeviceMemory *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO ||
        !info->allocationSize || info->memoryTypeIndex != 0) return INVALID;
    if (info->pNext) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!d->memory.allocate || !d->memory.release || !d->memory.flush ||
        !d->memory.invalidate || !power_two(d->noncoherent_atom))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (info->allocationSize > d->max_allocation || info->allocationSize > SIZE_MAX)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkAllocationCallbacks saved = {0};
    VkBool32 custom = VK_FALSE;
    VkDeviceMemory m = object_alloc(d, allocator, sizeof(*m), &saved, &custom);
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->allocator = saved; m->custom_allocator = custom;
    VkResult result = d->memory.allocate(d->memory.context, info->allocationSize,
                                        &m->address, &m->backing);
    if (result != VK_SUCCESS) { object_free(m, &saved, custom); return result; }
    /* A successful backend allocation must own CPU-addressable backing. */
    if (!m->address || !m->backing) {
        if (m->backing) d->memory.release(d->memory.context, m->backing);
        object_free(m, &saved, custom);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    m->device = d; m->size = info->allocationSize;
    m->next = d->memories; d->memories = m; *out = m;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice d, VkDeviceMemory m,
                                       const VkAllocationCallbacks *allocator)
{
    (void)allocator; /* Valid usage requires allocator compatibility. */
    if (!m || !d || m->device != d) return;
    for (VkImage image = d->images; image; image = image->next)
        if (image->memory == m && (image->pending ||
            (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_IMAGE, image)))) {
            ++d->lifetime_errors; return;
        }
    for (VkBuffer b = d->buffers; b; b = b->next)
        if (b->memory == m && d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_BUFFER, b)) {
            ++d->lifetime_errors; return;
        }
    /* Vulkan permits freeing backing while buffer objects remain alive; those
     * buffers cannot subsequently be used. Do not retain dangling host pointers. */
    for (VkBuffer b = d->buffers; b; b = b->next)
        if (b->memory == m) b->memory = VK_NULL_HANDLE;
    for (VkImage image = d->images; image; image = image->next)
        if (image->memory == m) image->memory = VK_NULL_HANDLE;
    VkDeviceMemory *p = &d->memories;
    while (*p && *p != m) p = &(*p)->next;
    if (!*p) return;
    *p = m->next;
    d->memory.release(d->memory.context, m->backing);
    VkAllocationCallbacks a = m->allocator;
    VkBool32 custom = m->custom_allocator;
    object_free(m, &a, custom);
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice d, VkDeviceMemory m,
    VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void **out)
{
    if (!out) return INVALID;
    *out = NULL;
    VkDeviceSize length;
    if (!d || !m || m->device != d || flags || m->mapped ||
        !range_in(m->size, offset, size, &length)) return VK_ERROR_MEMORY_MAP_FAILED;
    m->mapped = VK_TRUE; m->map_offset = offset; m->map_size = length;
    *out = (unsigned char *)m->address + offset;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice d, VkDeviceMemory m)
{
    if (!d || !m || m->device != d) return;
    /* Unmapping is NOT an implicit noncoherent flush. */
    m->mapped = VK_FALSE; m->map_offset = m->map_size = 0;
}

static int sync_range(VkDevice d, const VkMappedMemoryRange *r, VkDeviceSize *size)
{
    VkDeviceMemory m = r->memory;
    if (r->sType != VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE || r->pNext || !m ||
        m->device != d || !m->mapped || !power_two(d->noncoherent_atom) ||
        r->offset < m->map_offset || r->offset - m->map_offset >= m->map_size) return 0;
    /* VkMappedMemoryRange WHOLE_SIZE ends at the current mapping, unlike
     * vkMapMemory/descriptor WHOLE_SIZE, which resolves against the allocation
     * or buffer. Partial mappings must not flush inaccessible trailing bytes. */
    VkDeviceSize available = m->map_size - (r->offset - m->map_offset);
    *size = r->size == VK_WHOLE_SIZE ? available : r->size;
    if (!*size || *size > available) return 0;
    return r->offset % d->noncoherent_atom == 0 &&
        (*size % d->noncoherent_atom == 0 || *size == m->size - r->offset);
}
static VkResult sync_ranges(VkDevice d, uint32_t count,
                            const VkMappedMemoryRange *ranges, int invalidate)
{
    if (!d || (count && !ranges)) return INVALID;
    VkDeviceSize size;
    /* Validate the whole array before any backend effect. A backend runtime
     * error can still occur after earlier ranges were processed. */
    for (uint32_t i = 0; i < count; ++i)
        if (!sync_range(d, &ranges[i], &size)) return INVALID;
    for (uint32_t i = 0; i < count; ++i) {
        (void)sync_range(d, &ranges[i], &size);
        VkResult result = (invalidate ? d->memory.invalidate : d->memory.flush)(
            d->memory.context, ranges[i].memory->backing, ranges[i].offset, size);
        if (result != VK_SUCCESS) return result;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice d, uint32_t n,
                                                       const VkMappedMemoryRange *r)
{ return sync_ranges(d, n, r, 0); }
VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(VkDevice d, uint32_t n,
                                                            const VkMappedMemoryRange *r)
{ return sync_ranges(d, n, r, 1); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBuffer *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO ||
        !info->size || !power_two(d->buffer_alignment)) return INVALID;
    if (info->pNext || info->flags || info->sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        !info->usage || (info->usage & ~(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT))) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->size > UINT64_MAX - (d->buffer_alignment - 1))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkDeviceSize required = (info->size + d->buffer_alignment - 1) & ~(d->buffer_alignment - 1);
    if (required > d->max_allocation) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkBuffer b = object_alloc(d, allocator, sizeof(*b), &saved, &custom);
    if (!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
    b->allocator = saved; b->custom_allocator = custom;
    b->device = d; b->size = info->size; b->required_size = required;
    b->usage = info->usage;
    b->next = d->buffers; d->buffers = b; *out = b;
    return VK_SUCCESS;
}
VkBool32 ps5vk_buffer_usage(VkDevice d,VkBuffer b,VkBufferUsageFlags usage)
{ return d && b && b->device==d && (b->usage & usage)==usage; }
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice d, VkBuffer b,
                                          const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!d || !b || b->device != d) return;
    if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_BUFFER, b)) { ++d->lifetime_errors; return; }
    VkBuffer *p = &d->buffers;
    while (*p && *p != b) p = &(*p)->next;
    if (!*p) return;
    *p = b->next;
    VkAllocationCallbacks a = b->allocator; VkBool32 custom = b->custom_allocator;
    object_free(b, &a, custom);
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice d, VkBuffer b,
                                                        VkMemoryRequirements *out)
{
    if (!out) return;
    *out = (VkMemoryRequirements){0};
    if (d && b && b->device == d)
        *out = (VkMemoryRequirements){b->required_size, d->buffer_alignment, 1};
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice d, VkBuffer b,
                                                VkDeviceMemory m, VkDeviceSize offset)
{
    if (!d || !b || !m || b->device != d || m->device != d || b->ever_bound ||
        offset % d->buffer_alignment || offset > m->size ||
        b->required_size > m->size - offset) return INVALID;
    b->memory = m; b->offset = offset; b->ever_bound = VK_TRUE;
    return VK_SUCCESS;
}
VkResult ps5vk_buffer_span(VkDevice d, VkBuffer b, VkDeviceSize offset,
                          VkDeviceSize range, void **address, VkDeviceSize *size)
{
    if (!address || !size) return INVALID;
    *address = NULL; *size = 0;
    VkDeviceSize length;
    if (!d || !b) return INVALID;
    VkBuffer live = d->buffers;
    while (live && live != b) live = live->next;
    if (!live || b->device != d || !b->memory ||
        !range_in(b->size, offset, range, &length)) return INVALID;
    *address = (unsigned char *)b->memory->address + b->offset + offset;
    *size = length;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice d, const VkImageCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImage *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO) return INVALID;
    if (!d->graphics_enabled || !d->image_requirements) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->pNext || info->flags || info->imageType != VK_IMAGE_TYPE_2D ||
        info->arrayLayers != 1 || info->samples != VK_SAMPLE_COUNT_1_BIT ||
        info->sharingMode != VK_SHARING_MODE_EXCLUSIVE || info->tiling != VK_IMAGE_TILING_OPTIMAL ||
        info->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED ||
        (info->format != VK_FORMAT_R8G8B8A8_UNORM && info->format != VK_FORMAT_B8G8R8A8_UNORM &&
         info->format != VK_FORMAT_D32_SFLOAT)) return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkImageUsageFlags supported = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!info->usage || info->usage & ~supported || !info->extent.width || !info->extent.height ||
        info->extent.depth != 1 || !info->mipLevels) return INVALID;
    uint32_t dim = info->extent.width > info->extent.height ? info->extent.width : info->extent.height;
    uint32_t levels = 0; for (; dim; dim >>= 1) ++levels;
    if (info->mipLevels > levels ||
        (info->format == VK_FORMAT_D32_SFLOAT ? (info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) :
         (info->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) return INVALID;
    VkMemoryRequirements requirements = {0};
    VkResult rc = d->image_requirements(d, info, &requirements);
    if (rc != VK_SUCCESS) return rc;
    if (!requirements.size || requirements.size > d->max_allocation ||
        !power_two(requirements.alignment) || requirements.memoryTypeBits != 1 ||
        requirements.size % requirements.alignment) return VK_ERROR_INITIALIZATION_FAILED;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkImage image = object_alloc(d, allocator, sizeof(*image), &saved, &custom);
    if (!image) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(image, 0, sizeof(*image));
    image->device = d; image->allocator = saved; image->custom_allocator = custom;
    image->info = *info;
    /* Queue family indices are ignored for exclusive sharing; do not retain a
     * caller-owned pointer even when a caller supplies an ignored array. */
    image->info.pQueueFamilyIndices = NULL; image->info.queueFamilyIndexCount = 0;
    image->requirements = requirements;
    image->next = d->images; d->images = image; ++d->graphics_objects;
    *out = image; return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice d, VkImage image, VkMemoryRequirements *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (d && image && image->device == d) *out = image->requirements;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice d, VkImage image, VkDeviceMemory m, VkDeviceSize offset)
{
    if (!d || !image || image->device != d || !m || m->device != d || image->ever_bound ||
        offset % image->requirements.alignment || offset > m->size ||
        image->requirements.size > m->size - offset) return INVALID;
    image->memory = m; image->offset = offset; image->ever_bound = VK_TRUE;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice d, VkImage image, const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!image) return;
    if (!d || image->device != d || image->pending || image->views ||
        (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_IMAGE, image))) {
        if (d) ++d->lifetime_errors;
        return;
    }
    VkImage *p = &d->images;
    while (*p && *p != image) p = &(*p)->next;
    if (!*p) return;
    *p = image->next; --d->graphics_objects;
    VkAllocationCallbacks saved = image->allocator; VkBool32 custom = image->custom_allocator;
    object_free(image, &saved, custom);
}

VkResult ps5vk_image_span(VkDevice d, VkImage image, void **address, VkDeviceSize *bytes)
{
    if (!address || !bytes) return INVALID;
    *address = NULL; *bytes = 0;
    if (!d || !image) return INVALID;
    VkImage live = d->images;
    while (live && live != image) live = live->next;
    if (!live || !image->memory || image->memory->device != d ||
        image->offset > image->memory->size || image->requirements.size > image->memory->size - image->offset)
        return INVALID;
    *address = (unsigned char *)image->memory->address + image->offset;
    *bytes = image->requirements.size;
    return VK_SUCCESS;
}
