/* Host contract of the VK_KHR_timeline_semaphore payload model.
 *
 * The backend here is a control mock: it records prepare/launch order and
 * reports completion only when the fixture says so. It proves the frontend's
 * ordering, visibility and lifetime rules, not GPU execution. */
#define _POSIX_C_SOURCE 200809L
#include "vk_queue.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { MAX_LAUNCHES = 64 };
struct fixture {
    unsigned prepares, launches, releases, pauses;
    VkResult prepare_result, launch_result;
    /* complete: the launched job reports completion; auto_complete: every
     * launch completes immediately (used when another thread drives). */
    int complete, auto_complete, real_time;
    uint64_t now;
    uint64_t launched[MAX_LAUNCHES];
};
struct mock_job { struct fixture *fixture; uint64_t serial; };

static VkResult prepare(VkDevice d, const struct ps5vk_submission *s, void **job)
{
    struct fixture *f = d->progress.context;
    ++f->prepares;
    if (f->prepare_result) return f->prepare_result;
    struct mock_job *prepared = malloc(sizeof(*prepared));
    if (!prepared) return VK_ERROR_OUT_OF_HOST_MEMORY;
    prepared->fixture = f; prepared->serial = s->serial;
    *job = prepared;
    return VK_SUCCESS;
}
static VkResult launch(VkDevice d, void *job)
{
    (void)d;
    struct mock_job *prepared = job;
    struct fixture *f = prepared->fixture;
    if (f->launches < MAX_LAUNCHES) f->launched[f->launches] = prepared->serial;
    ++f->launches;
    if (f->auto_complete) f->complete = 1;
    return f->launch_result;
}
static VkResult poll_backend(VkDevice d, void *job, uint64_t *completed)
{
    (void)d;
    struct mock_job *prepared = job;
    *completed = prepared->fixture->complete ? prepared->serial : 0;
    return VK_SUCCESS;
}
static void release(VkDevice d, void *job)
{
    (void)d;
    struct mock_job *prepared = job;
    ++prepared->fixture->releases;
    free(prepared);
}
static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
static uint64_t clock_ns(void *ctx)
{
    struct fixture *f = ctx;
    return f->real_time ? monotonic_ns() : f->now;
}
static void pause_wait(void *ctx, uint64_t remaining)
{
    struct fixture *f = ctx;
    assert(remaining);
    if (f->real_time) {
        struct timespec ts = {0, 100000};
        nanosleep(&ts, NULL);
        return;
    }
    ++f->pauses;
    f->now += 1000;
}

static void init_device(struct VkDevice_T *d, struct fixture *f)
{
    *d = (struct VkDevice_T){
        .progress = {f, ps5vk_queue_poll, clock_ns, pause_wait},
        .submit_backend = {prepare, launch, poll_backend, release},
        .enabled_features_t09 = PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE,
        .timeline_extension_enabled = VK_TRUE,
    };
    d->queue.device = d;
    d->queue.next_serial = 1;
}
static VkSemaphore make_semaphore(VkDevice d, VkSemaphoreType type, uint64_t initial)
{
    VkSemaphoreTypeCreateInfo type_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = type, .initialValue = initial};
    VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                  .pNext = &type_info};
    VkSemaphore semaphore = VK_NULL_HANDLE;
    assert(vkCreateSemaphore(d, &info, NULL, &semaphore) == VK_SUCCESS && semaphore);
    return semaphore;
}
static uint64_t counter(VkDevice d, VkSemaphore semaphore)
{
    uint64_t value = 0;
    assert(vkGetSemaphoreCounterValueKHR(d, semaphore, &value) == VK_SUCCESS);
    return value;
}
static VkResult wait_value(VkDevice d, VkSemaphore semaphore, uint64_t value,
                           uint64_t timeout)
{
    VkSemaphoreWaitInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &value};
    return vkWaitSemaphoresKHR(d, &info, timeout);
}
static VkResult host_signal(VkDevice d, VkSemaphore semaphore, uint64_t value)
{
    VkSemaphoreSignalInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                  .semaphore = semaphore, .value = value};
    return vkSignalSemaphoreKHR(d, &info);
}

