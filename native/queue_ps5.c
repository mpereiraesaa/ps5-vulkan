#include "vk_queue.h"
#include "descriptor_encode.h"
#include "dispatch_encode.h"
#include "ps5_platform.h"
#include "ps5_agc_driver.h"
#include "ps5log.h"
#include "submit_suspend_ps5.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { COMMAND_BYTES = 131072, ALIGNMENT = 65536, MAX_DISPATCHES =
       PS5VK_MAX_OPERATIONS * PS5VK_MAX_SUBMITTED_BUFFERS };
struct prepared_dispatch {
    void *arena, *backing;
    size_t bytes, words;
    uint32_t packet[PS5VK_COMPUTE_COMMAND_CAPACITY + 8];
};
struct native_job {
    uint64_t serial;
    void *command;
    int64_t physical;
    unsigned reserved, mapped, count, completed, attempted;
    struct prepared_dispatch dispatches[MAX_DISPATCHES];
};
static void cache(const void *address, size_t size)
{
    uintptr_t end = (uintptr_t)address + size;
    for (uintptr_t p = (uintptr_t)address & ~(uintptr_t)63; p < end; p += 64)
        __asm__ volatile("clflush (%0)" : : "r"(p) : "memory");
    __asm__ volatile("mfence" : : : "memory");
}
static uint64_t clock_ns(void *unused)
{
    (void)unused; struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}
