#include "vk_internal.h"
#include "ps5_platform.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static size_t wanted, allocated;
static unsigned physicals, maps, fail_map, misalign;
static void *raw;
int ps5log_printf(const char *level, const char *format, ...)
{ (void)level; (void)format; return 0; }
void ps5log_close(const char *reason) { (void)reason; abort(); }
int sceKernelAllocateMainDirectMemory(size_t bytes, size_t alignment, int type, int64_t *offset)
{
    assert(alignment == wanted && bytes % wanted == 0 && type == 0x0c);
    allocated=bytes; ++physicals; *offset=wanted; return 0;
}
int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags,
                            int64_t offset, size_t alignment)
{
    assert(bytes == allocated && alignment == wanted && protection == 0x33 && !flags && offset == (int64_t)wanted);
    if (fail_map) return -1;
    raw=aligned_alloc(wanted, bytes + wanted); assert(raw);
    *address=(char *)raw + (misalign ? 64 : 0); ++maps; return 0;
}
int sceKernelMunmap(void *address, size_t bytes)
{ (void)address; assert(bytes == allocated && maps); --maps; free(raw); return 0; }
int sceKernelReleaseDirectMemory(int64_t offset, size_t bytes)
{ assert(offset == (int64_t)wanted && bytes == allocated && physicals); --physicals; return 0; }
int main(void)
{
    for (unsigned graphics=0; graphics<2; ++graphics) {
        wanted=graphics ? 131072 : 65536;
        struct ps5vk_native_memory_budget budget={wanted*2, 0};
        struct ps5vk_memory_backend backend=graphics ? ps5vk_native_graphics_memory_backend() : ps5vk_native_memory_backend();
        backend.context=&budget;
        void *address, *backing;
        assert(backend.allocate(&budget, 1, &address, &backing) == VK_SUCCESS);
        assert(budget.used == wanted && !((uintptr_t)address % wanted));
        backend.release(&budget, backing); assert(!budget.used && !maps && !physicals);
        assert(backend.allocate(&budget, wanted*2+1, &address, &backing) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        assert(!maps && !physicals);
        fail_map=1;
        assert(backend.allocate(&budget, 1, &address, &backing) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(!budget.used && !maps && !physicals); fail_map=0;
        misalign=1;
        assert(backend.allocate(&budget, 1, &address, &backing) == VK_ERROR_MEMORY_MAP_FAILED);
        assert(!budget.used && !maps && !physicals); misalign=0;
    }
    puts("Native 64/128 KiB allocation policy: host syscall doubles only");
}