/* One VkSubmitInfo with at most one wait and one signal, each binary or
 * timeline, and an optional command buffer. */
struct batch {
    VkSemaphore wait, signal;
    uint64_t wait_value, signal_value;
    VkCommandBuffer command;
    VkPipelineStageFlags stage;
    VkTimelineSemaphoreSubmitInfo values;
    VkSubmitInfo info;
};
static const VkSubmitInfo *make_batch(struct batch *b, VkSemaphore wait,
    uint64_t wait_value, VkSemaphore signal, uint64_t signal_value,
    VkCommandBuffer command)
{
    *b = (struct batch){.wait = wait, .signal = signal, .wait_value = wait_value,
                        .signal_value = signal_value, .command = command,
                        .stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
    b->values = (VkTimelineSemaphoreSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = wait ? 1u : 0u,
        .pWaitSemaphoreValues = wait ? &b->wait_value : NULL,
        .signalSemaphoreValueCount = signal ? 1u : 0u,
        .pSignalSemaphoreValues = signal ? &b->signal_value : NULL};
    b->info = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &b->values,
        .waitSemaphoreCount = wait ? 1u : 0u, .pWaitSemaphores = &b->wait,
        .pWaitDstStageMask = &b->stage,
        .commandBufferCount = command ? 1u : 0u, .pCommandBuffers = &b->command,
        .signalSemaphoreCount = signal ? 1u : 0u, .pSignalSemaphores = &b->signal};
    return &b->info;
}
static VkResult submit(VkDevice d, VkSemaphore wait, uint64_t wait_value,
    VkSemaphore signal, uint64_t signal_value, VkCommandBuffer command, VkFence fence)
{
    struct batch b;
    return vkQueueSubmit(&d->queue, 1,
        make_batch(&b, wait, wait_value, signal, signal_value, command), fence);
}
static VkCommandBuffer command_buffer(VkDevice d, VkCommandPool *pool,
                                      VkCommandBufferUsageFlags usage)
{
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    assert(vkCreateCommandPool(d, &pi, NULL, pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = *pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    VkCommandBuffer c;
    assert(vkAllocateCommandBuffers(d, &ai, &c) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = usage};
    assert(vkBeginCommandBuffer(c, &bi) == VK_SUCCESS);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS);
    return c;
}

static void creation_contract(void)
{
    struct fixture f = {0};
    struct VkDevice_T d;
    init_device(&d, &f);
    VkSemaphoreTypeCreateInfo type_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 42};
    VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                  .pNext = &type_info};
    VkSemaphore s = VK_NULL_HANDLE;

    /* The feature, not only the extension, gates a timeline semaphore. */
    d.enabled_features_t09 = 0;
    assert(vkCreateSemaphore(&d, &info, NULL, &s) == VK_ERROR_UNKNOWN && !s);
    d.enabled_features_t09 = PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE;
    /* The type structure is unknown without the extension. */
    d.timeline_extension_enabled = VK_FALSE;
    assert(vkCreateSemaphore(&d, &info, NULL, &s) == VK_ERROR_UNKNOWN && !s);
    d.timeline_extension_enabled = VK_TRUE;
    /* A binary semaphore has no initial value. */
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_BINARY;
    assert(vkCreateSemaphore(&d, &info, NULL, &s) == VK_ERROR_UNKNOWN && !s);
    type_info.initialValue = 0;
    assert(vkCreateSemaphore(&d, &info, NULL, &s) == VK_SUCCESS && s);
    assert(s->type == VK_SEMAPHORE_TYPE_BINARY && !s->value);
    uint64_t value = 7;
    assert(vkGetSemaphoreCounterValueKHR(&d, s, &value) == VK_ERROR_UNKNOWN && value == 7);
    assert(host_signal(&d, s, 1) == VK_ERROR_UNKNOWN);
    assert(wait_value(&d, s, 0, 0) == VK_ERROR_UNKNOWN);
    vkDestroySemaphore(&d, s, NULL);
    /* Unknown types and duplicated structures are refused. */
    type_info.semaphoreType = (VkSemaphoreType)2;
    assert(vkCreateSemaphore(&d, &info, NULL, &s) == VK_ERROR_UNKNOWN && !s);
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreTypeCreateInfo second = type_info;
    type_info.pNext = &second;
    assert(vkCreateSemaphore(&d, &info, NULL, &s) == VK_ERROR_UNKNOWN && !s);
    type_info.pNext = NULL;
    /* No type structure keeps the Vulkan 1.0 binary semaphore. */
    info.pNext = NULL;
    assert(vkCreateSemaphore(&d, &info, NULL, &s) == VK_SUCCESS);
    assert(s->type == VK_SEMAPHORE_TYPE_BINARY);
    vkDestroySemaphore(&d, s, NULL);
    assert(!d.semaphores && !d.lifetime_errors);

    /* The initial value is the payload, including the extremes. */
    VkSemaphore zero = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, 0);
    VkSemaphore high = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, UINT64_MAX);
    assert(counter(&d, zero) == 0 && counter(&d, high) == UINT64_MAX);
    assert(wait_value(&d, zero, 0, 0) == VK_SUCCESS);
    assert(wait_value(&d, zero, 1, 0) == VK_TIMEOUT);
    assert(wait_value(&d, high, UINT64_MAX, 0) == VK_SUCCESS);
    /* Nothing exceeds UINT64_MAX, so no host signal is valid any more. */
    assert(host_signal(&d, high, UINT64_MAX) == VK_ERROR_UNKNOWN);
    vkDestroySemaphore(&d, zero, NULL);
    vkDestroySemaphore(&d, high, NULL);
    assert(!d.semaphores);
}

