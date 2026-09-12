#include "command_arena_ps5.h"
#include "ps5_platform.h"
#include <assert.h>
#include <stdlib.h>
static unsigned reserves, physicals, maps, fail_physical;
int sceKernelReserveVirtualRange(void **a,size_t n,int flags,size_t alignment)
{ assert(!flags); *a=aligned_alloc(alignment,n); assert(*a); ++reserves; return 0; }
int sceKernelAllocateMainDirectMemory(size_t n,size_t align,int type,int64_t *p)
{ assert(n==131072 && align==65536 && type==0x0c); if(fail_physical)return -1; *p=65536; ++physicals; return 0; }
int sceKernelBatchMap(void *opaque,int n,int *processed)
{
    struct ps5_batch_map_entry *e=opaque;
    assert(n==1 && e->length==131072);
    if(e->operation==0) ++maps; else { assert(maps); --maps; }
    *processed=1; return 0;
}
int sceKernelReleaseDirectMemory(int64_t p,size_t n)
{ assert(p==65536 && n==131072 && physicals); --physicals; return 0; }
int sceKernelMunmap(void *a,size_t n)
{ assert(n==131072 && reserves); free(a); --reserves; return 0; }
int main(void)
{
    struct ps5vk_command_arena a={0};
    assert(!ps5vk_command_arena_label(&a));
    fail_physical=1;
    assert(ps5vk_command_arena_create(&a)==VK_ERROR_OUT_OF_DEVICE_MEMORY && !reserves && !a.address);
    fail_physical=0;
    assert(ps5vk_command_arena_create(&a)==VK_SUCCESS);
    volatile uint64_t *label=ps5vk_command_arena_label(&a);
    assert(label && !*label && (uintptr_t)label%64==0);
    assert((uintptr_t)label==(uintptr_t)a.address+4*PS5VK_COMMAND_ARENA_WORDS);
    assert(ps5vk_command_arena_create(&a)!=VK_SUCCESS);
    assert(ps5vk_command_arena_release(&a)==VK_SUCCESS);
    assert(!reserves && !physicals && !maps && !a.address);
}
