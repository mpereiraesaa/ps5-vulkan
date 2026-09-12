#include "vk_internal.h"
#include "ps5_platform.h"
#include "ps5log.h"
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

struct native_allocation {
    void *address;
    int64_t physical;
    size_t mapped_size;
    VkDeviceSize requested_size;
};

/* Matches the bootstrap compute profile CPU/GPU-readable direct arena, not its special command mapping.
 * Command buffers and completion storage remain a separate backend resource. */
enum { DIRECT_ALIGNMENT = 65536 };

static void retain(const char *reason, int result)
{
    ps5log_printf(PS5LOG_ERR, "PS5VK_MEMORY_RETAIN reason=%s rc=%d", reason, result);
    /* An uncertain release is not reported as a successful destruction. Keep
     * the app parked for exact-title closure; do not release potentially live
     * physical memory or conceal the failure with a CPU fallback. */
    ps5log_close("memory-ownership-retained");
    for (;;) sleep(1);
}

static VkResult allocate_aligned(void *context, VkDeviceSize size, void **address, void **backing,
                                size_t alignment)
{
    struct ps5vk_native_memory_budget *budget = context;
    *address = NULL; *backing = NULL;
    if (!size || size > SIZE_MAX - (alignment - 1))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    struct native_allocation *a = calloc(1, sizeof(*a));
    if (!a) return VK_ERROR_OUT_OF_HOST_MEMORY;
    a->physical = -1; a->requested_size = size;
    a->mapped_size = ((size_t)size + alignment - 1) & ~(alignment - 1);
    if (budget && (budget->used > budget->limit || a->mapped_size > budget->limit - budget->used)) {
        free(a); return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    int result = sceKernelAllocateMainDirectMemory(a->mapped_size, alignment,
                                                  0x0c, &a->physical);
    if (result) {
        ps5log_printf(PS5LOG_INFO, "PS5VK_MEMORY_ALLOC_FAILED bytes=%zu rc=%d", a->mapped_size, result);
        free(a); return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    result = sceKernelMapDirectMemory(&a->address, a->mapped_size, 0x33, 0,
                                      a->physical, alignment);
    if (result) {
        int release_result = sceKernelReleaseDirectMemory(a->physical, a->mapped_size);
        if (release_result) retain("allocation-rollback", release_result);
        free(a); return VK_ERROR_MEMORY_MAP_FAILED;
    }
    if (!a->address || ((uintptr_t)a->address & (alignment - 1)) ||
        a->mapped_size > (UINT64_C(1) << 48) ||
        (uintptr_t)a->address > (UINT64_C(1) << 48) - a->mapped_size) {
        result = sceKernelMunmap(a->address, a->mapped_size);
        if (result) retain("invalid-address-unmap", result);
        result = sceKernelReleaseDirectMemory(a->physical, a->mapped_size);
        if (result) retain("invalid-address-release", result);
        free(a); return VK_ERROR_MEMORY_MAP_FAILED;
    }
    *address = a->address; *backing = a;
    if (budget) budget->used += a->mapped_size;
    ps5log_printf(PS5LOG_INFO, "PS5VK_MEMORY_ALLOC requested=%llu mapped=%zu",
                  (unsigned long long)size, a->mapped_size);
    return VK_SUCCESS;
}

static VkResult allocate(void *c, VkDeviceSize size, void **address, void **backing)
{ return allocate_aligned(c, size, address, backing, DIRECT_ALIGNMENT); }
static VkResult allocate_graphics(void *c, VkDeviceSize size, void **address, void **backing)
{ return allocate_aligned(c, size, address, backing, 131072); }

static void release(void *context, void *backing)
{
    struct ps5vk_native_memory_budget *budget = context;
    struct native_allocation *a = backing;
    int result = sceKernelMunmap(a->address, a->mapped_size);
    if (result) retain("unmap", result);
    result = sceKernelReleaseDirectMemory(a->physical, a->mapped_size);
    if (result) retain("physical-release", result);
    if (budget) {
        if (budget->used < a->mapped_size) retain("budget-underflow", -1);
        budget->used -= a->mapped_size;
    }
    ps5log_printf(PS5LOG_INFO, "PS5VK_MEMORY_RELEASE mapped=%zu", a->mapped_size);
    free(a);
}

static VkResult host_cache(void *context, void *backing, VkDeviceSize offset, VkDeviceSize size)
{
    (void)context;
    struct native_allocation *a = backing;
    if (!size || offset >= a->requested_size || size > a->requested_size - offset)
        return VK_ERROR_UNKNOWN;
    uintptr_t start = (uintptr_t)a->address + offset;
    uintptr_t end = start + size;
    for (uintptr_t line = start & ~(uintptr_t)63; line < end; line += 64)
        __asm__ volatile("clflush (%0)" : : "r"(line) : "memory");
    __asm__ volatile("mfence" : : : "memory");
    /* This handles CPU cache visibility only. Invalidate after GPU execution
     * requires the queue to complete bootstrap compute profile's GCR writeback BEFORE this function.
     * It is not a GPU fence, GPU cache operation or execution dependency. */
    return VK_SUCCESS;
}

struct ps5vk_memory_backend ps5vk_native_memory_backend(void)
{
    return (struct ps5vk_memory_backend){NULL, allocate, release, host_cache, host_cache};
}

struct ps5vk_memory_backend ps5vk_native_graphics_memory_backend(void)
{
    return (struct ps5vk_memory_backend){NULL, allocate_graphics, release, host_cache, host_cache};
}