static void host_signal_and_wait(void)
{
    struct fixture f = {0};
    struct VkDevice_T d;
    init_device(&d, &f);
    VkSemaphore a = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, 3);
    VkSemaphore b = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, 0);
    VkSemaphore both[] = {a, b};
    uint64_t values[] = {5, 2};
    VkSemaphoreWaitInfo all = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 2, .pSemaphores = both, .pValues = values};
    VkSemaphoreWaitInfo any = all;
    any.flags = VK_SEMAPHORE_WAIT_ANY_BIT;

    assert(vkWaitSemaphoresKHR(&d, &all, 0) == VK_TIMEOUT);
    assert(vkWaitSemaphoresKHR(&d, &any, 0) == VK_TIMEOUT);
    /* A finite timeout pauses, measures and then reports VK_TIMEOUT. */
    const unsigned pauses = f.pauses;
    assert(vkWaitSemaphoresKHR(&d, &any, 5000) == VK_TIMEOUT);
    assert(f.pauses == pauses + 5 && f.now == 5000);

    /* Monotonic host signals: equal or smaller values are invalid usage. */
    assert(host_signal(&d, a, 3) == VK_ERROR_UNKNOWN);
    assert(host_signal(&d, a, 2) == VK_ERROR_UNKNOWN && counter(&d, a) == 3);
    assert(host_signal(&d, b, 2) == VK_SUCCESS && counter(&d, b) == 2);
    assert(vkWaitSemaphoresKHR(&d, &any, 0) == VK_SUCCESS);
    assert(vkWaitSemaphoresKHR(&d, &all, 0) == VK_TIMEOUT);
    assert(host_signal(&d, a, 9) == VK_SUCCESS);
    assert(vkWaitSemaphoresKHR(&d, &all, UINT64_MAX) == VK_SUCCESS);
    /* A value already passed is satisfied without waiting for equality. */
    values[0] = 1; values[1] = 0;
    assert(vkWaitSemaphoresKHR(&d, &all, 0) == VK_SUCCESS);

    /* Malformed wait descriptions. */
    all.semaphoreCount = 0;
    assert(vkWaitSemaphoresKHR(&d, &all, 0) == VK_ERROR_UNKNOWN);
    all.semaphoreCount = 2; all.flags = 2;
    assert(vkWaitSemaphoresKHR(&d, &all, 0) == VK_ERROR_UNKNOWN);
    all.flags = 0; all.pValues = NULL;
    assert(vkWaitSemaphoresKHR(&d, &all, 0) == VK_ERROR_UNKNOWN);
    all.pValues = values;
    struct VkDevice_T other;
    init_device(&other, &f);
    assert(vkWaitSemaphoresKHR(&other, &all, 0) == VK_ERROR_UNKNOWN);
    assert(host_signal(&other, a, 10) == VK_ERROR_UNKNOWN && counter(&d, a) == 9);

    /* Full-width arithmetic at the top of the range: a zero payload can be
     * signalled straight to UINT64_MAX (difference UINT64_MAX) and a wait on
     * UINT64_MAX is ordered correctly on both sides of it. */
    assert(wait_value(&d, b, UINT64_MAX, 0) == VK_TIMEOUT);
    assert(host_signal(&d, b, UINT64_MAX - 1) == VK_SUCCESS);
    assert(wait_value(&d, b, UINT64_MAX, 0) == VK_TIMEOUT);
    assert(wait_value(&d, b, UINT64_MAX - 1, 0) == VK_SUCCESS);
    assert(host_signal(&d, b, UINT64_MAX) == VK_SUCCESS);
    assert(wait_value(&d, b, UINT64_MAX, 0) == VK_SUCCESS && counter(&d, b) == UINT64_MAX);
    VkSemaphore c = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, 0);
    assert(host_signal(&d, c, UINT64_MAX) == VK_SUCCESS && counter(&d, c) == UINT64_MAX);

    d.lost = VK_TRUE;
    uint64_t value;
    assert(vkGetSemaphoreCounterValueKHR(&d, a, &value) == VK_ERROR_DEVICE_LOST);
    assert(wait_value(&d, a, 1, 0) == VK_ERROR_DEVICE_LOST);
    assert(host_signal(&d, a, 10) == VK_ERROR_DEVICE_LOST);
    d.lost = VK_FALSE;
    vkDestroySemaphore(&d, a, NULL);
    vkDestroySemaphore(&d, b, NULL);
    vkDestroySemaphore(&d, c, NULL);
    assert(!d.semaphores && !d.lifetime_errors);
}

