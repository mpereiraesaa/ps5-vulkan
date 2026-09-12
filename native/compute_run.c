#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ps5_platform.h"
#include "ps5_agc_driver.h"
#include "submit_suspend_ps5.h"
#include "ps5log.h"
#include "compute_check.h"
#include "compute_commands.h"
#include "compute_shader.h"

#ifndef PS5VK_SUBMIT
#define PS5VK_SUBMIT 0
#endif
#ifndef PS5VK_DMA_ONLY
#define PS5VK_DMA_ONLY 0
#endif
enum { ARENA_BYTES = 65536, COMMAND_BYTES = 131072, COMMAND_ALIGNMENT = 65536 };
static void flush(const void *address, size_t bytes)
{
    uintptr_t begin = (uintptr_t)address & ~(uintptr_t)63;
    uintptr_t end = (uintptr_t)address + bytes;
    for (; begin < end; begin += 64)
        __asm__ volatile("clflush (%0)" : : "r"(begin) : "memory");
    __asm__ volatile("mfence" : : : "memory");
}
static uint64_t clock_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}
static void retain(const char *reason)
{
    ps5log_printf(PS5LOG_ERR, "PS5VK_RETAIN reason=%s ownership=unknown", reason);
    ps5log_close("parked-retain");
    for (;;) sleep(1); /* Owner/controller may close exact app; never unmap. */
}
static void descriptor(uint32_t *d, const void *buffer)
{
    uint64_t address = (uintptr_t)buffer;
    d[0] = (uint32_t)address; d[1] = (uint32_t)(address >> 32);
    d[2] = PS5VK_ELEMENTS * sizeof(uint32_t);
    /* Pinned gfx10-rsrc fields: XYZW, R32_FLOAT, resource level, raw OOB. */
    d[3] = UINT32_C(0x31016fac);
}
int ps5vk_run_compute(void)
{
    void *arena = 0, *command = 0;
    int64_t physical = -1, command_physical = -1;
    int reserved = 0, mapped = 0, command_mapped = 0, rc = -1, processed;
    rc = sceKernelAllocateMainDirectMemory(ARENA_BYTES, 65536, 0x0c, &physical);
    if (rc) goto cleanup;
    rc = sceKernelMapDirectMemory(&arena, ARENA_BYTES, 0x33, 0, physical, 65536);
    if (rc) goto cleanup;
    mapped = 1;
    if (!arena || (uintptr_t)arena > (UINT64_C(1) << 48) - ARENA_BYTES ||
        ((uintptr_t)arena >> 32) != (((uintptr_t)arena + ARENA_BYTES - 1) >> 32)) {
        rc = -2; goto cleanup;
    }
    rc = sceKernelReserveVirtualRange(&command, COMMAND_BYTES, 0, COMMAND_ALIGNMENT);
    if (rc) goto cleanup;
    reserved = 1;
    rc = sceKernelAllocateMainDirectMemory(COMMAND_BYTES, COMMAND_ALIGNMENT, 0x0c,
                                          &command_physical);
    if (rc) goto cleanup;
    struct ps5_batch_map_entry entry = {command, command_physical, COMMAND_BYTES,
                                      0xf2, 0x0c, 0, 0};
    processed = 0;
    rc = sceKernelBatchMap(&entry, 1, &processed);
    if (rc || processed != 1) { rc = rc ? rc : -3; goto cleanup; }
    command_mapped = 1;
    memset(arena, 0xa5, ARENA_BYTES);
    memset(command, 0, COMMAND_BYTES);
    unsigned char *base = arena;
    void *code = base + 0x1000;
    uint32_t *table = (uint32_t *)(base + 0x2000);
    struct ps5vk_compute_buffer *input = (void *)(base + 0x4000);
    struct ps5vk_compute_buffer *output = (void *)(base + 0x6000);
    volatile uint64_t *fence = (void *)((unsigned char *)command + 0x1100);
    uint32_t *readback = (void *)((unsigned char *)command + 0x0ff4);
    readback[0] = readback[1] = readback[2] = readback[3] = UINT32_C(0xdeadbeef);
    _Static_assert(sizeof(ps5vk_shader_code) <= 0x1000, "code region");
    _Static_assert(sizeof(*input) < 0x2000, "buffer regions");
    memcpy(code, ps5vk_shader_code, sizeof(ps5vk_shader_code));
    ps5vk_compute_prepare(input, output);
    /* Compiled LLPC layout: output descriptor first, input second. */
    descriptor(table, output->values); descriptor(table + 4, input->values);
    *fence = UINT64_C(1);
    struct ps5vk_compute_addresses addresses = {
        (uintptr_t)code, (uintptr_t)table, (uintptr_t)fence, (uintptr_t)readback
    };
    size_t count = ps5vk_compute_commands(command, PS5VK_COMPUTE_COMMAND_CAPACITY,
                                         &addresses);
    if (!count) { rc = -4; goto cleanup; }
    if (PS5VK_DMA_ONLY) {
        /* Isolate the final DMA_DATA + RELEASE_MEM pair. No shader dispatch. */
        memmove(command, (uint32_t *)command + count - 15, 15 * sizeof(uint32_t));
        count = 15;
    }
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_PREPARED shader=%s dwords=%zu submit_enabled=%d dma_only=%d",
        PS5VK_SHADER_ELF_SHA256, count, PS5VK_SUBMIT, PS5VK_DMA_ONLY);
    ps5log_printf(PS5LOG_INFO, "PS5VK_ADDRESSES code=%llx table=%llx input=%llx output=%llx fence=%llx",
        (unsigned long long)(uintptr_t)code, (unsigned long long)(uintptr_t)table,
        (unsigned long long)(uintptr_t)input->values,
        (unsigned long long)(uintptr_t)output->values,
        (unsigned long long)(uintptr_t)fence);
    ps5log_printf(PS5LOG_INFO, "PS5VK_MAPPING arena=%llx physical=%llx command=%llx command_physical=%llx control=%llx",
        (unsigned long long)(uintptr_t)arena, (unsigned long long)physical,
        (unsigned long long)(uintptr_t)command, (unsigned long long)command_physical,
        (unsigned long long)(uintptr_t)(readback + 3));
    flush(arena, ARENA_BYTES); flush(command, COMMAND_BYTES);
    if (PS5VK_DMA_ONLY) {
        const volatile uint32_t *actual = command;
        for (size_t i = 0; i < count; ++i)
            ps5log_printf(PS5LOG_INFO, "PS5VK_WORD index=%zu value=%08x", i, actual[i]);
    }
    ps5log_printf(PS5LOG_INFO, "PS5VK_BEFORE fence=%llx immediate=%08x",
        (unsigned long long)*fence, readback[3]);
    if (PS5VK_SUBMIT) {
        struct ps5_agc_submit submit = {command, (uint32_t)count, 0, {0, 0, 0}};
        struct ps5vk_submit_result submission=ps5vk_submit_suspend(&submit);
        rc = submission.submit_rc;
        ps5log_printf(PS5LOG_MARK, "PS5VK_SUBMIT rc=%d", rc);
        if (rc) retain("submit-error");
        ps5log_printf(PS5LOG_MARK,"PS5VK_SUSPEND_POINT rc=%d",submission.suspend_rc);
        if(submission.suspend_rc)retain("suspend-point-error");
        uint64_t deadline = clock_ns() + UINT64_C(3000000000);
        while (__atomic_load_n(fence, __ATOMIC_ACQUIRE) != PS5VK_COMPLETION_VALUE) {
            flush((const void *)fence, sizeof(*fence));
            if (clock_ns() > deadline) retain("completion-timeout");
            usleep(1000);
        }
        uint32_t before_flush = __atomic_load_n(readback + 3, __ATOMIC_ACQUIRE);
        ps5log_printf(PS5LOG_MARK, "PS5VK_COMPLETION observed=%llx",
            (unsigned long long)__atomic_load_n(fence, __ATOMIC_ACQUIRE));
        if (PS5VK_DMA_ONLY) {
            const volatile uint32_t *actual = command;
            for (size_t i = 0; i < count; ++i)
                ps5log_printf(PS5LOG_INFO, "PS5VK_POST_WORD index=%zu value=%08x", i, actual[i]);
        }
        ps5log_printf(PS5LOG_INFO, "PS5VK_VISIBILITY control_before_flush=%08x",
            before_flush);
        flush(arena, ARENA_BYTES);
        flush(readback, 16);
        ps5log_printf(PS5LOG_INFO,
            "PS5VK_REGISTERS pgm_lo=%08x threads_x=%08x dispatch=%08x immediate=%08x",
            readback[0], readback[1], readback[2], readback[3]);
        struct ps5vk_compute_result result = ps5vk_compute_check(input, output);
        size_t untouched = 0;
        for (size_t i = 0; i < PS5VK_ELEMENTS; ++i)
            untouched += output->values[i] == ~ps5vk_compute_expected(i);
        ps5log_printf(PS5LOG_INFO, "PS5VK_VALUES untouched=%zu out0=%08x out1=%08x out1023=%08x expected0=%08x",
            untouched, output->values[0], output->values[1], output->values[1023],
            ps5vk_compute_expected(0));
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_RESULT completion=token outputs=%zu inputs=%zu guards=%zu first=%zu",
            result.output_errors, result.input_errors, result.guard_errors,
            result.first_output_error);
        rc = (result.output_errors || result.input_errors || result.guard_errors) ? -5 : 0;
