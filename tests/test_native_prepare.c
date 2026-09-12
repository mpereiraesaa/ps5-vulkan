#include "vk_queue.h"
#include "ps5_platform.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

/* Runs native prepare/release with explicit host syscall doubles. No DCB is
 * submitted and no shader is executed or computed on the CPU. */
static unsigned reservations, physicals, mappings, arenas, attempts, submissions;
static unsigned fail_reserve, fail_physical, fail_arena;
int ps5log_printf(const char *level, const char *format, ...)
{ (void)level; (void)format; return 0; }
void ps5log_close(const char *reason) { (void)reason; abort(); }
VkResult ps5vk_queue_poll(VkDevice d) { (void)d; abort(); }
int32_t sceAgcDriverSubmitDcb(void *d) { (void)d; ++submissions; abort(); }
int32_t sceAgcSuspendPoint(void) { abort(); }
int sceKernelReserveVirtualRange(void **address, size_t bytes, int flags, size_t alignment)
{
    assert(!flags); if (fail_reserve) return -1;
    *address = aligned_alloc(alignment, bytes); assert(*address); ++reservations; return 0;
}
int sceKernelAllocateMainDirectMemory(size_t bytes, size_t alignment, int type, int64_t *offset)
{
    assert(bytes == 131072 && alignment == 65536 && type == 0x0c);
    if (fail_physical) return -1;
    *offset = 65536; ++physicals; return 0;
}
int sceKernelBatchMap(void *entries, int count, int *processed)
{
    struct ps5_batch_map_entry *e = entries;
    assert(count == 1 && e->length == 131072);
    if (e->operation == 0) ++mappings;
    else { assert(e->operation == 1 && mappings); --mappings; }
    *processed = 1; return 0;
}
int sceKernelMunmap(void *address, size_t bytes)
{ assert(bytes == 131072 && reservations); --reservations; free(address); return 0; }
int sceKernelReleaseDirectMemory(int64_t offset, size_t bytes)
{ assert(offset == 65536 && bytes == 131072 && physicals); --physicals; return 0; }
static VkResult allocate(void *ctx, VkDeviceSize bytes, void **address, void **backing)
{
    (void)ctx; assert(bytes < 65536); ++attempts;
    if (fail_arena == attempts) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *address = *backing = aligned_alloc(65536, 65536); assert(*address); ++arenas;
    return VK_SUCCESS;
}
static void release(void *ctx, void *backing)
{ (void)ctx; assert(arenas); --arenas; free(backing); }
VkResult ps5vk_buffer_span(VkDevice d, VkBuffer b, VkDeviceSize off,
                          VkDeviceSize range, void **address, VkDeviceSize *bytes)
{ (void)d; *address = (void *)((uintptr_t)b + off); *bytes = range; return VK_SUCCESS; }
static void clean(void)
{ assert(!arenas && !mappings && !physicals && !reservations && !submissions); }
int main(void)
{
    struct VkDevice_T device = {.memory = {.allocate = allocate, .release = release}};
    ps5vk_native_queue_configure(&device);
    struct VkDescriptorPool_T pool = {.device = &device};
    struct VkDescriptorSet_T set = {.pool = &pool, .defined = {VK_TRUE}};
    set.signature.binding[0] = (struct ps5vk_binding){1, 0, VK_SHADER_STAGE_COMPUTE_BIT};
    set.buffers[0] = (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)0x100004000, 256, 4096};
    uint32_t code[80] = {0}; /* Packet metadata fixture, never executed. */
    struct VkPipeline_T *pipeline = calloc(1, sizeof(*pipeline)); assert(pipeline);
    pipeline->program = (struct ps5vk_compiled_program){.code = code, .code_words = 80,
        .gfx = 1013, .wave_size = 32, .local_size = {64, 1, 1}, .vgprs = 3,
        .sgprs = 10, .user_sgprs = 2, .descriptor_count = 1};
    struct VkCommandBuffer_T cb = {.operation_count = 2};
    for (unsigned i = 0; i < 2; ++i) cb.operations[i] = (struct ps5vk_operation){
        .type = PS5VK_DISPATCH, .pipeline = pipeline, .set = &set, .groups = {16, 1, 1}};
    struct ps5vk_submission submit = {.serial = 1, .count = 1, .buffers = {&cb}};
    void *job = NULL;
    fail_reserve = 1;
    assert(device.submit_backend.prepare(&device, &submit, &job) != VK_SUCCESS && !job); clean();
    fail_reserve = 0; fail_physical = 1;
    assert(device.submit_backend.prepare(&device, &submit, &job) != VK_SUCCESS && !job); clean();
    fail_physical = 0; fail_arena = 2;
    assert(device.submit_backend.prepare(&device, &submit, &job) != VK_SUCCESS && !job); clean();
    fail_arena = 0; set.defined[0] = VK_FALSE;
    assert(device.submit_backend.prepare(&device, &submit, &job) != VK_SUCCESS && !job); clean();
    set.defined[0] = VK_TRUE;
    assert(device.submit_backend.prepare(&device, &submit, &job) == VK_SUCCESS && job);
    assert(arenas == 2 && mappings == 1 && !submissions);
    device.submit_backend.release(&device, job); clean();
    free(pipeline);
    puts("Native prepare rollback: pass (host syscall doubles, no GPU execution)");
}