/* A queue wait on a value nobody has signalled yet must not block the submit,
 * and the signal it carries must stay invisible until the job completes. */
static void wait_before_signal(void)
{
    struct fixture f = {0};
    struct VkDevice_T d;
    init_device(&d, &f);
    VkCommandPool pool;
    VkCommandBuffer c = command_buffer(&d, &pool, 0);
    VkSemaphore gate = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, 0);
    VkSemaphore done = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, 10);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    assert(vkCreateFence(&d, &fi, NULL, &fence) == VK_SUCCESS);

    assert(submit(&d, gate, 5, done, 20, c, fence) == VK_SUCCESS);
    /* Nothing prepared (the host may still write what the job reads),
     * nothing launched, nothing paused, and the payload has not moved. */
    assert(!f.prepares && !f.launches && !f.pauses);
    assert(d.submission && c->state == PS5VK_PENDING && fence->pending_serial);
    assert(gate->pending == 1 && done->pending == 1);
    assert(counter(&d, done) == 10 && wait_value(&d, done, 20, 0) == VK_TIMEOUT);
    /* The queued signal also bounds later operations on the same payload. */
    assert(host_signal(&d, done, 20) == VK_ERROR_UNKNOWN);
    assert(host_signal(&d, done, 25) == VK_ERROR_UNKNOWN);
    assert(submit(&d, NULL, 0, done, 20, NULL, NULL) == VK_ERROR_UNKNOWN);
    /* An empty submission queued behind the blocked head. */
    assert(submit(&d, NULL, 0, done, 21, NULL, NULL) == VK_SUCCESS);
    assert(!f.launches && d.queue.next_serial == 3);
    /* A host value below the next queued signal is still legal. */
    assert(host_signal(&d, done, 15) == VK_SUCCESS && counter(&d, done) == 15);

    /* Releasing the gate launches the job, but its signal is published only
     * after the backend reports exact completion. */
    assert(host_signal(&d, gate, 4) == VK_SUCCESS);
    assert(wait_value(&d, done, 20, 0) == VK_TIMEOUT && !f.launches);
    assert(host_signal(&d, gate, 5) == VK_SUCCESS);
    assert(!f.launches);  /* the host signal itself never runs queue work */
    assert(wait_value(&d, done, 20, 0) == VK_TIMEOUT);
    assert(f.prepares == 1 && f.launches == 1 && f.launched[0] == 1);
    assert(counter(&d, done) == 15 && !fence->signaled);
    f.complete = 1;
    assert(wait_value(&d, done, 21, 0) == VK_SUCCESS);
    assert(counter(&d, done) == 21 && fence->signaled && !fence->pending_serial);
    assert(!d.submission && c->state == PS5VK_EXECUTABLE && f.releases == 1);
    assert(!gate->pending && !done->pending);
    assert(d.queue.completed_serial == 2 && d.queue.next_serial == 3);

    /* Already satisfied: a wait on a passed value never blocks or defers. */
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    assert(submit(&d, gate, 3, done, 22, c, fence) == VK_SUCCESS);
    assert(f.prepares == 2 && f.launches == 2 && d.submission);
    assert(vkWaitForFences(&d, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
    assert(counter(&d, done) == 22 && !d.submission);

    vkDestroyFence(&d, fence, NULL);
    vkDestroySemaphore(&d, gate, NULL);
    vkDestroySemaphore(&d, done, NULL);
    vkDestroyCommandPool(&d, pool, NULL);
    assert(!d.semaphores && !d.fences && !d.lifetime_errors);
}

/* Twelve chained submissions each wait for the previous value, like the
 * upstream host_wait_before_signal case, then retire in queue order. */
static void ordered_chain(void)
{
    struct fixture f = {.auto_complete = 1};
    struct VkDevice_T d;
    init_device(&d, &f);
    VkCommandPool pool;
    VkCommandBuffer c = command_buffer(&d, &pool, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
    VkSemaphore s = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, 0);
    uint64_t values[13] = {100};
    for (unsigned i = 1; i < 13; ++i) values[i] = values[i - 1] + 1000 * i;
    for (unsigned i = 0; i < 12; ++i)
        assert(submit(&d, s, values[i], s, values[i + 1], c, NULL) == VK_SUCCESS);
    assert(!f.prepares && !f.launches && c->pending_count == 12);
    for (unsigned i = 0; i < 13; ++i) assert(wait_value(&d, s, values[i], 0) == VK_TIMEOUT);
    /* A host value at or above the first queued signal would overtake it. */
    assert(host_signal(&d, s, values[1]) == VK_ERROR_UNKNOWN);
    assert(host_signal(&d, s, values[0]) == VK_SUCCESS);
    assert(wait_value(&d, s, values[12], UINT64_MAX) == VK_SUCCESS);
    assert(counter(&d, s) == values[12] && f.launches == 12 && f.releases == 12);
    for (unsigned i = 0; i < 12; ++i) assert(f.launched[i] == i + 1);
    assert(!d.submission && !c->pending_count && !s->pending);
    vkDestroySemaphore(&d, s, NULL);
    vkDestroyCommandPool(&d, pool, NULL);
}

/* Binary semaphores keep their Vulkan 1.0 rules while work is queued behind a
 * timeline wait: validation replays the queued operations. */
static void binary_behind_timeline(void)
{
    struct fixture f = {.auto_complete = 1};
    struct VkDevice_T d;
    init_device(&d, &f);
    VkSemaphore gate = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, 0);
    VkSemaphoreCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore binary;
    assert(vkCreateSemaphore(&d, &bi, NULL, &binary) == VK_SUCCESS);

    /* Head: timeline wait + binary signal, both in one batch. */
    VkSemaphore waits[] = {gate};
    VkSemaphore signals[] = {binary};
    uint64_t wait_values[] = {1};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkTimelineSemaphoreSubmitInfo tl = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = wait_values};
    VkSubmitInfo head = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tl,
        .waitSemaphoreCount = 1, .pWaitSemaphores = waits, .pWaitDstStageMask = &stage,
        .signalSemaphoreCount = 1, .pSignalSemaphores = signals};
    assert(vkQueueSubmit(&d.queue, 1, &head, NULL) == VK_SUCCESS);
    assert(!binary->signaled && d.submission);
    /* Its queued binary signal satisfies a later binary wait... */
    VkSubmitInfo consume = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = signals, .pWaitDstStageMask = &stage};
    assert(vkQueueSubmit(&d.queue, 1, &consume, NULL) == VK_SUCCESS);
    /* ...exactly once. */
    assert(vkQueueSubmit(&d.queue, 1, &consume, NULL) == VK_ERROR_UNKNOWN);
    /* And a second signal of a pending-signalled binary is refused. */
    VkSubmitInfo resignal = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount = 1, .pSignalSemaphores = signals};
    assert(vkQueueSubmit(&d.queue, 1, &resignal, NULL) == VK_SUCCESS);
    assert(vkQueueSubmit(&d.queue, 1, &resignal, NULL) == VK_ERROR_UNKNOWN);
    assert(binary->pending == 3);
    assert(host_signal(&d, gate, 1) == VK_SUCCESS);
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
    assert(!d.submission && binary->signaled && !binary->pending && !d.lost);
    vkDestroySemaphore(&d, binary, NULL);
    vkDestroySemaphore(&d, gate, NULL);
}