static void pause_ns(void *unused, uint64_t remaining)
{
    (void)unused;
    usleep(remaining < 1000000 ? 1 : 1000);
}
static void retain(const char *reason)
{
    ps5log_printf(PS5LOG_ERR, "PS5VK_QUEUE_RETAIN reason=%s", reason);
    ps5log_close("queue-ownership-retained");
    for (;;) sleep(1);
}
static void release(VkDevice device, void *opaque)
{
    struct native_job *job = opaque;
    if (job->attempted && job->completed != job->count) retain("release-inflight");
    if (job->mapped) {
        struct ps5_batch_map_entry e = {job->command, 0, COMMAND_BYTES, 0xf2, 0x0c, 0, 1};
        int processed = 0;
        if (sceKernelBatchMap(&e, 1, &processed) || processed != 1)
            retain("command-unmap");
    }
    if (job->physical >= 0 && sceKernelReleaseDirectMemory(job->physical, COMMAND_BYTES))
        retain("command-physical-release");
    if (job->reserved && sceKernelMunmap(job->command, COMMAND_BYTES))
        retain("command-reservation-release");
    for (unsigned i = 0; i < job->count; ++i)
        if (job->dispatches[i].backing)
            device->memory.release(device->memory.context, job->dispatches[i].backing);
    free(job);
}
static VkResult prepare(VkDevice device, const struct ps5vk_submission *submission, void **out)
{
    *out = NULL;
    /* Reserve low bits for exact per-dispatch tokens. No token truncation. */
    if (!submission->serial || submission->serial > UINT32_MAX) return VK_ERROR_UNKNOWN;
    struct native_job *job = calloc(1, sizeof(*job));
    if (!job) return VK_ERROR_OUT_OF_HOST_MEMORY;
    job->physical = -1; job->serial = submission->serial;
    VkResult result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (sceKernelReserveVirtualRange(&job->command, COMMAND_BYTES, 0, ALIGNMENT)) goto fail;
    job->reserved = 1;
    if (sceKernelAllocateMainDirectMemory(COMMAND_BYTES, ALIGNMENT, 0x0c, &job->physical)) goto fail;
    struct ps5_batch_map_entry entry = {job->command, job->physical, COMMAND_BYTES, 0xf2, 0x0c, 0, 0};
    int processed = 0;
    int map_rc = sceKernelBatchMap(&entry, 1, &processed);
    /* Partial mapping is not safe to unwind as though no mapping existed. */
    if (map_rc || processed != 1) retain("command-map-ambiguous");
    job->mapped = 1;
    for (unsigned b = 0; b < submission->count; ++b) {
        VkCommandBuffer cb = submission->buffers[b];
        for (unsigned i = 0; i < cb->operation_count; ++i) {
            const struct ps5vk_operation *op = &cb->operations[i];
            /* Each dispatch is fully retired before the next. This is stronger
             * than supported global host/compute barriers, not a skipped GPU
             * dependency. No image or range-barrier semantics are advertised. */
            if (op->type == PS5VK_BARRIER) continue;
            if (job->count == MAX_DISPATCHES) { result = VK_ERROR_UNKNOWN; goto fail; }
            struct prepared_dispatch *p = &job->dispatches[job->count++];
            const struct ps5vk_compiled_program *program = &op->pipeline->program;
            if (!program->code_words || program->code_words > 1024 * 1024) {
                result = VK_ERROR_UNKNOWN; goto fail;
            }
            size_t code_bytes = program->code_words * 4;
            size_t table_offset = (code_bytes + 255) & ~(size_t)255;
            p->bytes = table_offset + 512;
            result = device->memory.allocate(device->memory.context, p->bytes, &p->arena, &p->backing);
            if (result != VK_SUCCESS) goto fail;
            memcpy(p->arena, program->code, code_bytes);
            uint32_t *table = (void *)((unsigned char *)p->arena + table_offset);
            memset(table, 0, 512);
            result = ps5vk_descriptor_encode(device, program, op->set, table, 128);
            if (result != VK_SUCCESS) goto fail;
            struct ps5vk_dispatch_encoding encoding = {.program = program,
                .addresses = {(uintptr_t)p->arena, (uintptr_t)table,
                    (uintptr_t)job->command + 0x1100, (uintptr_t)job->command + 0xff4},
                .completion_value = (job->serial << 32) | job->count};
            memcpy(encoding.groups, op->groups, sizeof(encoding.groups));
            /* Mesa ac_emit_cp_acquire_mem GFX10 compute form + pkt3 GCR_CNTL:
             * invalidate instruction/scalar/vector/L1/L2 caches globally.
             * No WB here: previous dispatch RELEASE_MEM already wrote back,
             * and host noncoherent writes require explicit vkFlush*. */
            const uint32_t acquire[8] = {0xc0065800, 0, 0xffffffff, 0xff, 0, 0, 10, 0x4381};
            memcpy(p->packet, acquire, sizeof(acquire));
            p->words = ps5vk_dispatch_encode(p->packet + 8, PS5VK_COMPUTE_COMMAND_CAPACITY, &encoding);
            if (!p->words) { result = VK_ERROR_UNKNOWN; goto fail; }
            p->words += 8;
            cache(p->arena, p->bytes);
        }
    }
    *out = job;
    ps5log_printf(PS5LOG_MARK, "PS5VK_QUEUE_PREPARED serial=%llu dispatches=%u",
                  (unsigned long long)job->serial, job->count);
    return VK_SUCCESS;
fail:
    release(device, job);
    return result;
}
static VkResult launch(VkDevice device, void *opaque)
{
    (void)device; struct native_job *job = opaque;
    /* Synchronous conservative submission, not simulated execution. Each
     * actual DCB must produce its own token before command memory is reused. */
    for (unsigned i = 0; i < job->count; ++i) {
        struct prepared_dispatch *p = &job->dispatches[i];
        volatile uint64_t *label = (void *)((unsigned char *)job->command + 0x1100);
        uint32_t *readback = (void *)((unsigned char *)job->command + 0xff4);
        memset(job->command, 0, COMMAND_BYTES);
        memcpy(job->command, p->packet, p->words * 4);
        readback[3] = 0xdeadbeef;
        cache(job->command, COMMAND_BYTES);
        struct ps5_agc_submit submit = {job->command, (uint32_t)p->words, 0, {0, 0, 0}};
        job->attempted = 1;
        struct ps5vk_submit_result result=ps5vk_submit_suspend(&submit);
        int rc = result.submit_rc;
        ps5log_printf(PS5LOG_MARK, "PS5VK_QUEUE_SUBMIT serial=%llu index=%u rc=%d",
                      (unsigned long long)job->serial, i, rc);
        if (rc) return VK_ERROR_DEVICE_LOST;
        ps5log_printf(PS5LOG_MARK, "PS5VK_QUEUE_SUSPEND_POINT serial=%llu index=%u rc=%d",
                      (unsigned long long)job->serial, i, result.suspend_rc);
        if (result.suspend_rc) return VK_ERROR_DEVICE_LOST;
        uint64_t token = (job->serial << 32) | (i + 1);
        uint64_t start = clock_ns(NULL);
        for (;;) {
            cache((const void *)label, 8);
            uint64_t observed = __atomic_load_n(label, __ATOMIC_ACQUIRE);
            if (observed == token) break;
            if (observed || !start || clock_ns(NULL) - start > UINT64_C(3000000000))
                return VK_ERROR_DEVICE_LOST;
            usleep(1000);
        }
        cache(readback, 16);
        if (readback[3] != 0) return VK_ERROR_DEVICE_LOST;
        ++job->completed;
        ps5log_printf(PS5LOG_MARK, "PS5VK_QUEUE_COMPLETED serial=%llu index=%u token=%llx gcr=0070f528",
                      (unsigned long long)job->serial, i, (unsigned long long)token);
    }
    return VK_SUCCESS;
}
static VkResult poll(VkDevice device, void *opaque, uint64_t *completed)
{
    (void)device; struct native_job *job = opaque;
    *completed = job->completed == job->count ? job->serial : 0;
    return VK_SUCCESS;
}
void ps5vk_native_queue_configure(VkDevice device)
{
    device->graphics_submit_enabled = VK_FALSE;
    device->submit_backend = (struct ps5vk_queue_backend){prepare, launch, poll, release};
    device->progress = (struct ps5vk_progress){NULL, ps5vk_queue_poll, clock_ns, pause_ns};
}
