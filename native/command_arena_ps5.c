#include "command_arena_ps5.h"
#include "ps5_platform.h"
#include <string.h>
VkResult ps5vk_command_arena_release(struct ps5vk_command_arena *a)
{
    if (!a) return VK_ERROR_UNKNOWN;
    if (a->uncertain) return VK_ERROR_DEVICE_LOST;
    if (a->mapped) {
        struct ps5_batch_map_entry e={a->address,0,PS5VK_COMMAND_ARENA_BYTES,0xf2,0x0c,0,1};
        int processed=0;
        if (sceKernelBatchMap(&e,1,&processed) || processed!=1) goto uncertain;
        a->mapped=0;
    }
    if (a->allocated) {
        if (sceKernelReleaseDirectMemory(a->physical,PS5VK_COMMAND_ARENA_BYTES)) goto uncertain;
        a->allocated=0;
    }
    if (a->reserved) {
        if (sceKernelMunmap(a->address,PS5VK_COMMAND_ARENA_BYTES)) goto uncertain;
        a->reserved=0;
    }
    memset(a,0,sizeof(*a)); return VK_SUCCESS;
uncertain:
    a->uncertain=1; return VK_ERROR_DEVICE_LOST;
}
VkResult ps5vk_command_arena_create(struct ps5vk_command_arena *a)
{
    if (!a || a->address || a->reserved || a->allocated || a->mapped || a->uncertain) return VK_ERROR_UNKNOWN;
    if (sceKernelReserveVirtualRange(&a->address,PS5VK_COMMAND_ARENA_BYTES,0,PS5VK_COMMAND_ARENA_ALIGNMENT))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    a->reserved=1;
    if (!a->address || (uintptr_t)a->address % PS5VK_COMMAND_ARENA_ALIGNMENT ||
        (uintptr_t)a->address > (UINT64_C(1)<<48)-PS5VK_COMMAND_ARENA_BYTES) goto rollback;
    if (sceKernelAllocateMainDirectMemory(PS5VK_COMMAND_ARENA_BYTES,PS5VK_COMMAND_ARENA_ALIGNMENT,0x0c,&a->physical))
        goto rollback;
    a->allocated=1;
    struct ps5_batch_map_entry e={a->address,a->physical,PS5VK_COMMAND_ARENA_BYTES,0xf2,0x0c,0,0};
    int processed=0;
    if (sceKernelBatchMap(&e,1,&processed) || processed!=1) { a->uncertain=1; return VK_ERROR_DEVICE_LOST; }
    a->mapped=1; memset(a->address,0,PS5VK_COMMAND_ARENA_BYTES); return VK_SUCCESS;
rollback:
    return ps5vk_command_arena_release(a)==VK_SUCCESS ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_DEVICE_LOST;
}
volatile uint64_t *ps5vk_command_arena_label(struct ps5vk_command_arena *a)
{
    if (!a || !a->mapped || a->uncertain) return NULL;
    return (volatile uint64_t *)((unsigned char *)a->address+PS5VK_COMMAND_ARENA_BYTES-64);
}