static void submit_validation(void)
{
    struct fixture f = {.auto_complete = 1};
    struct VkDevice_T d;
    init_device(&d, &f);
    VkSemaphore t = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, 5);
    struct batch b;
    const VkSubmitInfo *info = make_batch(&b, t, 1, t, 6, NULL);
    /* A timeline semaphore needs the values structure and matching counts. */
    b.info.pNext = NULL;
    assert(vkQueueSubmit(&d.queue, 1, info, NULL) == VK_ERROR_UNKNOWN);
    b.info.pNext = &b.values;
    b.values.waitSemaphoreValueCount = 0; b.values.pWaitSemaphoreValues = NULL;
    assert(vkQueueSubmit(&d.queue, 1, info, NULL) == VK_ERROR_UNKNOWN);
    info = make_batch(&b, t, 1, t, 6, NULL);
    b.values.signalSemaphoreValueCount = 2;
    assert(vkQueueSubmit(&d.queue, 1, info, NULL) == VK_ERROR_UNKNOWN);
    /* A non-zero count needs its array. */
    info = make_batch(&b, t, 1, t, 6, NULL);
    b.values.pSignalSemaphoreValues = NULL;
    assert(vkQueueSubmit(&d.queue, 1, info, NULL) == VK_ERROR_UNKNOWN);
    /* The structure is unknown without the extension, and not twice. */
    info = make_batch(&b, t, 1, t, 6, NULL);
    d.timeline_extension_enabled = VK_FALSE;
    assert(vkQueueSubmit(&d.queue, 1, info, NULL) == VK_ERROR_UNKNOWN);
    d.timeline_extension_enabled = VK_TRUE;
    VkTimelineSemaphoreSubmitInfo duplicate = b.values;
    b.values.pNext = &duplicate;
    assert(vkQueueSubmit(&d.queue, 1, info, NULL) == VK_ERROR_UNKNOWN);
    b.values.pNext = NULL;
    /* Signals must exceed the current payload, and each other in order. */
    info = make_batch(&b, NULL, 0, t, 5, NULL);
    assert(vkQueueSubmit(&d.queue, 1, info, NULL) == VK_ERROR_UNKNOWN);
    struct batch two[2];
    VkSubmitInfo pair[2] = {*make_batch(&two[0], NULL, 0, t, 8, NULL),
                            *make_batch(&two[1], NULL, 0, t, 7, NULL)};
    assert(vkQueueSubmit(&d.queue, 2, pair, NULL) == VK_ERROR_UNKNOWN);
    assert(!d.submission && counter(&d, t) == 5 && !t->pending && !f.launches);
    pair[1] = *make_batch(&two[1], NULL, 0, t, 9, NULL);
    assert(vkQueueSubmit(&d.queue, 2, pair, NULL) == VK_SUCCESS);
    assert(counter(&d, t) == 9);
    /* A binary-only submission may carry the structure with empty arrays. */
    VkTimelineSemaphoreSubmitInfo empty = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    VkSubmitInfo plain = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &empty};
    assert(vkQueueSubmit(&d.queue, 1, &plain, NULL) == VK_SUCCESS);
    vkDestroySemaphore(&d, t, NULL);
    assert(!d.semaphores && !d.lifetime_errors);
}

