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
struct mock_gpu_address { void *backing; VkDeviceAddress address; };
static struct mock_gpu_address gpu_addresses[128];
static unsigned gpu_address_count;
static VkDeviceAddress next_gpu_address = UINT64_C(0x200000000);
static int refuse_gpu_address;
VkResult ps5vk_memory_backend_device_address(void *backing, VkDeviceAddress *out)
{
    if (out) *out = 0;
    if (refuse_gpu_address || !out) return VK_ERROR_FEATURE_NOT_PRESENT;
    for (unsigned n = 0; n < gpu_address_count; ++n)
        if (gpu_addresses[n].backing == backing) {
            *out = gpu_addresses[n].address;
            return VK_SUCCESS;
        }
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
static VkResult allocate(void *ctx, VkDeviceSize bytes, void **address, void **backing)
{
    struct mock *m = ctx;
    if (m->allocation_result) return m->allocation_result;
    *address = calloc(1, bytes); *backing = *address;
    if (!*address) return VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(gpu_address_count < sizeof(gpu_addresses) / sizeof(gpu_addresses[0]));
    gpu_addresses[gpu_address_count++] = (struct mock_gpu_address){
        *backing, next_gpu_address};
    next_gpu_address += UINT64_C(0x100000);
    ++m->allocations; return VK_SUCCESS;
}
static void release(void *ctx, void *backing)
{
    ++((struct mock *)ctx)->releases;
    for (unsigned n = 0; n < gpu_address_count; ++n)
        if (gpu_addresses[n].backing == backing) {
            gpu_addresses[n] = gpu_addresses[--gpu_address_count];
            break;
        }
    free(backing);
}
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
    /* D3D11 UAV buffer usage: storage texel next to storage buffer. */
    bi.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    assert(vkCreateBuffer(&d, &bi, NULL, &b) == VK_SUCCESS && b);
    assert(ps5vk_buffer_usage(&d, b, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT));
    assert(!ps5vk_buffer_usage(&d, b, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT));
    vkDestroyBuffer(&d, b, NULL);
    /* Transform feedback usage stays refused. */
    bi.usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT; b = VK_NULL_HANDLE;
    assert(vkCreateBuffer(&d, &bi, NULL, &b) == VK_ERROR_FEATURE_NOT_PRESENT && !b);
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
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
    /* Creation follows the witnessed mask.  The typed native matrix covers
     * every row below directly, across 1/2/4/8/16-byte elements. */
    const VkFormat witnessed[] = {
        VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
        VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM,
        VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_FORMAT_A8B8G8R8_SNORM_PACK32,
        VK_FORMAT_A8B8G8R8_UINT_PACK32, VK_FORMAT_A8B8G8R8_SINT_PACK32,
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
        VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM,
        VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT,
        VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UINT,
        VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32_SFLOAT,
        VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_SFLOAT,
        VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
        VK_FORMAT_R32G32B32A32_SFLOAT,
    };
    enum { WITNESSED_COUNT = sizeof(witnessed) / sizeof(witnessed[0]) };
    VkBufferView extra[WITNESSED_COUNT];
    memset(extra, 0, sizeof(extra));
    for (unsigned i = 0; i < sizeof(witnessed) / sizeof(witnessed[0]); ++i) {
        vi.format = witnessed[i]; vi.offset = 0;
        assert(vkCreateBufferView(&d, &vi, NULL, &extra[i]) == VK_SUCCESS && extra[i]);
    }
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    /* Negatives: no enabled uniform-texel role, unknown format, misaligned
     * offset, another device, and a buffer without the required usage. */
    const VkFormat refused[] = {VK_FORMAT_R8G8B8A8_SRGB,
                                VK_FORMAT_B8G8R8A8_UNORM,
                                VK_FORMAT_A8B8G8R8_SRGB_PACK32,
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
    /* A storage-texel-only buffer gets no view: no format carries the
     * witnessed storage-texel role yet. */
    VkBufferCreateInfo storage_info = bi;
    storage_info.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    VkBuffer storage_texel;
    assert(vkCreateBuffer(&d, &storage_info, NULL, &storage_texel) == VK_SUCCESS);
    assert(vkBindBufferMemory(&d, storage_texel, m, 1024) == VK_SUCCESS);
    vi.buffer = storage_texel;
#if defined(PS5VK_STORAGE_TEXEL_DIAGNOSTIC) && PS5VK_STORAGE_TEXEL_DIAGNOSTIC
    /* Measurement build: exactly the three storage-texel rows serve the role. */
    const VkFormat storage_formats[] = {VK_FORMAT_R32_UINT, VK_FORMAT_R8G8B8A8_UNORM,
                                        VK_FORMAT_R32G32B32A32_SFLOAT};
    for (unsigned i = 0; i < 3; ++i) {
        vi.format = storage_formats[i]; vi.offset = 0; vi.range = 64;
        assert(vkCreateBufferView(&d, &vi, NULL, &view) == VK_SUCCESS && view);
        vkDestroyBufferView(&d, view, NULL); view = VK_NULL_HANDLE;
    }
    /* A row with only the uniform role is refused for a storage buffer. */
    vi.format = VK_FORMAT_R32_SINT;
    assert(vkCreateBufferView(&d, &vi, NULL, &view) == VK_ERROR_FEATURE_NOT_PRESENT && !view);
    /* A buffer naming both roles needs both on the row. */
    VkBufferCreateInfo both_info = bi;
    both_info.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    VkBuffer both;
    assert(vkCreateBuffer(&d, &both_info, NULL, &both) == VK_SUCCESS);
    assert(vkBindBufferMemory(&d, both, m, 2048) == VK_SUCCESS);
    vi.buffer = both; vi.format = VK_FORMAT_R32_UINT;
    assert(vkCreateBufferView(&d, &vi, NULL, &view) == VK_SUCCESS && view);
    vkDestroyBufferView(&d, view, NULL); view = VK_NULL_HANDLE;
    vi.format = VK_FORMAT_R32_SINT;
    assert(vkCreateBufferView(&d, &vi, NULL, &view) == VK_ERROR_FEATURE_NOT_PRESENT && !view);
    vkDestroyBuffer(&d, both, NULL);
    vi.format = VK_FORMAT_R32_UINT; vi.offset = 4; vi.range = VK_WHOLE_SIZE;
#else
    assert(vkCreateBufferView(&d, &vi, NULL, &view) == VK_ERROR_FEATURE_NOT_PRESENT && !view);
#endif
    vkDestroyBuffer(&d, storage_texel, NULL);
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

static void test_buffer_device_address(void)
{
    struct mock state = {0};
    struct VkDevice_T d = device(&state), other = device(&state);
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 256, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags, .allocationSize = 1024};
    VkBuffer b = VK_NULL_HANDLE;
    VkDeviceMemory m = VK_NULL_HANDLE;
    assert(vkCreateBuffer(&d, &buffer_info, NULL, &b) == VK_ERROR_FEATURE_NOT_PRESENT && !b);
    assert(vkAllocateMemory(&d, &memory_info, NULL, &m) == VK_ERROR_FEATURE_NOT_PRESENT && !m);
    d.enabled_features = PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS;
    assert(vkCreateBuffer(&d, &buffer_info, NULL, &b) == VK_SUCCESS);
    VkBufferDeviceAddressInfo address_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b};
    assert(!vkGetBufferDeviceAddressKHR(&d, &address_info)); /* Unbound. */
    VkDeviceMemory plain = memory(&d, 1024);
    assert(vkBindBufferMemory(&d, b, plain, 256) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(!vkGetBufferDeviceAddressKHR(&d, &address_info));
    flags.flags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
    assert(vkAllocateMemory(&d, &memory_info, NULL, &m) == VK_ERROR_FEATURE_NOT_PRESENT && !m);
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryOpaqueCaptureAddressAllocateInfo capture = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO,
        .opaqueCaptureAddress = 1};
    flags.pNext = &capture;
    assert(vkAllocateMemory(&d, &memory_info, NULL, &m) == VK_ERROR_FEATURE_NOT_PRESENT && !m);
    flags.pNext = NULL;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT |
                  VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT_KHR;
    flags.deviceMask = 1;
    assert(vkAllocateMemory(&d, &memory_info, NULL, &m) == VK_ERROR_FEATURE_NOT_PRESENT && !m);
    d.device_group_extension_enabled = VK_TRUE;
    flags.deviceMask = 0;
    assert(vkAllocateMemory(&d, &memory_info, NULL, &m) == VK_ERROR_FEATURE_NOT_PRESENT && !m);
    flags.deviceMask = 2;
    assert(vkAllocateMemory(&d, &memory_info, NULL, &m) == VK_ERROR_FEATURE_NOT_PRESENT && !m);
    flags.deviceMask = 1;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT_KHR;
    assert(vkAllocateMemory(&d, &memory_info, NULL, &m) == VK_SUCCESS);
    vkFreeMemory(&d, m, NULL);
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    assert(vkAllocateMemory(&d, &memory_info, NULL, &m) == VK_ERROR_FEATURE_NOT_PRESENT && !m);
    flags.deviceMask = 0;
    refuse_gpu_address = 1;
    unsigned releases = state.releases;
    assert(vkAllocateMemory(&d, &memory_info, NULL, &m) == VK_ERROR_FEATURE_NOT_PRESENT && !m);
    assert(state.releases == releases + 1); /* Backend allocation was rolled back. */
    refuse_gpu_address = 0;
    assert(vkAllocateMemory(&d, &memory_info, NULL, &m) == VK_SUCCESS);
    assert(vkBindBufferMemory(&d, b, m, 256) == VK_SUCCESS);
    VkDeviceAddress address = vkGetBufferDeviceAddressKHR(&d, &address_info);
    assert(address);
    void *cpu_map = NULL;
    assert(vkMapMemory(&d, m, 0, VK_WHOLE_SIZE, 0, &cpu_map) == VK_SUCCESS);
    assert(address != (VkDeviceAddress)(uintptr_t)((unsigned char *)cpu_map + 256));
    VkDeviceAddress base = 0;
    assert(ps5vk_memory_backend_device_address(cpu_map, &base) == VK_SUCCESS);
    assert(address == base + 256);
    assert(!vkGetBufferDeviceAddressKHR(&other, &address_info));
    VkBufferDeviceAddressInfo malformed = address_info;
    malformed.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    assert(!vkGetBufferDeviceAddressKHR(&d, &malformed));
    VkBuffer second = VK_NULL_HANDLE;
    assert(vkCreateBuffer(&d, &buffer_info, NULL, &second) == VK_SUCCESS);
    assert(vkBindBufferMemory(&d, second, m, 512) == VK_SUCCESS);
    address_info.buffer = second;
    assert(vkGetBufferDeviceAddressKHR(&d, &address_info) == base + 512);
    assert(vkGetBufferDeviceAddressKHR(&d, &address_info) != address);
    VkBuffer independent = VK_NULL_HANDLE;
    VkDeviceMemory independent_memory = VK_NULL_HANDLE;
    assert(vkCreateBuffer(&d, &buffer_info, NULL, &independent) == VK_SUCCESS);
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT |
                  VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT_KHR;
    flags.deviceMask = 1;
    assert(vkAllocateMemory(&d, &memory_info, NULL, &independent_memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(&d, independent, independent_memory, 0) == VK_SUCCESS);
    address_info.buffer = independent;
    VkDeviceAddress independent_address = vkGetBufferDeviceAddressKHR(&d, &address_info);
    assert(independent_address && independent_address != base &&
           independent_address != base + 256 && independent_address != base + 512);
    vkDestroyBuffer(&d, independent, NULL);
    vkFreeMemory(&d, independent_memory, NULL);
    vkDestroyBuffer(&d, b, NULL);
    address_info.buffer = b;
    assert(!vkGetBufferDeviceAddressKHR(&d, &address_info)); /* Destroyed handle. */
    vkFreeMemory(&d, m, NULL);
    address_info.buffer = second;
    assert(!vkGetBufferDeviceAddressKHR(&d, &address_info)); /* Backing freed. */
    assert(vkBindBufferMemory(&d, second, plain, 0) != VK_SUCCESS); /* No rebind. */
    vkDestroyBuffer(&d, second, NULL);
    vkFreeMemory(&d, plain, NULL);
    assert(state.allocations == state.releases);
}
int main(void)
{
    test_binding(); test_mapping(); test_failures_and_allocators(); test_buffer_views();
    test_commitment();
    test_buffer_device_address();
    puts("Vulkan memory contracts: pass (host mock only, no GPU evidence)");
    return 0;
}
