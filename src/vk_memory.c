#include "vk_internal.h"
#include "vk_descriptor.h"
#include "vk_image.h"
#include "depth_stencil_layout.h"
#include "texture_format.h"
#include "texture_layout.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(PS5VK_TARGET_PS5) && PS5VK_TARGET_PS5
#include "ps5log.h"
#define IMAGE_MARK(...) ps5log_printf(PS5LOG_MARK, __VA_ARGS__)
#else
#define IMAGE_MARK(...) ((void)0)
#endif

/* Exactly the descriptor the pinned upstream draw module's host readback
 * creates: RGBA8, 2D, one mip, one layer, one sample, LINEAR tiling, usage
 * TRANSFER_DST alone, exclusive sharing and an UNDEFINED initial layout. Every
 * field is part of the match, so the linear role cannot be widened by a caller
 * that only gets the interesting ones right. */

struct VkDeviceMemory_T {
    VkDevice device;
    VkDeviceSize size, map_offset, map_size;
    void *address, *backing;
    VkDeviceAddress gpu_address;
    VkBool32 device_address_allocation;
    VkBool32 mapped;
    /* VK_KHR_dedicated_allocation: the one resource this allocation was made
     * for, or none. Such memory binds only that resource, at offset 0; it stays
     * dedicated (and unbindable) after that resource is destroyed. */
    VkBool32 dedicated;
    VkImage dedicated_image;
    struct VkBuffer_T *dedicated_buffer;
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

/* The host backend has no GPU virtual address. The native direct-memory
 * backend supplies a strong implementation; a host test can do the same with
 * a synthetic GPU address distinct from its CPU mapping. */
__attribute__((weak)) VkResult ps5vk_memory_backend_device_address(
    void *backing, VkDeviceAddress *out)
{
    (void)backing;
    if (out) *out = 0;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice d,
    const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator,
    VkDeviceMemory *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO ||
        !info->allocationSize || info->memoryTypeIndex != 0) return INVALID;
    VkBool32 device_address_allocation = VK_FALSE;
    VkBool32 saw_flags = VK_FALSE, saw_capture = VK_FALSE, saw_dedicated = VK_FALSE;
    VkImage dedicated_image = VK_NULL_HANDLE;
    VkBuffer dedicated_buffer = VK_NULL_HANDLE;
    for (const VkBaseInStructure *next = (const VkBaseInStructure *)info->pNext;
         next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
            if (saw_flags) return INVALID;
            saw_flags = VK_TRUE;
            const VkMemoryAllocateFlagsInfo *flags =
                (const VkMemoryAllocateFlagsInfo *)next;
            const VkMemoryAllocateFlags supported =
                VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT |
                VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT_KHR;
            if (flags->flags & ~supported)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            if (flags->flags & VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT_KHR) {
                /* The driver exposes one physical device in its group. */
                if (!d->device_group_extension_enabled || flags->deviceMask != 1)
                    return VK_ERROR_FEATURE_NOT_PRESENT;
            } else if (flags->deviceMask) return VK_ERROR_FEATURE_NOT_PRESENT;
            if (flags->flags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT) {
                if (!(d->enabled_features & PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS))
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                device_address_allocation = VK_TRUE;
            }
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO) {
            if (saw_capture) return INVALID;
            saw_capture = VK_TRUE;
            if (((const VkMemoryOpaqueCaptureAddressAllocateInfo *)next)->opaqueCaptureAddress)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        } else if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
            if (!d->dedicated_allocation_extension_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
            if (saw_dedicated) return INVALID;
            saw_dedicated = VK_TRUE;
            const VkMemoryDedicatedAllocateInfo *dedicated =
                (const VkMemoryDedicatedAllocateInfo *)next;
            dedicated_image = dedicated->image;
            dedicated_buffer = dedicated->buffer;
            /* VUID-VkMemoryDedicatedAllocateInfo-image-01432 and -02964/-02965:
             * at most one resource, live on this device, and the allocation is
             * exactly that resource's reported size. */
            if (dedicated_image && dedicated_buffer) return INVALID;
            if (dedicated_image) {
                VkImage live = d->images;
                while (live && live != dedicated_image) live = live->next;
                if (!live || live->ever_bound ||
                    info->allocationSize != live->requirements.size) return INVALID;
            }
            if (dedicated_buffer) {
                VkBuffer live = d->buffers;
                while (live && live != dedicated_buffer) live = live->next;
                if (!live || live->ever_bound ||
                    info->allocationSize != live->required_size) return INVALID;
            }
        } else return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (saw_capture && !device_address_allocation)
        return VK_ERROR_FEATURE_NOT_PRESENT;
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
    if (device_address_allocation) {
        VkDeviceAddress gpu_address = 0;
        result = ps5vk_memory_backend_device_address(m->backing, &gpu_address);
        if (result != VK_SUCCESS || !gpu_address ||
            info->allocationSize > UINT64_MAX - gpu_address) {
            d->memory.release(d->memory.context, m->backing);
            object_free(m, &saved, custom);
            return result == VK_SUCCESS ? VK_ERROR_MEMORY_MAP_FAILED : result;
        }
        m->gpu_address = gpu_address;
    }
    m->device = d; m->size = info->allocationSize;
    m->device_address_allocation = device_address_allocation;
    m->dedicated = dedicated_image || dedicated_buffer;
    m->dedicated_image = dedicated_image; m->dedicated_buffer = dedicated_buffer;
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
    if (!d || !count || !ranges) return INVALID;
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
VKAPI_ATTR void VKAPI_CALL vkGetDeviceMemoryCommitment(VkDevice d, VkDeviceMemory m,
                                                       VkDeviceSize *pCommittedMemoryInBytes)
{
    if (!pCommittedMemoryInBytes) return;
    *pCommittedMemoryInBytes = 0;
    if (!d || !m || m->device != d) return;
    /*
     * The physical profile exposes no memory type with
     * VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, and Vulkan valid usage
     * restricts this query to lazily allocated memory. In this no-lazy
     * profile, ordinary allocations are never lazily committed; safely
     * report 0 bytes committed (*pCommittedMemoryInBytes = 0).
     */
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBuffer *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO ||
        !info->size || !power_two(d->buffer_alignment)) return INVALID;
    /* Usage is permission, not a capability claim. STORAGE_TEXEL_BUFFER is
     * admitted because D3D11 UAV buffers carry it next to STORAGE_BUFFER
     * (their raw/structured access); a storage-texel VIEW still needs a format
     * with the witnessed storage-texel role, and none has one. Transform
     * feedback usage stays refused until that feature exists. */
    if (info->pNext || info->flags || info->sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        !info->usage || (info->usage & ~(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if ((info->usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) &&
        !(d->enabled_features & PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS))
        return VK_ERROR_FEATURE_NOT_PRESENT;
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
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBufferView(VkDevice d,const VkBufferViewCreateInfo *info,
    const VkAllocationCallbacks *allocator,VkBufferView *out)
{
    if(!out)return INVALID;
    *out=VK_NULL_HANDLE;
    if(!d || !info || info->sType!=VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO || info->pNext ||
       info->flags || !info->buffer || info->buffer->device!=d)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* The view serves every texel role its buffer's usage names
     * (VUID-VkBufferViewCreateInfo-buffer-00933/-00934), and needs one. */
    const VkBool32 uniform_role=ps5vk_buffer_usage(d,info->buffer,
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
    const VkBool32 storage_role=ps5vk_buffer_usage(d,info->buffer,
        VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT);
    if(!uniform_role && !storage_role)return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Creation follows the WITNESSED capability, exactly like the published
     * VkFormatProperties: a format whose uniform-texel role is implemented but
     * still waiting for its console witness must not become a public success
     * path, so the reported set and the creatable set stay identical instead
     * of merely nested. The alignment bound is the texel size of the same
     * capability row. */
    const struct ps5vk_texture_format *texel=ps5vk_texture_format_lookup(info->format);
    const uint32_t element=texel?texel->bytes_per_texel:0;
    if((uniform_role &&
        !ps5vk_texture_format_witnessed(info->format,PS5VK_FORMAT_CAP_UNIFORM_TEXEL_BUFFER)) ||
       (storage_role &&
        !ps5vk_texture_format_witnessed(info->format,PS5VK_FORMAT_CAP_STORAGE_TEXEL_BUFFER)) ||
       !element || info->offset%element)return VK_ERROR_FEATURE_NOT_PRESENT;
    void *address;VkDeviceSize range;
    if(ps5vk_buffer_span(d,info->buffer,info->offset,info->range,&address,&range)!=VK_SUCCESS ||
       range%element || (uintptr_t)address%element)return INVALID;
    VkAllocationCallbacks saved={0};VkBool32 custom=VK_FALSE;
    VkBufferView view=object_alloc(d,allocator,sizeof(*view),&saved,&custom);
    if(!view)return VK_ERROR_OUT_OF_HOST_MEMORY;
    view->device=d;view->buffer=info->buffer;view->format=info->format;
    view->offset=info->offset;view->range=range;view->allocator=saved;view->custom_allocator=custom;
    view->next=d->buffer_views;d->buffer_views=view;*out=view;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBufferView(VkDevice d,VkBufferView view,
    const VkAllocationCallbacks *allocator)
{
    (void)allocator;if(!d || !view || view->device!=d)return;
    if(view->pending || (d->invalidate && !d->invalidate(d,VK_OBJECT_TYPE_BUFFER_VIEW,view)))
        {++d->lifetime_errors;return;}
    VkBufferView *link=&d->buffer_views;while(*link && *link!=view)link=&(*link)->next;
    if(!*link)return;
    *link=view->next;
    VkAllocationCallbacks saved=view->allocator;VkBool32 custom=view->custom_allocator;
    object_free(view,&saved,custom);
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice d, VkBuffer b,
                                          const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!d || !b || b->device != d) return;
    for(VkBufferView view=d->buffer_views;view;view=view->next)
        if(view->buffer==b){++d->lifetime_errors;return;}
    if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_BUFFER, b)) { ++d->lifetime_errors; return; }
    VkBuffer *p = &d->buffers;
    while (*p && *p != b) p = &(*p)->next;
    if (!*p) return;
    *p = b->next;
    for (VkDeviceMemory m = d->memories; m; m = m->next)
        if (m->dedicated_buffer == b) m->dedicated_buffer = VK_NULL_HANDLE;
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
static VkResult buffer_bind_check(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize offset)
{
    if (!d || !b || !m || b->device != d || m->device != d || b->ever_bound ||
        offset % d->buffer_alignment || offset > m->size ||
        b->required_size > m->size - offset) return INVALID;
    /* VUID-vkBindBufferMemory-memory-01508: dedicated memory binds only its
     * own buffer, at offset 0. */
    if (m->dedicated && (m->dedicated_buffer != b || offset)) return INVALID;
    if ((b->usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) &&
        !m->device_address_allocation) return VK_ERROR_FEATURE_NOT_PRESENT;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice d, VkBuffer b,
                                                VkDeviceMemory m, VkDeviceSize offset)
{
    VkResult result = buffer_bind_check(d, b, m, offset);
    if (result != VK_SUCCESS) return result;
    b->memory = m; b->offset = offset; b->ever_bound = VK_TRUE;
    return VK_SUCCESS;
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddressKHR(VkDevice d,
    const VkBufferDeviceAddressInfo *info)
{
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO ||
        info->pNext || !(d->enabled_features & PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS))
        return 0;
    VkBuffer live = d->buffers;
    while (live && live != info->buffer) live = live->next;
    if (!live || !(live->usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ||
        !live->memory || !live->memory->device_address_allocation)
        return 0;
    return live->memory->gpu_address + live->offset;
}
VKAPI_ATTR uint64_t VKAPI_CALL vkGetBufferOpaqueCaptureAddressKHR(VkDevice d,
    const VkBufferDeviceAddressInfo *info)
{ (void)d; (void)info; return 0; }
VKAPI_ATTR uint64_t VKAPI_CALL vkGetDeviceMemoryOpaqueCaptureAddressKHR(VkDevice d,
    const VkDeviceMemoryOpaqueCaptureAddressInfo *info)
{ (void)d; (void)info; return 0; }
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

VkResult ps5vk_image_flush_range(VkDevice d, VkImage image, VkDeviceSize offset, VkDeviceSize size)
{
    void *address = NULL; VkDeviceSize bytes = 0;
    if (ps5vk_image_span(d, image, &address, &bytes) != VK_SUCCESS) return INVALID;
    if (!image->memory || offset > bytes || size > bytes - offset) return INVALID;
    if (!d->memory.flush) return VK_SUCCESS;
    return d->memory.flush(d->memory.context, image->memory->backing, image->offset + offset, size);
}

VkResult ps5vk_image_invalidate_range(VkDevice d, VkImage image, VkDeviceSize offset, VkDeviceSize size)
{
    void *address = NULL; VkDeviceSize bytes = 0;
    if (ps5vk_image_span(d, image, &address, &bytes) != VK_SUCCESS) return INVALID;
    if (!image->memory || offset > bytes || size > bytes - offset) return INVALID;
    if (!d->memory.invalidate) return VK_SUCCESS;
    return d->memory.invalidate(d->memory.context, image->memory->backing, image->offset + offset, size);
}

VkResult ps5vk_buffer_cache(VkDevice d, VkBuffer b, VkDeviceSize offset,
    VkDeviceSize range, VkBool32 invalidate)
{
    VkDeviceSize length;
    if (!d || !b || b->device != d || !b->memory ||
        !range_in(b->size, offset, range, &length)) return INVALID;
    VkBuffer live = d->buffers;
    while (live && live != b) live = live->next;
    if (!live) return INVALID;
    VkResult (*operation)(void *, void *, VkDeviceSize, VkDeviceSize) =
        invalidate ? d->memory.invalidate : d->memory.flush;
    if (!operation) return VK_ERROR_INITIALIZATION_FAILED;
    return operation(d->memory.context, b->memory->backing,
        b->offset + offset, length);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice d, const VkImageCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImage *out)
{
    IMAGE_MARK("PS5VK_IMAGE_CREATE format=%u samples=%u usage=%08x extent=%ux%u",
        info ? (unsigned)info->format : 0u,
        info ? (unsigned)info->samples : 0u,
        info ? (unsigned)info->usage : 0u,
        info ? info->extent.width : 0u,
        info ? info->extent.height : 0u);
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO) return INVALID;
    if (!d->graphics_enabled || !d->image_requirements) return VK_ERROR_FEATURE_NOT_PRESENT;
    /* The one linear image this profile creates: the pinned upstream draw
     * module's host-readback staging image, which it creates with
     * VK_IMAGE_TILING_LINEAR and TRANSFER_DST only
     * (vktDrawImageObjectUtil.cpp:398-401) and then addresses through
     * vkGetImageSubresourceLayout. The descriptor is matched exactly here, so
     * a different format, type, mip/layer/sample count, usage, sharing mode or
     * initial layout stays refused instead of being stored as tiled and read
     * back as if it were linear. */
    const int linear_staging = ps5vk_linear_staging_descriptor(info);
    /* The sample counts this device's platform serves (DXVK262-T06). 1x is
     * always accepted; a multisampled count is accepted only for the one role
     * that has a multisampled target - a 2D one-mip colour image in one of the
     * profile's colour formats, created with exactly the role combinations the
     * pinned multisample oracle builds (colour attachment, its readback pair,
     * and the per-sample input-attachment form). The backing requirements apply
     * the same bound, so a shape this predicate let through cannot be refused
     * later for its size. */
    int samples_supported = info->samples == VK_SAMPLE_COUNT_1_BIT;
    if (!samples_supported &&
        ps5vk_multisampled_color_usage(info->usage) &&
        (info->format == VK_FORMAT_B8G8R8A8_UNORM || info->format == VK_FORMAT_R8G8B8A8_UNORM) &&
        info->imageType == VK_IMAGE_TYPE_2D && info->mipLevels == 1 &&
        (ps5vk_platform_sample_counts(d->platform_features) & info->samples))
        samples_supported = 1;
    /* Mutable-format views. VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT is admitted
     * only for a format with an implemented reinterpretation and only on a
     * device whose platform serves it; a VkImageFormatListCreateInfo
     * (VK_KHR_image_format_list, enabled on the device) may narrow the view
     * formats, and each entry must be an implemented reinterpretation. The
     * flag changes no storage, so the backend and every shape predicate see
     * the storage flags alone. */
    const VkImageFormatListCreateInfo *format_list = NULL;
    for (const VkBaseInStructure *next = (const VkBaseInStructure *)info->pNext; next;
         next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO && !format_list &&
            d->image_format_list_extension_enabled)
            format_list = (const VkImageFormatListCreateInfo *)next;
        else
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const VkBool32 mutable_format = (info->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0;
    const VkImageCreateFlags storage_flags =
        info->flags & ~(VkImageCreateFlags)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (mutable_format && (!d->mutable_format_views || info->tiling != VK_IMAGE_TILING_OPTIMAL ||
                           !ps5vk_texture_format_mutable(info->format)))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t list_count = format_list ? format_list->viewFormatCount : 0u;
    if (list_count && !format_list->pViewFormats) return INVALID;
    /* VUID-VkImageCreateInfo-flags-04738: without the mutable flag a list
     * holds at most the image's own format. */
    if (!mutable_format &&
        (list_count > 1u || (list_count == 1u && format_list->pViewFormats[0] != info->format)))
        return INVALID;
    for (uint32_t n = 0; n < list_count; ++n)
        if (!ps5vk_texture_format_view_compatible(info->format, format_list->pViewFormats[n]))
            return VK_ERROR_FEATURE_NOT_PRESENT;
    if ((storage_flags && storage_flags != VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ||
        (info->imageType != VK_IMAGE_TYPE_1D && info->imageType != VK_IMAGE_TYPE_2D &&
         info->imageType != VK_IMAGE_TYPE_3D) ||
        !info->arrayLayers || !samples_supported ||
        info->sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        info->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED) return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Only the one linear descriptor above is backed; any other linear request
     * is a format/tiling combination this profile does not support, which is
     * what the corresponding capability query reports too. */
    if (info->tiling != VK_IMAGE_TILING_OPTIMAL && !linear_staging)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    /* The mask admits the input-attachment role so the pinned multiview
     * helper's exact shape can be validated below; the exact-combination
     * predicate is what actually accepts a usage set, so every other role
     * combination stays refused. */
    const VkImageUsageFlags supported = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT;
    /* The input-attachment role is bounded by the measured six-view floor: the
     * format query reports exactly that ceiling for this shape, so creation has
     * to refuse anything deeper rather than accept a shape the query says does
     * not exist. No other role is affected. */
    if ((info->usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) &&
        info->arrayLayers > (uint32_t)PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!info->usage || info->usage & ~supported || !info->extent.width || !info->extent.height ||
        !info->extent.depth || !info->mipLevels ||
        (info->imageType==VK_IMAGE_TYPE_1D &&
         (info->extent.height!=1 || info->extent.depth!=1)) ||
        (info->imageType==VK_IMAGE_TYPE_2D && info->extent.depth!=1) ||
        (info->imageType==VK_IMAGE_TYPE_3D && info->arrayLayers!=1) ||
        (storage_flags==VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT &&
         (info->imageType!=VK_IMAGE_TYPE_2D || info->arrayLayers<6 ||
          info->extent.width!=info->extent.height ||
          info->samples!=VK_SAMPLE_COUNT_1_BIT))) return INVALID;
    uint32_t dim = info->extent.width > info->extent.height ? info->extent.width : info->extent.height;
    if(info->extent.depth>dim)dim=info->extent.depth;
    uint32_t levels = 0; for (; dim; dim >>= 1) ++levels;
    if (info->mipLevels > levels ||
        (ps5vk_format_aspects(info->format) != VK_IMAGE_ASPECT_COLOR_BIT ?
         (info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) :
         (info->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) return INVALID;
    if ((info->usage & VK_IMAGE_USAGE_STORAGE_BIT) &&
        (info->format != VK_FORMAT_R32_UINT || info->imageType != VK_IMAGE_TYPE_2D ||
         info->extent.width > 8 || info->extent.height > 8 ||
         info->mipLevels != 1 || info->arrayLayers != 1 ||
         info->usage != (VK_IMAGE_USAGE_STORAGE_BIT |
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    /* The backend owns format/usage support. Keeping a second format whitelist
     * here made newly validated native formats impossible to create even when
     * the query and requirements paths accepted them. */
    VkImageCreateInfo storage = *info;
    storage.pNext = NULL; storage.flags = storage_flags;
    VkMemoryRequirements requirements = {0};
    VkResult rc = d->image_requirements(d, &storage, &requirements);
    if (rc != VK_SUCCESS) return rc;
    if (!requirements.size || requirements.size > d->max_allocation ||
        !power_two(requirements.alignment) || requirements.memoryTypeBits != 1 ||
        requirements.size % requirements.alignment) return VK_ERROR_INITIALIZATION_FAILED;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    struct VkImage_T shape = {.info = storage};
    size_t layout_count = 0;
    if ((info->mipLevels > 1 || info->arrayLayers > 1) &&
        (ps5vk_bc_linear_image(&shape) || ps5vk_rgba_linear_image(&shape))) {
        if ((size_t)info->mipLevels > (SIZE_MAX - sizeof(shape)) /
            sizeof(VkImageLayout) / info->arrayLayers) return VK_ERROR_OUT_OF_HOST_MEMORY;
        layout_count = (size_t)info->mipLevels * info->arrayLayers;
    }
    VkImage image = object_alloc(d, allocator,
        sizeof(*image) + layout_count * sizeof(VkImageLayout), &saved, &custom);
    if (!image) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(image, 0, sizeof(*image));
    image->device = d; image->allocator = saved; image->custom_allocator = custom;
    image->info = storage;
    if (mutable_format) {
        /* The admitted view formats, without duplicates: the list's entries,
         * or every implemented reinterpretation when no list narrows it. */
        image->mutable_format = VK_TRUE;
        const uint32_t candidate_count = list_count ? list_count : ps5vk_texture_format_count();
        for (uint32_t n = 0; n < candidate_count; ++n) {
            const VkFormat candidate = list_count ? format_list->pViewFormats[n] :
                ps5vk_texture_format_at(n)->format;
            VkBool32 seen = !ps5vk_texture_format_view_compatible(info->format, candidate);
            for (uint32_t m = 0; m < image->view_format_count && !seen; ++m)
                seen = image->view_formats[m] == candidate;
            if (!seen && image->view_format_count <
                    (uint32_t)(sizeof(image->view_formats) / sizeof(image->view_formats[0])))
                image->view_formats[image->view_format_count++] = candidate;
        }
    }
    if (layout_count) {
        image->subresource_layouts = (VkImageLayout *)(image + 1);
        memset(image->subresource_layouts, 0, layout_count * sizeof(VkImageLayout));
    }
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

VKAPI_ATTR void VKAPI_CALL vkGetImageSparseMemoryRequirements(VkDevice d, VkImage image,
    uint32_t *count, VkSparseImageMemoryRequirements *out)
{
    (void)out;
    if (!count) return;
    /* Images cannot be created with VK_IMAGE_CREATE_SPARSE_BINDING_BIT because
     * sparseBinding is not advertised, so every valid image reports zero sparse
     * memory requirements. Invalid handles remain outside this contract. */
    *count = 0;
    if (!d || !image || image->device != d) return;
}

VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice d, VkImage image,
    const VkImageSubresource *pSubresource, VkSubresourceLayout *pLayout)
{
    if (!pLayout) return;
    memset(pLayout, 0, sizeof(*pLayout));
    if (!d || !image || image->device != d || !pSubresource) return;
    /* Tiled images never report a fabricated linear layout: they stay zeroed.
     * The one linear image this profile creates is described honestly from the
     * same padded linear layout the transfer role and the upload path use. */
    if (!ps5vk_linear_staging_image(image)) return;
    if (pSubresource->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
        pSubresource->mipLevel || pSubresource->arrayLayer) return;
    uint32_t row_pitch = 0;
    uint64_t bytes = 0;
    if (ps5vk_texture_row_layout(4u, image->info.extent.width,
            image->info.extent.height, &row_pitch, &bytes)) return;
    pLayout->offset = 0;
    pLayout->rowPitch = row_pitch;
    pLayout->depthPitch = bytes;
    pLayout->arrayPitch = bytes;
    pLayout->size = bytes;
}

static VkResult image_bind_check(VkDevice d, VkImage image, VkDeviceMemory m, VkDeviceSize offset)
{
    if (!d || !image || image->device != d || !m || m->device != d || image->ever_bound ||
        offset % image->requirements.alignment || offset > m->size ||
        image->requirements.size > m->size - offset) return INVALID;
    /* VUID-vkBindImageMemory-memory-01509: dedicated memory binds only its
     * own image, at offset 0. */
    if (m->dedicated && (m->dedicated_image != image || offset)) return INVALID;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice d, VkImage image, VkDeviceMemory m, VkDeviceSize offset)
{
    IMAGE_MARK("PS5VK_IMAGE_BIND samples=%u usage=%08x extent=%ux%u",
        image && image->device == d ? (unsigned)image->info.samples : 0u,
        image && image->device == d ? (unsigned)image->info.usage : 0u,
        image && image->device == d ? image->info.extent.width : 0u,
        image && image->device == d ? image->info.extent.height : 0u);
    VkResult result = image_bind_check(d, image, m, offset);
    if (result != VK_SUCCESS) return result;
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
    for (VkDeviceMemory m = d->memories; m; m = m->next)
        if (m->dedicated_image == image) m->dedicated_image = VK_NULL_HANDLE;
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


/* The pinned upstream draw module's host-readback staging image: the only
 * linear-tiling image this profile creates. It is real linear memory, so
 * vkGetImageSubresourceLayout can describe it and a colour copy can fill it
 * row by row; nothing samples, renders into or clears it. */

/* The colour-attachment shape whose clear and buffer-upload destinations are
 * implemented. It is the readback row of the transfer format with the
 * attachment role and a transfer destination declared, which is exactly the
 * image the pinned upstream draw tests create; every other combination stays
 * fail-closed, and the transfer-only predicate above is unchanged. */

/* VK_KHR_get_memory_requirements2 / VK_KHR_dedicated_allocation. The 2KHR
 * queries report exactly the Vulkan 1.0 requirements; the only output chain
 * structure is VkMemoryDedicatedRequirements, which this profile answers with
 * neither a preference nor a requirement: every buffer and image is placed by
 * offset in ordinary memory, and nothing in the backend benefits from, or
 * depends on, one allocation per resource. */
static void dedicated_requirements(VkDevice d, void *chain)
{
    for (VkBaseOutStructure *next = chain; next; next = next->pNext)
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS &&
            d->dedicated_allocation_extension_enabled) {
            VkMemoryDedicatedRequirements *dedicated = (VkMemoryDedicatedRequirements *)next;
            dedicated->prefersDedicatedAllocation = VK_FALSE;
            dedicated->requiresDedicatedAllocation = VK_FALSE;
        }
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2KHR(VkDevice d,
    const VkBufferMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *out)
{
    if (!out || out->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2) return;
    out->memoryRequirements = (VkMemoryRequirements){0};
    if (!d || !d->memory_requirements2_extension_enabled || !info ||
        info->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2 || info->pNext)
        return;
    vkGetBufferMemoryRequirements(d, info->buffer, &out->memoryRequirements);
    dedicated_requirements(d, out->pNext);
}
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2KHR(VkDevice d,
    const VkImageMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *out)
{
    if (!out || out->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2) return;
    out->memoryRequirements = (VkMemoryRequirements){0};
    /* VkImagePlaneMemoryRequirementsInfo is for disjoint multi-planar images,
     * which this profile cannot create. */
    if (!d || !d->memory_requirements2_extension_enabled || !info ||
        info->sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 || info->pNext)
        return;
    vkGetImageMemoryRequirements(d, info->image, &out->memoryRequirements);
    dedicated_requirements(d, out->pNext);
}
VKAPI_ATTR void VKAPI_CALL vkGetImageSparseMemoryRequirements2KHR(VkDevice d,
    const VkImageSparseMemoryRequirementsInfo2 *info, uint32_t *count,
    VkSparseImageMemoryRequirements2 *out)
{
    (void)out;
    if (!count) return;
    /* No image is created with sparse binding (sparseBinding is not reported),
     * so, as for the 1.0 query, every valid image has no sparse requirements. */
    *count = 0;
    (void)d; (void)info;
}

/* VK_KHR_bind_memory2. Every element is validated before any binding
 * changes, so a refused call leaves every resource as it was (stronger than
 * the extension requires). The single-device group forms are the only chained
 * structures accepted: one physical device, index 0, no split-instance
 * regions (images cannot be created with SPLIT_INSTANCE_BIND_REGIONS). */
static VkResult bind_group_chain(VkDevice d, const void *chain, VkStructureType type)
{
    VkBool32 seen = VK_FALSE;
    for (const VkBaseInStructure *next = chain; next; next = next->pNext) {
        if (next->sType != type || !d->device_group_extension_enabled)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (seen) return INVALID;
        seen = VK_TRUE;
        uint32_t count; const uint32_t *indices;
        if (type == VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO) {
            const VkBindBufferMemoryDeviceGroupInfo *group = (const void *)next;
            count = group->deviceIndexCount; indices = group->pDeviceIndices;
        } else {
            const VkBindImageMemoryDeviceGroupInfo *group = (const void *)next;
            if (group->splitInstanceBindRegionCount) return INVALID;
            count = group->deviceIndexCount; indices = group->pDeviceIndices;
        }
        if (count > 1 || (count == 1 && (!indices || indices[0] != 0))) return INVALID;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2KHR(VkDevice d, uint32_t count,
    const VkBindBufferMemoryInfo *infos)
{
    if (!d || !d->bind_memory2_extension_enabled || !count || !infos) return INVALID;
    for (uint32_t n = 0; n < count; ++n) {
        const VkBindBufferMemoryInfo *info = &infos[n];
        if (info->sType != VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO) return INVALID;
        VkResult result = bind_group_chain(d, info->pNext,
            VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO);
        if (result == VK_SUCCESS)
            result = buffer_bind_check(d, info->buffer, info->memory, info->memoryOffset);
        if (result != VK_SUCCESS) return result;
        for (uint32_t k = 0; k < n; ++k)
            if (infos[k].buffer == info->buffer) return INVALID;
    }
    for (uint32_t n = 0; n < count; ++n)
        (void)vkBindBufferMemory(d, infos[n].buffer, infos[n].memory, infos[n].memoryOffset);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2KHR(VkDevice d, uint32_t count,
    const VkBindImageMemoryInfo *infos)
{
    if (!d || !d->bind_memory2_extension_enabled || !count || !infos) return INVALID;
    for (uint32_t n = 0; n < count; ++n) {
        const VkBindImageMemoryInfo *info = &infos[n];
        if (info->sType != VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO) return INVALID;
        /* VkBindImagePlaneMemoryInfo (disjoint planes) and
         * VkBindImageMemorySwapchainInfoKHR (swapchain-backed binding) are
         * refused: neither kind of image is created by this profile's
         * vkCreateImage. */
        VkResult result = bind_group_chain(d, info->pNext,
            VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO);
        if (result == VK_SUCCESS)
            result = image_bind_check(d, info->image, info->memory, info->memoryOffset);
        if (result != VK_SUCCESS) return result;
        for (uint32_t k = 0; k < n; ++k)
            if (infos[k].image == info->image) return INVALID;
    }
    for (uint32_t n = 0; n < count; ++n)
        (void)vkBindImageMemory(d, infos[n].image, infos[n].memory, infos[n].memoryOffset);
    return VK_SUCCESS;
}

/* VK_KHR_maintenance4: requirements for a creation description without a
 * resource. The answer is, by construction, exactly what vkCreateBuffer or
 * vkCreateImage followed by the 1.0 query reports: a transient object is
 * created from the same description, queried and destroyed before return (it
 * is never visible to the application and cannot be referenced). A
 * description those commands refuse has no requirements, so the query
 * reports zero, including a memoryTypeBits of 0. */
VKAPI_ATTR void VKAPI_CALL vkGetDeviceBufferMemoryRequirementsKHR(VkDevice d,
    const VkDeviceBufferMemoryRequirements *info, VkMemoryRequirements2 *out)
{
    if (!out || out->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2) return;
    out->memoryRequirements = (VkMemoryRequirements){0};
    if (!d || !d->maintenance4_extension_enabled || !info ||
        info->sType != VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS ||
        info->pNext || !info->pCreateInfo) return;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(d, info->pCreateInfo, NULL, &buffer) != VK_SUCCESS) return;
    vkGetBufferMemoryRequirements(d, buffer, &out->memoryRequirements);
    vkDestroyBuffer(d, buffer, NULL);
    dedicated_requirements(d, out->pNext);
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirementsKHR(VkDevice d,
    const VkDeviceImageMemoryRequirements *info, VkMemoryRequirements2 *out)
{
    if (!out || out->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2) return;
    out->memoryRequirements = (VkMemoryRequirements){0};
    /* planeAspect names a plane of a disjoint image, which is never created. */
    if (!d || !d->maintenance4_extension_enabled || !info ||
        info->sType != VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS ||
        info->pNext || !info->pCreateInfo || info->planeAspect) return;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(d, info->pCreateInfo, NULL, &image) != VK_SUCCESS) return;
    vkGetImageMemoryRequirements(d, image, &out->memoryRequirements);
    vkDestroyImage(d, image, NULL);
    dedicated_requirements(d, out->pNext);
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSparseMemoryRequirementsKHR(VkDevice d,
    const VkDeviceImageMemoryRequirements *info, uint32_t *count,
    VkSparseImageMemoryRequirements2 *out)
{
    (void)d; (void)info; (void)out;
    /* sparseBinding is not reported: no description can be sparse. */
    if (count) *count = 0;
}