/* Failure paths publish nothing: a refused submission leaves no pending
 * signal, and a job that fails at the head never advances the payload. */
static void failure_rollback(void)
{
    struct fixture f = {0};
    struct VkDevice_T d;
    init_device(&d, &f);
    VkCommandPool pool;
    VkCommandBuffer c = command_buffer(&d, &pool, 0);
    VkSemaphore t = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, 0);

    /* Prepare failure at submission (no timeline wait, so prepared now). */
    f.prepare_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint64_t serial = d.queue.next_serial;
    assert(submit(&d, NULL, 0, t, 3, c, NULL) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    assert(!d.submission && !t->pending && c->state == PS5VK_EXECUTABLE &&
           d.queue.next_serial == serial && counter(&d, t) == 0);
    /* The refused signal is not pending: a host signal past it is legal. */
    assert(host_signal(&d, t, 3) == VK_SUCCESS);
    f.prepare_result = VK_SUCCESS;

    /* Deferred prepare failure at the queue head loses the device and
     * publishes neither the timeline signal nor the fence. */
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    assert(vkCreateFence(&d, &fi, NULL, &fence) == VK_SUCCESS);
    const unsigned prepares = f.prepares;
    assert(submit(&d, t, 4, t, 6, c, fence) == VK_SUCCESS && f.prepares == prepares);
    f.prepare_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(host_signal(&d, t, 4) == VK_SUCCESS);
    assert(wait_value(&d, t, 6, 0) == VK_ERROR_DEVICE_LOST);
    assert(d.lost && t->value == 4 && !fence->signaled && d.submission);
    /* Destroying a semaphore still referenced by queued work is retained. */
    vkDestroySemaphore(&d, t, NULL);
    assert(d.semaphores == t && d.lifetime_errors == 1);
    /* Synthetic teardown of the fault (no GPU owns anything here). */
    d.lost = VK_FALSE; f.prepare_result = VK_SUCCESS; f.complete = 1;
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
    assert(!d.submission && t->value == 6 && fence->signaled);

    /* Launch failure: device lost, payload unchanged. */
    assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
    f.launch_result = VK_ERROR_DEVICE_LOST; f.complete = 0;
    assert(submit(&d, t, 6, t, 7, c, fence) == VK_ERROR_DEVICE_LOST);
    assert(d.lost && t->value == 6 && !fence->signaled);
    uint64_t value = 0;
    assert(vkGetSemaphoreCounterValueKHR(&d, t, &value) == VK_ERROR_DEVICE_LOST);
    d.lost = VK_FALSE; f.launch_result = VK_SUCCESS; f.complete = 1;
    assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS && t->value == 7);

    vkDestroySemaphore(&d, t, NULL);
    vkDestroyFence(&d, fence, NULL);
    vkDestroyCommandPool(&d, pool, NULL);
    assert(!d.semaphores && !d.fences);
}

