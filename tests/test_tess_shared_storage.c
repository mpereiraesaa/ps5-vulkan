#include "tess_shared_storage.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static unsigned allocations, releases, initializations;
static int fail_allocate, fail_init, null_address;
static int owner1, owner2;
static VkResult allocate(void *ctx, VkDeviceSize bytes, void **address, void **backing)
{
    (void)ctx;
    if (fail_allocate) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *backing = malloc((size_t)bytes);
    assert(*backing);
    *address = null_address ? NULL : *backing;
    ++allocations;
    return VK_SUCCESS;
}
static void release(void *ctx, void *backing)
{ (void)ctx; ++releases; free(backing); }
static VkResult flush(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static const struct ps5vk_memory_backend memory = {
    .allocate = allocate, .release = release, .flush = flush
};
static VkResult initialize(const struct ps5vk_memory_backend *m,
                          void *address, void *backing, VkDeviceSize bytes)
{
    ++initializations;
    if (fail_init) return VK_ERROR_MEMORY_MAP_FAILED;
    memset(address, 0x5a, (size_t)bytes);
    return m->flush(m->context, backing, 0, bytes);
}
static void *worker(void *expected)
{
    for (unsigned i=0; i<1000; ++i) {
        struct ps5vk_tess_storage *p = NULL;
        assert(ps5vk_tess_storage_acquire(&owner1, &memory, 256, initialize, &p) == VK_SUCCESS);
        assert(p == expected);
        assert(((unsigned char *)ps5vk_tess_storage_address(p))[0] == 0x31);
        ps5vk_tess_storage_release(&p);
        assert(!p);
    }
    return NULL;
}
int main(void)
{
    struct ps5vk_tess_storage *a=NULL, *b=NULL, *c=NULL;
    assert(ps5vk_tess_storage_acquire(NULL,&memory,256,initialize,&a)!=VK_SUCCESS);
    fail_allocate=1;
    assert(ps5vk_tess_storage_acquire(&owner1,&memory,256,initialize,&a)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
    assert(!a && !allocations && !releases);
    fail_allocate=0; null_address=1;
    assert(ps5vk_tess_storage_acquire(&owner1,&memory,256,initialize,&a)==VK_ERROR_MEMORY_MAP_FAILED);
    assert(!a && allocations==releases);
    null_address=0; fail_init=1;
    assert(ps5vk_tess_storage_acquire(&owner1,&memory,256,initialize,&a)==VK_ERROR_MEMORY_MAP_FAILED);
    assert(!a && allocations==releases);
    fail_init=0;
    assert(ps5vk_tess_storage_acquire(&owner1,&memory,256,initialize,&a)==VK_SUCCESS);
    unsigned count=allocations, initialized=initializations;
    assert(ps5vk_tess_storage_acquire(&owner1,&memory,256,initialize,&b)==VK_SUCCESS);
    assert(a==b && allocations==count && initializations==initialized);
    /* Creating another pipeline must not zero data used by the first one. */
    ((unsigned char *)ps5vk_tess_storage_address(a))[0]=0x31;
    assert(ps5vk_tess_storage_acquire(&owner1,&memory,512,initialize,&c)!=VK_SUCCESS && !c);
    struct ps5vk_memory_backend different=memory;
    different.context=&owner2;
    assert(ps5vk_tess_storage_acquire(&owner1,&different,256,initialize,&c)!=VK_SUCCESS && !c);
    pthread_t threads[8];
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],NULL,worker,a));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
    assert(allocations==count && initializations==initialized);
    assert(ps5vk_tess_storage_acquire(&owner2,&memory,256,initialize,&c)==VK_SUCCESS);
    assert(c!=a && ps5vk_tess_storage_address(c)!=ps5vk_tess_storage_address(a));
    ps5vk_tess_storage_release(&a);
    assert(((unsigned char *)ps5vk_tess_storage_address(b))[0]==0x31);
    ps5vk_tess_storage_release(&b);
    ps5vk_tess_storage_release(&b);
    ps5vk_tess_storage_release(&c);
    assert(allocations==releases);
    assert(ps5vk_tess_storage_acquire(&owner1,&memory,256,initialize,&a)==VK_SUCCESS);
    assert(((unsigned char *)ps5vk_tess_storage_address(a))[0]==0x5a);
    ps5vk_tess_storage_release(&a);
    assert(allocations==releases);
    return 0;
}