#if PS5VK_INSPECT
        ps5log_line(PS5LOG_MARK, "PS5VK_INSPECT phase=after-result seconds=60 allocations=retained");
        sleep(60);
#endif
    } else rc = 0;
cleanup:
    ps5log_printf(PS5LOG_INFO, "PS5VK_CLEANUP_BEGIN result=%d", rc);
    if (command_mapped) {
        struct ps5_batch_map_entry unmap = {command, 0, COMMAND_BYTES, 0xf2, 0x0c, 0, 1};
        processed = 0;
        int r = sceKernelBatchMap(&unmap, 1, &processed);
        if (r || processed != 1) retain("command-unmap-failed");
    }
    if (command_physical >= 0 && sceKernelReleaseDirectMemory(command_physical, COMMAND_BYTES))
        retain("command-release-failed");
    if (reserved && sceKernelMunmap(command, COMMAND_BYTES)) retain("reservation-release-failed");
    if (mapped && sceKernelMunmap(arena, ARENA_BYTES)) retain("arena-unmap-failed");
    if (physical >= 0 && sceKernelReleaseDirectMemory(physical, ARENA_BYTES)) retain("arena-release-failed");
    ps5log_printf(PS5LOG_MARK, "PS5VK_CLEANUP_END result=%d allocations_live=0", rc);
    return rc;
}