/* Two threads: the main thread queues work that waits on a value, returns
 * from vkQueueSubmit without blocking, then blocks in vkWaitSemaphoresKHR;
 * another thread publishes the value from the host. */
struct signaller {
    VkDevice device;
    VkSemaphore gate;
    uint64_t value;
    volatile int go;
    VkResult result;
};
static void *signal_thread(void *opaque)
{
    struct signaller *s = opaque;
    while (!__atomic_load_n(&s->go, __ATOMIC_ACQUIRE)) {
        struct timespec ts = {0, 50000};
        nanosleep(&ts, NULL);
    }
    struct timespec ts = {0, 2000000};
    nanosleep(&ts, NULL);
    s->result = host_signal(s->device, s->gate, s->value);
    return NULL;
}
static void cross_thread(int wait_idle)
{
    struct fixture f = {.auto_complete = 1, .real_time = 1};
    struct VkDevice_T d;
    init_device(&d, &f);
    VkCommandPool pool;
    VkCommandBuffer c = command_buffer(&d, &pool, 0);
    VkSemaphore gate = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, 0);
    VkSemaphore done = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, 0);
    struct signaller s = {.device = &d, .gate = gate, .value = 1};
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, signal_thread, &s));
    /* The signaller does not start until the submission has returned, so a
     * submit that blocked on the unsatisfied wait would never return. */
    assert(submit(&d, gate, 1, done, 2, c, NULL) == VK_SUCCESS);
    assert(counter(&d, done) == 0);
    __atomic_store_n(&s.go, 1, __ATOMIC_RELEASE);
    if (wait_idle) assert(vkQueueWaitIdle(&d.queue) == VK_SUCCESS);
    assert(wait_value(&d, done, 2, UINT64_MAX) == VK_SUCCESS);
    assert(!pthread_join(thread, NULL) && s.result == VK_SUCCESS);
    assert(counter(&d, gate) == 1 && counter(&d, done) == 2 && !d.submission);
    vkDestroySemaphore(&d, gate, NULL);
    vkDestroySemaphore(&d, done, NULL);
    vkDestroyCommandPool(&d, pool, NULL);
}

