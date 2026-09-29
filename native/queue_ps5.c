#include "vk_queue.h"
#include "vk_indirect.h"
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
       PS5VK_MAX_OPERATIONS * PS5VK_MAX_SUBMITTED_BUFFERS,
       /* Control words live past the command stream: a chained stream of many
        * dispatches must never reach the words its packets write. */
       READBACK_OFFSET = COMMAND_BYTES - 0x100, LABEL_OFFSET = COMMAND_BYTES - 0x80,
       STREAM_BYTES = COMMAND_BYTES - 0x200, WAIT_WORDS = 7 };
/* How long a submission polls its completion label before it sleeps between polls. */
#define SPIN_NS UINT64_C(50000000)
struct prepared_dispatch {
    void *arena, *backing;
    size_t bytes, words, scratch_offset, scratch_bytes;
    uint32_t packet[PS5VK_COMPUTE_COMMAND_CAPACITY + 8];
};
/* The command stream and its control words, in the special command mapping. */
struct command_region {
    void *command;
    int64_t physical;
    unsigned reserved, mapped;
};
/* Jobs are synchronous, so one mapped region serves them all: a job borrows it
 * and returns it on release. A job prepared while it is still lent maps its own. */
struct queue_state {
    struct command_region region;
    unsigned lent;
};
struct native_job {
    uint64_t serial;
    struct command_region region;
    void *command;
    unsigned borrowed, count, completed, attempted;
    struct prepared_dispatch dispatches[];
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
static int map_region(struct command_region *r)
{
    r->physical = -1;
    if (sceKernelReserveVirtualRange(&r->command, COMMAND_BYTES, 0, ALIGNMENT)) return -1;
    r->reserved = 1;
    if (sceKernelAllocateMainDirectMemory(COMMAND_BYTES, ALIGNMENT, 0x0c, &r->physical)) return -1;
    struct ps5_batch_map_entry entry = {r->command, r->physical, COMMAND_BYTES, 0xf2, 0x0c, 0, 0};
    int processed = 0;
    int map_rc = sceKernelBatchMap(&entry, 1, &processed);
    /* Partial mapping is not safe to unwind as though no mapping existed. */
    if (map_rc || processed != 1) retain("command-map-ambiguous");
    r->mapped = 1;
    return 0;
}
static void unmap_region(struct command_region *r)
{
    if (r->mapped) {
        struct ps5_batch_map_entry e = {r->command, 0, COMMAND_BYTES, 0xf2, 0x0c, 0, 1};
        int processed = 0;
        if (sceKernelBatchMap(&e, 1, &processed) || processed != 1)
            retain("command-unmap");
    }
    if (r->physical >= 0 && sceKernelReleaseDirectMemory(r->physical, COMMAND_BYTES))
        retain("command-physical-release");
    if (r->reserved && sceKernelMunmap(r->command, COMMAND_BYTES))
        retain("command-reservation-release");
    memset(r, 0, sizeof(*r));
    r->physical = -1;
}
static void release(VkDevice device, void *opaque)
{
    struct native_job *job = opaque;
    if (job->attempted && job->completed != job->count) retain("release-inflight");
    if (job->borrowed) ((struct queue_state *)device->queue_state)->lent = 0;
    else unmap_region(&job->region);
    for (unsigned i = 0; i < job->count; ++i)
        if (job->dispatches[i].backing)
            device->memory.release(device->memory.context, job->dispatches[i].backing);
    free(job);
}
static void teardown(VkDevice device)
{
    struct queue_state *state = device->queue_state;
    if (!state) return;
    if (state->lent) retain("teardown-lent-command-region");
    unmap_region(&state->region);
    free(state);
    device->queue_state = NULL;
}
/* The command region for a new job: the device's, or a job-owned one while it is lent. */
static VkResult acquire_region(VkDevice device, struct native_job *job)
{
    struct queue_state *state = device->queue_state;
    if (!state) {
        state = calloc(1, sizeof(*state));
        if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
        state->region.physical = -1;
        if (map_region(&state->region)) {
            unmap_region(&state->region);
            free(state);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        device->queue_state = state;
    }
    if (!state->lent) {
        state->lent = job->borrowed = 1;
        job->command = state->region.command;
        return VK_SUCCESS;
    }
    if (map_region(&job->region)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    job->command = job->region.command;
    return VK_SUCCESS;
}
/* Bytes one dispatch takes from the tables arena: its set tables (at least the
 * 512-byte anchor) followed by its push constants, on a 256-byte boundary. */
static size_t dispatch_table_region(const struct ps5vk_compiled_program *program)
{
    size_t table_bytes = 0;
    for (uint32_t set = 0; set < PS5VK_MAX_SETS; ++set)
        if (program->descriptor_set_mask & (1u << set))
            table_bytes += (ps5vk_compute_table_dwords(program, set) * 4 + 15) & ~(size_t)15;
    if (table_bytes < 512) table_bytes = 512;
    if (program->push_constant_size) table_bytes += PS5VK_MAX_PUSH_CONSTANT_BYTES;
    return (table_bytes + 255) & ~(size_t)255;
}
/* Grow a device arena. A new scratch arena is zeroed once and fenced by guards
 * that every job checks; its contents are never cleared per dispatch because
 * spilled values are always written before they are read. */
static VkResult ensure_arena(VkDevice device, struct ps5vk_device_arena *arena, size_t bytes, int guarded)
{
    if (arena->bytes >= bytes) return VK_SUCCESS;
    if (arena->backing) device->memory.release(device->memory.context, arena->backing);
    arena->address = arena->backing = NULL;
    arena->bytes = 0;
    /* Grow geometrically so a slowly increasing workload does not churn. */
    size_t grown = bytes < SIZE_MAX / 2 ? bytes + bytes / 2 : bytes;
    VkResult result = device->memory.allocate(device->memory.context, grown, &arena->address, &arena->backing);
    if (result != VK_SUCCESS) return result;
    arena->bytes = grown;
    if (guarded) {
        memset(arena->address, 0, grown);
        memset(arena->address, 0xa5, PS5VK_SCRATCH_GUARD_BYTES);
        memset((unsigned char *)arena->address + grown - PS5VK_SCRATCH_GUARD_BYTES, 0xa5,
               PS5VK_SCRATCH_GUARD_BYTES);
        cache(arena->address, grown);
    }
    return VK_SUCCESS;
}
/* GCR_CNTL of a mid-chain acquire: GLI, GLK, GLV and GL1 invalidate, GL2 kept. */
enum { ACQUIRE_WITHOUT_GL2 = 0x0381 };

/* Words of a prepared dispatch up to and including its CS partial flush
 * (EVENT_WRITE CS_PARTIAL_FLUSH, index 4); 0 if the stream has none. */
static size_t intermediate_words(const struct prepared_dispatch *p)
{
    for (size_t i = 8; i < p->words;) {
        const uint32_t header = p->packet[i];
        if ((header >> 30) != 3) return 0;
        const size_t total = ((header >> 16) & 0x3fff) + 2;
        if (((header >> 8) & 0xff) == 0x46 && total == 2 && p->packet[i + 1] == 0x00000407)
            return i + total;
        i += total;
    }
    return 0;
}

static void check_scratch_guards(VkDevice device)
{
    const struct ps5vk_device_arena *arena = &device->compute_scratch;
    if (!arena->bytes) return;
    const uint8_t *low = arena->address;
    const uint8_t *high = (const uint8_t *)arena->address + arena->bytes - PS5VK_SCRATCH_GUARD_BYTES;
    cache(low, PS5VK_SCRATCH_GUARD_BYTES);
    cache(high, PS5VK_SCRATCH_GUARD_BYTES);
    for (size_t j = 0; j < PS5VK_SCRATCH_GUARD_BYTES; ++j)
        if (low[j] != 0xa5 || high[j] != 0xa5) retain("scratch-guard-corruption");
}
static VkResult prepare(VkDevice device, const struct ps5vk_submission *submission, void **out)
{
    *out = NULL;
    /* Reserve low bits for exact per-dispatch tokens. No token truncation. */
    if (!submission->serial || submission->serial > UINT32_MAX) return VK_ERROR_UNKNOWN;
    /* The job holds one prepared packet per dispatch it carries, not the maximum. */
    size_t dispatches = 0;
    for (unsigned b = 0; b < submission->count; ++b) {
        VkCommandBuffer cb = submission->buffers[b];
        uint32_t first = ps5vk_submission_first_operation(submission, b);
        uint32_t count = ps5vk_submission_operation_count(submission, b);
        if (first > cb->operation_count || count > cb->operation_count - first) return VK_ERROR_UNKNOWN;
        for (uint32_t i = first; i < first + count; ++i)
            dispatches += cb->operations[i].type == PS5VK_DISPATCH ||
                          ps5vk_indirect_compute_operation(cb->operations[i].type);
    }
    if (dispatches > MAX_DISPATCHES) return VK_ERROR_UNKNOWN;
    struct native_job *job = calloc(1, sizeof(*job) + dispatches * sizeof(struct prepared_dispatch));
    if (!job) return VK_ERROR_OUT_OF_HOST_MEMORY;
    job->region.physical = -1; job->serial = submission->serial;
    VkResult result = acquire_region(device, job);
    if (result != VK_SUCCESS) goto fail;
    /* Pass 1: size the job. Shader code becomes resident per pipeline; tables,
     * push constants and scratch come from device arenas reused by every job.
     * Jobs are synchronous, so no earlier job still reads those arenas. */
    size_t tables_needed = 0, scratch_needed = 0;
    for (unsigned b = 0; b < submission->count; ++b) {
        VkCommandBuffer cb = submission->buffers[b];
        uint32_t first = ps5vk_submission_first_operation(submission, b);
        uint32_t count = ps5vk_submission_operation_count(submission, b);
        if (first > cb->operation_count || count > cb->operation_count - first) {
            result = VK_ERROR_UNKNOWN; goto fail;
        }
        for (uint32_t i = first; i < first + count; ++i) {
            const struct ps5vk_operation *recorded = &cb->operations[i];
            if (recorded->type == PS5VK_BARRIER) continue;
            if (recorded->type != PS5VK_DISPATCH && !ps5vk_indirect_compute_operation(recorded->type)) {
                result = VK_ERROR_FEATURE_NOT_PRESENT; goto fail;
            }
            VkPipeline pipeline = recorded->pipeline;
            const struct ps5vk_compiled_program *program = &pipeline->program;
            if (!program->code_words || program->code_words > 1024 * 1024) {
                result = VK_ERROR_UNKNOWN; goto fail;
            }
            if (!pipeline->native_code) {
                size_t code_bytes = ps5vk_placed_code_bytes(program->code_words);
                result = device->memory.allocate(device->memory.context, code_bytes,
                                                 &pipeline->native_code, &pipeline->native_code_backing);
                if (result != VK_SUCCESS) goto fail;
                ps5vk_place_code(pipeline->native_code, program->code, program->code_words);
                cache(pipeline->native_code, code_bytes);
            }
            tables_needed += dispatch_table_region(program);
            if (program->scratch_bytes_per_wave) {
                if ((program->scratch_bytes_per_wave & 1023u) ||
                    program->scratch_bytes_per_wave > 8191u * 1024u) {
                    result = VK_ERROR_UNKNOWN; goto fail;
                }
                size_t bytes = (size_t)(program->scratch_bytes_per_wave | 1024u) *
                    PS5VK_COMPUTE_SCRATCH_WAVES;
                if (bytes > scratch_needed) scratch_needed = bytes;
            }
        }
    }
    if ((result = ensure_arena(device, &device->compute_tables, tables_needed, 0)) != VK_SUCCESS ||
        (scratch_needed &&
         (result = ensure_arena(device, &device->compute_scratch,
                                scratch_needed + 2 * PS5VK_SCRATCH_GUARD_BYTES, 1)) != VK_SUCCESS))
        goto fail;
    /* Pass 2: encode every dispatch against the arenas. */
    size_t tables_used = 0;
    for (unsigned b = 0; b < submission->count; ++b) {
        VkCommandBuffer cb = submission->buffers[b];
        uint32_t first = ps5vk_submission_first_operation(submission, b);
        uint32_t count = ps5vk_submission_operation_count(submission, b);
        for (uint32_t i = first; i < first + count; ++i) {
            const struct ps5vk_operation *recorded = &cb->operations[i];
            struct ps5vk_operation resolved;
            const struct ps5vk_operation *op = recorded;
            if (ps5vk_indirect_compute_operation(recorded->type)) {
                result = ps5vk_indirect_resolve(device, recorded, &resolved);
                if (result != VK_SUCCESS) goto fail;
                op = &resolved;
            }
            /* Dispatches retire in order (see launch); that is stronger than the
             * supported global host/compute barriers, not a skipped dependency.
             * Image transitions and queue-family transfers are not handled here. */
            if (op->type == PS5VK_BARRIER) continue;
            if (op->type != PS5VK_DISPATCH) {
                result = VK_ERROR_FEATURE_NOT_PRESENT; goto fail;
            }
            if (job->count == dispatches) { result = VK_ERROR_UNKNOWN; goto fail; }
            struct prepared_dispatch *p = &job->dispatches[job->count++];
            const struct ps5vk_compiled_program *program = &op->pipeline->program;
            /* Each set's table is as long as the records the program reads
             * (up to PS5VK_MAX_DESCRIPTORS of them); the bootstrap template
             * still needs one mapped 512-byte anchor when no set is read. */
            size_t set_dwords[PS5VK_MAX_SETS] = {0}, set_offsets[PS5VK_MAX_SETS] = {0};
            size_t table_bytes = 0;
            for (uint32_t set = 0; set < PS5VK_MAX_SETS; ++set) {
                if (!(program->descriptor_set_mask & (1u << set))) continue;
                set_dwords[set] = ps5vk_compute_table_dwords(program, set);
                if (!set_dwords[set]) { result = VK_ERROR_UNKNOWN; goto fail; }
                set_offsets[set] = table_bytes / 4;
                table_bytes += (set_dwords[set] * 4 + 15) & ~(size_t)15;
            }
            if (table_bytes < 512) table_bytes = 512;
            const size_t region = dispatch_table_region(program);
            if (region > device->compute_tables.bytes - tables_used) { result = VK_ERROR_UNKNOWN; goto fail; }
            unsigned char *base = (unsigned char *)device->compute_tables.address + tables_used;
            tables_used += region;
            uint32_t *tables = (void *)base;
            memset(tables, 0, table_bytes);
            struct ps5vk_dispatch_encoding encoding = {.program = program,
                .addresses = {.code=(uintptr_t)op->pipeline->native_code,
                    /* The bootstrap template requires a mapped table anchor
                     * even when the compiled shader uses no descriptor set.
                     * Its user-SGPR packet is replaced below. */
                    .descriptor_table=(uintptr_t)tables,
                    .completion=(uintptr_t)job->command + LABEL_OFFSET,
                    .readback=(uintptr_t)job->command + READBACK_OFFSET},
                .completion_value = (job->serial << 32) | job->count};
            if (program->scratch_bytes_per_wave) {
                p->scratch_bytes = (size_t)(program->scratch_bytes_per_wave | 1024u) *
                    PS5VK_COMPUTE_SCRATCH_WAVES;
                encoding.scratch = (uintptr_t)device->compute_scratch.address + PS5VK_SCRATCH_GUARD_BYTES;
                encoding.scratch_bytes = p->scratch_bytes;
            }
            if(program->push_constant_size) {
                if(op->push_constant_size!=program->push_constant_size) {
                    result=VK_ERROR_UNKNOWN;goto fail;
                }
                void *push=base+table_bytes;
                memcpy(push,op->push_constants,program->push_constant_size);
                encoding.push_constants=(uintptr_t)push;
            }
            for(uint32_t set=0;set<PS5VK_MAX_SETS;++set)
                if(program->descriptor_set_mask&(1u<<set)) {
                    uint32_t *table=tables+set_offsets[set];
                    result=ps5vk_descriptor_encode(device,program,set,op->sets[set],
                        op->dynamic_offsets[set],table,set_dwords[set]);
                    if(result!=VK_SUCCESS)goto fail;
                    encoding.descriptor_tables[set]=(uintptr_t)table;
                }
            memcpy(encoding.groups, op->groups, sizeof(encoding.groups));
            memcpy(encoding.group_base, op->group_base, sizeof(encoding.group_base));
            /* Mesa ac_emit_cp_acquire_mem GFX10 compute form + pkt3 GCR_CNTL:
             * invalidate instruction/scalar/vector/L1/L2 caches globally.
             * No WB here: previous dispatch RELEASE_MEM already wrote back,
             * and host noncoherent writes require explicit vkFlush*. */
            const uint32_t acquire[8] = {0xc0065800, 0, 0xffffffff, 0xff, 0, 0, 10, 0x4381};
            memcpy(p->packet, acquire, sizeof(acquire));
            p->words = ps5vk_dispatch_encode(p->packet + 8, PS5VK_COMPUTE_COMMAND_CAPACITY, &encoding);
            if (!p->words) { result = VK_ERROR_UNKNOWN; goto fail; }
            p->words += 8;
        }
    }
    if (tables_used) cache(device->compute_tables.address, tables_used);
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
    volatile uint64_t *label = (void *)((unsigned char *)job->command + LABEL_OFFSET);
    uint32_t *readback = (void *)((unsigned char *)job->command + READBACK_OFFSET);
    /* Chain as many dispatches as fit into one DCB, ordered the way RADV orders
     * a compute-to-compute dependency: each dispatch ends with its CS partial
     * flush (the CP waits for its waves) and the next one's acquire invalidates
     * the per-CU instruction, scalar, L0 and L1 caches. GL2 is coherent for all
     * CUs, so only the chain's first dispatch also invalidates it (host writes)
     * and only the last one keeps the template tail: readbacks and the GL2
     * write-back RELEASE_MEM that publishes the completion token. */
    for (unsigned first = 0; first < job->count;) {
        uint32_t *stream = job->command;
        size_t words = 0, chain_words = 0;
        unsigned last = first;
        while (last < job->count && (chain_words + job->dispatches[last].words) * 4 <= STREAM_BYTES)
            chain_words += job->dispatches[last++].words;
        if (last == first) return VK_ERROR_UNKNOWN;
        for (unsigned i = first; i < last; ++i) {
            const struct prepared_dispatch *p = &job->dispatches[i];
            const size_t count = i + 1 < last ? intermediate_words(p) : p->words;
            if (!count) return VK_ERROR_UNKNOWN;
            memcpy(stream + words, p->packet, count * 4);
            if (i > first) stream[words + 7] = ACQUIRE_WITHOUT_GL2;
            words += count;
        }
        /* The CP reads the stream's words and the packets write only the control
         * words, so only those are published; the reused region's label may
         * still hold an earlier job's token. */
        *label = 0;
        readback[3] = 0xdeadbeef;
        cache(stream, words * 4);
        cache(readback, COMMAND_BYTES - READBACK_OFFSET);
        struct ps5_agc_submit submit = {job->command, (uint32_t)words, 0, {0, 0, 0}};
        job->attempted = 1;
        struct ps5vk_submit_result result=ps5vk_submit_suspend(&submit);
        int rc = result.submit_rc;
        ps5log_printf(PS5LOG_MARK, "PS5VK_QUEUE_SUBMIT serial=%llu first=%u count=%u words=%zu rc=%d",
                      (unsigned long long)job->serial, first, last - first, words, rc);
        if (rc) return VK_ERROR_DEVICE_LOST;
        if (result.suspend_rc) return VK_ERROR_DEVICE_LOST;
        const uint64_t token = (job->serial << 32) | last;
        const uint64_t budget = UINT64_C(3000000000) + UINT64_C(100000000) * (last - first);
        uint64_t start = clock_ns(NULL);
        for (;;) {
            cache((const void *)label, 8);
            uint64_t observed = __atomic_load_n(label, __ATOMIC_ACQUIRE);
            if (observed == token) break;
            /* Earlier dispatches of this chain legitimately publish lower tokens. */
            const uint64_t waited = clock_ns(NULL) - start;
            if ((observed && (observed >> 32) != job->serial) ||
                (uint32_t)observed > last || !start || waited > budget)
                return VK_ERROR_DEVICE_LOST;
            /* A sleep lasts at least one kernel timer tick, which on some system
             * software is 15.6 ms: longer than a whole frame of work. Jobs are
             * synchronous, so spin on the label while a job is young. */
            if (waited < SPIN_NS) __builtin_ia32_pause();
            else usleep(50);
        }
        cache(readback, 16);
        if (readback[3] != 0) return VK_ERROR_DEVICE_LOST;
        int used_scratch = 0;
        for (unsigned i = first; i < last; ++i) {
            used_scratch |= job->dispatches[i].scratch_bytes != 0;
            ++job->completed;
        }
        if (used_scratch) check_scratch_guards(device);
        ps5log_printf(PS5LOG_MARK, "PS5VK_QUEUE_COMPLETED serial=%llu first=%u count=%u token=%llx",
                      (unsigned long long)job->serial, first, last - first, (unsigned long long)token);
        first = last;
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
    device->queue_teardown = teardown;
    device->progress = (struct ps5vk_progress){NULL, ps5vk_queue_poll, clock_ns, pause_ns};
}
