#ifndef PS5VK_COMPILE_STACK_H
#define PS5VK_COMPILE_STACK_H
#include <pthread.h>
#include <stddef.h>

/* The runtime shader compilers (PSBC: SPIR-V to NIR to ACO) need far more
 * stack than an application thread is guaranteed to have. DXVK creates
 * pipelines from its own std::thread workers, which run on the platform's
 * default pthread stack, and a compile there overflowed it and killed the
 * process without a report. Every runtime compile therefore runs on a
 * dedicated thread whose stack this driver sizes itself, and the caller waits
 * for it with pthread_join: one bounded thread per compile, nothing
 * persistent to shut down, and the result is exactly the one the compile
 * function returns.
 *
 * PS5VK_COMPILE_STACK_BYTES is 8 MiB. Measured on the host by building the
 * whole runtime graphics compiler suite (vertex, fragment, geometry and
 * tessellation pairs) with smaller values: 160 KiB overflows, 192 KiB passes.
 * 8 MiB is some forty times that, which covers a different compiler's frame
 * layout on the console and larger application shaders; a console payload
 * that gave DXVK's pipeline workers 8 MiB stacks compiled its first pipeline
 * where the default stack died. */
#ifndef PS5VK_COMPILE_STACK_BYTES
#define PS5VK_COMPILE_STACK_BYTES ((size_t)8u << 20)
#endif

struct ps5vk_compile_stack_call {
    void (*run)(void *);
    void *arg;
};

static inline void *ps5vk_compile_stack_trampoline(void *opaque)
{
    struct ps5vk_compile_stack_call *call = opaque;
    call->run(call->arg);
    return NULL;
}

/* Run fn(arg) to completion on a thread with a PS5VK_COMPILE_STACK_BYTES
 * stack. Returns 0 when it ran there; nonzero when the thread could not be
 * created, in which case fn has not run and the caller reports the failure
 * (running it here would reintroduce the overflow). */
static inline int ps5vk_compile_on_sized_stack(void (*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) return -1;
    int rc = pthread_attr_setstacksize(&attr, PS5VK_COMPILE_STACK_BYTES);
    struct ps5vk_compile_stack_call call = {fn, arg};
    pthread_t thread;
    if (!rc) rc = pthread_create(&thread, &attr, ps5vk_compile_stack_trampoline, &call);
    pthread_attr_destroy(&attr);
    if (rc) return rc;
    return pthread_join(thread, NULL);
}

#endif