/* The upstream max_difference_value shape: one thread polls the counter
 * while the main thread submits and waits on fences; the payload must never
 * go backwards and the reads must not race queue retirement. */
struct observer { VkDevice device; VkSemaphore semaphore; volatile int stop; int backwards; };
static void *observe_thread(void *opaque)
{
    struct observer *o = opaque;
    uint64_t last = 0;
    while (!__atomic_load_n(&o->stop, __ATOMIC_ACQUIRE)) {
        uint64_t value = counter(o->device, o->semaphore);
        if (value < last) o->backwards = 1;
        last = value;
    }
    return NULL;
}
static void concurrent_counter(void)
{
    struct fixture f = {.auto_complete = 1, .real_time = 1};
    struct VkDevice_T d;
    init_device(&d, &f);
    VkCommandPool pool;
    VkCommandBuffer c = command_buffer(&d, &pool, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
    VkSemaphore s = make_semaphore(&d, VK_SEMAPHORE_TYPE_TIMELINE, 0);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    assert(vkCreateFence(&d, &fi, NULL, &fence) == VK_SUCCESS);
    struct observer o = {.device = &d, .semaphore = s};
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, observe_thread, &o));
    assert(host_signal(&d, s, 1) == VK_SUCCESS);
    uint64_t front = 1;
    for (unsigned i = 0; i < 20; ++i) {
        for (unsigned j = 0; j < 5; ++j)
            assert(submit(&d, NULL, 0, s, ++front, c, NULL) == VK_SUCCESS);
        front += UINT64_C(0x100000000);
        assert(submit(&d, NULL, 0, s, front, c, fence) == VK_SUCCESS);
        assert(vkWaitForFences(&d, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
        assert(vkResetFences(&d, 1, &fence) == VK_SUCCESS);
        assert(counter(&d, s) == front);
    }
    __atomic_store_n(&o.stop, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(thread, NULL) && !o.backwards);
    assert(vkDeviceWaitIdle(&d) == VK_SUCCESS);
    vkDestroyFence(&d, fence, NULL);
    vkDestroySemaphore(&d, s, NULL);
    vkDestroyCommandPool(&d, pool, NULL);
}

int main(void)
{
    creation_contract();
    host_signal_and_wait();
    wait_before_signal();
    ordered_chain();
    binary_behind_timeline();
    submit_validation();
    failure_rollback();
    cross_thread(0);
    cross_thread(1);
    concurrent_counter();
    puts("Timeline semaphores: pass (control backend only, no GPU work)");
    return 0;
}
