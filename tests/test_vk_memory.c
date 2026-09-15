#include "vk_internal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mock {
    unsigned allocations, releases, flushes, invalidates;
    VkDeviceSize offset, size;
    VkResult allocation_result, sync_result;
};
static VkResult allocate(void *ctx, VkDeviceSize bytes, void **address, void **backing)
{
    struct mock *m = ctx;
    if (m->allocation_result) return m->allocation_result;
    *address = calloc(1, bytes); *backing = *address;
    if (!*address) return VK_ERROR_OUT_OF_HOST_MEMORY;
    ++m->allocations; return VK_SUCCESS;
}
static void release(void *ctx, void *backing)
{ ++((struct mock *)ctx)->releases; free(backing); }
static VkResult flush(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{
    assert(backing);
    struct mock *m = ctx; ++m->flushes; m->offset = offset; m->size = size;
    return m->sync_result;
}
static VkResult invalidate(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{
    assert(backing);
    struct mock *m = ctx; ++m->invalidates; m->offset = offset; m->size = size;
    return m->sync_result;
}
static struct VkDevice_T device(struct mock *mock)
{
    return (struct VkDevice_T){
        .memory = {mock, allocate, release, flush, invalidate},
        .buffer_alignment = 256, .noncoherent_atom = 64, .max_allocation = 65536
    };
}
static VkDeviceMemory memory(VkDevice d, VkDeviceSize size)
{
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = size};
    VkDeviceMemory m;
    assert(vkAllocateMemory(d, &info, NULL, &m) == VK_SUCCESS);
    return m;
}
static VkBuffer buffer(VkDevice d, VkDeviceSize size)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b;
    assert(vkCreateBuffer(d, &info, NULL, &b) == VK_SUCCESS);
    return b;
}
static void test_binding(void)
{
    struct mock mock = {0}; struct VkDevice_T d = device(&mock), other = device(&mock);
    VkDeviceMemory m = memory(&d, 4096);
    VkBuffer b = buffer(&d, 257);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(&d, b, &req);
    assert(req.size == 512 && req.alignment == 256 && req.memoryTypeBits == 1);
    assert(vkBindBufferMemory(&other, b, m, 0) != VK_SUCCESS);
    assert(vkBindBufferMemory(&d, b, m, UINT64_MAX - 255) != VK_SUCCESS);
    assert(vkBindBufferMemory(&d, b, m, 3840) != VK_SUCCESS);
    assert(vkBindBufferMemory(&d, b, m, 1) != VK_SUCCESS);
    assert(vkBindBufferMemory(&d, b, m, 512) == VK_SUCCESS);
    assert(vkBindBufferMemory(&d, b, m, 1024) != VK_SUCCESS);
    void *map, *span; VkDeviceSize size;
    assert(vkMapMemory(&d, m, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS);
    assert(ps5vk_buffer_span(&d, b, 12, VK_WHOLE_SIZE, &span, &size) == VK_SUCCESS);
    assert(span == (unsigned char *)map + 524 && size == 245);
    assert(ps5vk_buffer_span(&d, b, 0, 258, &span, &size) != VK_SUCCESS);
    assert(!span && !size);
    assert(ps5vk_buffer_span(&d, b, 257, VK_WHOLE_SIZE, &span, &size) != VK_SUCCESS);
    assert(ps5vk_buffer_span(&d, b, UINT64_MAX, 1, &span, &size) != VK_SUCCESS);
    vkFreeMemory(&d, m, NULL); /* Also releases an active mapping, no flush. */
    assert(mock.releases == 1 && !mock.flushes);
    assert(ps5vk_buffer_span(&d, b, 0, 1, &span, &size) != VK_SUCCESS);
    m = memory(&d, 4096);
    assert(vkBindBufferMemory(&d, b, m, 0) != VK_SUCCESS); /* No rebind after free. */
    vkDestroyBuffer(&d, b, NULL); vkFreeMemory(&d, m, NULL);
    assert(!d.buffers && !d.memories && mock.allocations == mock.releases);
}
static void test_mapping(void)
{
    struct mock mock = {0}; struct VkDevice_T d = device(&mock);
    VkDeviceMemory m = memory(&d, 1000); void *map;
    VkMappedMemoryRange r = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                             .memory = m, .size = VK_WHOLE_SIZE};
    assert(vkFlushMappedMemoryRanges(&d, 0, NULL) != VK_SUCCESS);
    assert(vkInvalidateMappedMemoryRanges(&d, 0, NULL) != VK_SUCCESS);
    assert(vkFlushMappedMemoryRanges(&d, 1, &r) != VK_SUCCESS);
    assert(vkMapMemory(&d, m, 1000, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS);
    assert(vkMapMemory(&d, m, 1, UINT64_MAX - 1, 0, &map) != VK_SUCCESS);
    assert(vkMapMemory(&d, m, 64, 128, 0, &map) == VK_SUCCESS);
    void *second;
    assert(vkMapMemory(&d, m, 0, 1, 0, &second) != VK_SUCCESS && !second);
    assert(vkFlushMappedMemoryRanges(&d, 1, &r) != VK_SUCCESS); /* Beyond mapped range. */
    r.offset = 64; r.size = 128;
    assert(vkFlushMappedMemoryRanges(&d, 1, &r) == VK_SUCCESS);
    assert(mock.offset == 64 && mock.size == 128 && mock.flushes == 1);
    r.offset = 65; r.size = 64;
    assert(vkFlushMappedMemoryRanges(&d, 1, &r) != VK_SUCCESS);
    r.offset = 64; r.size = 63;
    assert(vkInvalidateMappedMemoryRanges(&d, 1, &r) != VK_SUCCESS);
    r.size = 64;
    assert(vkInvalidateMappedMemoryRanges(&d, 1, &r) == VK_SUCCESS);
    assert(mock.invalidates == 1 && mock.offset == 64 && mock.size == 64);
    r.offset = 128; r.size = VK_WHOLE_SIZE;
    assert(vkInvalidateMappedMemoryRanges(&d, 1, &r) == VK_SUCCESS);
    assert(mock.invalidates == 2 && mock.offset == 128 && mock.size == 64);
    r.offset = 64;
    assert(vkInvalidateMappedMemoryRanges(&d, 1, &r) == VK_SUCCESS);
    assert(mock.size == 128); /* Mapping ends at 192, allocation ends at 1000. */
    r.size = 64;
    VkMappedMemoryRange ranges[2] = {r, r}; ranges[1].offset = 960;
    assert(vkFlushMappedMemoryRanges(&d, 2, ranges) != VK_SUCCESS && mock.flushes == 1);
    vkUnmapMemory(&d, m); assert(mock.flushes == 1);
    assert(vkMapMemory(&d, m, 64, 127, 0, &map) == VK_SUCCESS);
    r.offset = 64; r.size = VK_WHOLE_SIZE;
    assert(vkInvalidateMappedMemoryRanges(&d, 1, &r) != VK_SUCCESS); /* Non-atom mapping end. */
    vkUnmapMemory(&d, m);
    assert(vkMapMemory(&d, m, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS);
    r.offset = 960; r.size = VK_WHOLE_SIZE;
    assert(vkFlushMappedMemoryRanges(&d, 1, &r) == VK_SUCCESS);
    assert(mock.size == 40); /* Final non-atom-sized allocation tail is legal. */
    mock.sync_result = VK_ERROR_DEVICE_LOST;
    assert(vkInvalidateMappedMemoryRanges(&d, 1, &r) == VK_ERROR_DEVICE_LOST);
    vkFreeMemory(&d, m, NULL);
    assert(!d.memories && mock.allocations == mock.releases);
}
struct allocator_state { unsigned allocated, freed; int fail; };
static void *VKAPI_CALL host_alloc(void *ctx, size_t size, size_t alignment,
                                   VkSystemAllocationScope scope)
{
    struct allocator_state *s = ctx;
    assert(scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT && alignment <= _Alignof(max_align_t));
    if (s->fail) return NULL;
    ++s->allocated; return malloc(size);
}
static void *VKAPI_CALL host_realloc(void *ctx, void *p, size_t size, size_t alignment,
                                     VkSystemAllocationScope scope)
{ (void)ctx; (void)alignment; (void)scope; return realloc(p, size); }
static void VKAPI_CALL host_free(void *ctx, void *p)
{ ++((struct allocator_state *)ctx)->freed; free(p); }
static void test_failures_and_allocators(void)
{
    struct mock mock = {0}; struct VkDevice_T d = device(&mock);
    struct allocator_state state = {0};
    d.custom_allocator = VK_TRUE;
    d.allocator = (VkAllocationCallbacks){.pUserData = &state, .pfnAllocation = host_alloc,
        .pfnReallocation = host_realloc, .pfnFree = host_free};
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = 512};
    VkDeviceMemory m;
    state.fail = 1;
    assert(vkAllocateMemory(&d, &info, NULL, &m) == VK_ERROR_OUT_OF_HOST_MEMORY && !m);
    state.fail = 0; mock.allocation_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(vkAllocateMemory(&d, &info, NULL, &m) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !m);
    assert(state.allocated == state.freed && !d.memories && !mock.allocations);
    mock.allocation_result = VK_SUCCESS;
    m = memory(&d, 512); VkBuffer b = buffer(&d, 256);
    vkDestroyBuffer(&d, b, NULL); vkFreeMemory(&d, m, NULL);
    assert(state.allocated == state.freed && mock.allocations == mock.releases);
    info.allocationSize = UINT64_MAX;
    assert(vkAllocateMemory(&d, &info, NULL, &m) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    info.allocationSize = 512; info.memoryTypeIndex = 1;
    assert(vkAllocateMemory(&d, &info, NULL, &m) != VK_SUCCESS);
    info.memoryTypeIndex = 0; info.pNext = &info;
    assert(vkAllocateMemory(&d, &info, NULL, &m) == VK_ERROR_FEATURE_NOT_PRESENT);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 256, .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
    assert(vkCreateBuffer(&d, &bi, NULL, &b) == VK_SUCCESS && b);
    assert(ps5vk_buffer_usage(&d,b,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT));
    vkDestroyBuffer(&d,b,NULL);
    bi.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    assert(vkCreateBuffer(&d, &bi, NULL, &b) == VK_ERROR_FEATURE_NOT_PRESENT && !b);
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    assert(vkCreateBuffer(&d, &bi, NULL, &b) == VK_SUCCESS && b);
    assert(ps5vk_buffer_usage(&d, b, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    assert(!ps5vk_buffer_usage(&d, b, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    vkDestroyBuffer(&d, b, NULL);
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bi.size = UINT64_MAX;
    assert(vkCreateBuffer(&d, &bi, NULL, &b) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
}
static void test_buffer_views(void)
{
    struct mock mock = {0}; struct VkDevice_T d = device(&mock), other = device(&mock);
    VkDeviceMemory m = memory(&d, 4096);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 256, .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b;
    assert(vkCreateBuffer(&d, &bi, NULL, &b) == VK_SUCCESS);
    assert(vkBindBufferMemory(&d, b, m, 0) == VK_SUCCESS);
    VkBufferViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
        .buffer = b, .format = VK_FORMAT_R32_UINT, .offset = 4, .range = VK_WHOLE_SIZE};
    VkBufferView base = VK_NULL_HANDLE, view = VK_NULL_HANDLE;
    assert(vkCreateBufferView(&d, &vi, NULL, &base) == VK_SUCCESS && base);
    /* Creation follows the witnessed mask, so every row whose role passed its
     * console witness creates and nothing else does. The four RGBA8 rows are
     * witnessed by the two-run texelFetch witness recorded in VALIDATION.md. */
    const VkFormat witnessed[] = {
        VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32_SFLOAT,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
    };
    VkBufferView extra[7] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                             VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                             VK_NULL_HANDLE};
    for (unsigned i = 0; i < sizeof(witnessed) / sizeof(witnessed[0]); ++i) {
        vi.format = witnessed[i]; vi.offset = 0;
        assert(vkCreateBufferView(&d, &vi, NULL, &extra[i]) == VK_SUCCESS && extra[i]);
    }
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    /* Negatives: no implemented role (sRGB and BGRA), implemented but
     * UNWITNESSED roles (the four packed A8B8G8R8 rows, the four one-byte R8
     * rows, the four two-byte R8G8 rows, the five single-component R16 rows and
     * the five two-component R16G16 rows, none of which has a console fetch for
     * its element shape yet), unknown format, misaligned offset for that row,
     * another device, and a buffer that was not created with the
     * uniform-texel-buffer usage. */
    const VkFormat refused[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
                                VK_FORMAT_A8B8G8R8_UNORM_PACK32,
                                VK_FORMAT_A8B8G8R8_SNORM_PACK32,
                                VK_FORMAT_A8B8G8R8_UINT_PACK32,
                                VK_FORMAT_A8B8G8R8_SINT_PACK32,
                                VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM,
                                VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
                                VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM,
                                VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
                                VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM,
                                VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UINT,
                                VK_FORMAT_R16_SINT,
                                VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM,
                                VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UINT,
                                VK_FORMAT_R16G16_SINT,
                                (VkFormat)0x7fffffff};
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        vi.format = refused[i]; vi.offset = 0;
        assert(vkCreateBufferView(&d, &vi, NULL, &view) == VK_ERROR_FEATURE_NOT_PRESENT && !view);
    }
    /* The alignment bound is the texel size of the row itself, exercised on a
     * witnessed row so the refusal is about alignment and not about the role. */
    vi.format = VK_FORMAT_R32_UINT; vi.offset = 2;
    assert(vkCreateBufferView(&d, &vi, NULL, &view) == VK_ERROR_FEATURE_NOT_PRESENT && !view);
    vi.offset = 4;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    assert(vkCreateBufferView(&other, &vi, NULL, &view) == VK_ERROR_FEATURE_NOT_PRESENT && !view);
    VkBuffer plain = buffer(&d, 256);
    vi.buffer = plain;
    vi.format = VK_FORMAT_R32_UINT;
    assert(vkCreateBufferView(&d, &vi, NULL, &view) == VK_ERROR_FEATURE_NOT_PRESENT && !view);
    vkDestroyBuffer(&d, plain, NULL);
    for (unsigned i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i)
        vkDestroyBufferView(&d, extra[i], NULL);
    vkDestroyBufferView(&d, base, NULL);
    /* The buffer can only be destroyed once its views are gone. */
    vkDestroyBuffer(&d, b, NULL);
    vkFreeMemory(&d, m, NULL);
    assert(mock.allocations == mock.releases);
}

static void test_commitment(void)
{
    struct mock mock = {0}; struct VkDevice_T d = device(&mock);
    VkDeviceMemory m = memory(&d, 1024);
    VkDeviceSize committed = 1024;
    /* The physical profile exposes no lazily allocated memory type;
     * verify ordinary allocations are never reported as lazily committed. */
    vkGetDeviceMemoryCommitment(&d, m, &committed);
    assert(committed == 0);
    vkGetDeviceMemoryCommitment(&d, m, NULL); /* no crash */
    committed = 999;
    vkGetDeviceMemoryCommitment(NULL, m, &committed);
    assert(committed == 0);
    committed = 999;
    vkGetDeviceMemoryCommitment(&d, NULL, &committed);
    assert(committed == 0);
    struct VkDevice_T other_d = device(&mock);
    committed = 999;
    vkGetDeviceMemoryCommitment(&other_d, m, &committed);
    assert(committed == 0);
    vkFreeMemory(&d, m, NULL);
}
int main(void)
{
    test_binding(); test_mapping(); test_failures_and_allocators(); test_buffer_views();
    test_commitment();
    puts("Vulkan memory contracts: pass (host mock only, no GPU evidence)");
    return 0;
}
