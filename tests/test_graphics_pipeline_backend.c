#include "graphics_pipeline_ps5.h"
#include <assert.h>
#include <stdlib.h>
static unsigned allocations, releases, flushes, fail_prepare, fail_flush;
static VkResult allocate(void *c,VkDeviceSize size,void **address,void **backing)
{ (void)c; *address=*backing=aligned_alloc(256,(size+255)&~255ull); assert(*address); ++allocations; return VK_SUCCESS; }
static void release(void *c,void *backing) { (void)c; free(backing); ++releases; }
static VkResult flush(void *c,void *b,VkDeviceSize off,VkDeviceSize bytes)
{ (void)c; assert(b && off==0 && bytes>704); ++flushes; return fail_flush ? VK_ERROR_UNKNOWN : VK_SUCCESS; }
int ps5vk_graphics_pair_prepare(struct ps5vk_graphics_pair *p,void *image,size_t capacity,
                                const struct ps5vk_graphics_pair_input *input)
{ assert(p && image && capacity==input->image_bytes); p->ready=!fail_prepare; return fail_prepare ? -1 : 0; }
int main(void)
{
    struct VkDevice_T device={.memory={NULL,allocate,release,flush,flush}};
    struct ps5vk_graphics_pair_input input={.image_bytes=704};
    void *state;
    assert(ps5vk_native_graphics_create(&device,&input,4u,&state)==VK_SUCCESS);
    assert(((struct ps5vk_native_graphics_pipeline *)state)->pair->ready && allocations==1 && !releases && flushes==1);
    struct ps5vk_native_graphics_pipeline *p=state;
    assert(p->global_table && !((uintptr_t)p->global_table & 15));
    assert((uintptr_t)p->global_table >= (uintptr_t)p->pair + sizeof(*p->pair));
    assert((uintptr_t)p->global_table + 16 == (uintptr_t)p->pair + p->allocation_bytes);
    assert(((uintptr_t)p->pair >> 32) == ((uintptr_t)p->global_table >> 32));
    for (unsigned j=0;j<4;++j) assert(!p->global_table[j]);
    ps5vk_native_graphics_release(&device,state); assert(releases==allocations);
    fail_prepare=1;
    assert(ps5vk_native_graphics_create(&device,&input,4u,&state)==VK_ERROR_INITIALIZATION_FAILED && !state);
    assert(releases==allocations && flushes==1);
    fail_prepare=0; fail_flush=1;
    assert(ps5vk_native_graphics_create(&device,&input,4u,&state)==VK_ERROR_UNKNOWN && !state);
    assert(releases==allocations && flushes==2);
    /* The offline library holds the audited triangle-list programs only, and a
     * strip pipeline cannot resolve to one; the backend refuses it instead of
     * linking a mismatched primitive. */
    fail_flush=0;
    unsigned before=allocations;
    assert(ps5vk_native_graphics_create(&device,&input,6u,&state)==
        VK_ERROR_FEATURE_NOT_PRESENT && !state && allocations==before);
    assert(ps5vk_native_graphics_create(&device,&input,5u,&state)==
        VK_ERROR_FEATURE_NOT_PRESENT && !state && allocations==before);
}
