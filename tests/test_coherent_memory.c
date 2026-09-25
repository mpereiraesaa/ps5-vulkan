/* Driver-maintained HOST_COHERENT memory: the optional second memory type,
 * its allocation and requirements, and the CPU cache maintenance the driver
 * performs instead of the application - invalidate on map, writeback on
 * unmap, writeback of every mapped coherent range before a submission
 * launches and invalidate after its completion is observed. The memory and
 * queue backends are mocks that record the order of events; this proves the
 * frontend contract only, not GPU visibility (that is the native witness). */
#include "vk_queue.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_EVENTS = 64 };
struct event { char kind; void *backing; VkDeviceSize offset, size; };
static struct event events[MAX_EVENTS];
static unsigned event_count;
static void record(char kind, void *backing, VkDeviceSize offset, VkDeviceSize size)
{
    assert(event_count < MAX_EVENTS);
    events[event_count++] = (struct event){kind, backing, offset, size};
}
static VkResult memory_allocate(void *ctx, VkDeviceSize n, void **a, void **b)
{ (void)ctx; *a = *b = calloc(1, n); return *a ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY; }
static void memory_release(void *ctx, void *b) { (void)ctx; free(b); }
static VkResult memory_flush(void *ctx, void *b, VkDeviceSize offset, VkDeviceSize bytes)
{ (void)ctx; record('F', b, offset, bytes); return VK_SUCCESS; }
static VkResult memory_invalidate(void *ctx, void *b, VkDeviceSize offset, VkDeviceSize bytes)
{ (void)ctx; record('I', b, offset, bytes); return VK_SUCCESS; }

struct fixture { int complete; uint64_t now; };
struct mock_job { uint64_t serial; };
static VkResult prepare(VkDevice d, const struct ps5vk_submission *s, void **job)
{
    (void)d;
    struct mock_job *prepared = malloc(sizeof(*prepared));
    if (!prepared) return VK_ERROR_OUT_OF_HOST_MEMORY;
    prepared->serial = s->serial; *job = prepared; return VK_SUCCESS;
}
static VkResult launch(VkDevice d, void *job)
{
    (void)job; ((struct fixture *)d->progress.context)->complete = 0;
    record('L', NULL, 0, 0); return VK_SUCCESS;
}
static VkResult poll_backend(VkDevice d, void *job, uint64_t *completed)
{
    struct fixture *f = d->progress.context;
    *completed = f->complete ? ((struct mock_job *)job)->serial : 0;
    return VK_SUCCESS;
}
static void release(VkDevice d, void *job) { (void)d; record('R', NULL, 0, 0); free(job); }
static uint64_t clock_ns(void *ctx) { return ((struct fixture *)ctx)->now; }
static void pause_wait(void *ctx, uint64_t remaining)
{ struct fixture *f = ctx; (void)remaining; ++f->now; f->complete = 1; }

static void profile(void)
{
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    const struct ps5vk_physical_profile_info info = {.name = "host mock, not a GPU",
        .heap_size = 65536, .allocation_granularity = 1, .buffer_image_granularity = 1};
    ps5vk_physical_profile_init(&properties, &memory, &info);
    assert(memory.memoryTypeCount == 1);
    ps5vk_profile_add_coherent_type(&memory);
    assert(memory.memoryTypeCount == 2);
    /* Type 0 is unchanged and first; type 1 adds exactly HOST_COHERENT. */
    assert(memory.memoryTypes[0].propertyFlags ==
           (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));
    assert(memory.memoryTypes[1].propertyFlags ==
           (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    assert(memory.memoryTypes[1].heapIndex == 0);
    assert(ps5vk_physical_profile_valid(&properties, &memory, 65536, VK_QUEUE_COMPUTE_BIT, 0, 0));
    /* Adding twice does nothing. */
    ps5vk_profile_add_coherent_type(&memory);
    assert(memory.memoryTypeCount == 2);
    /* Anything but the exact coherent variant of type 0 is refused. */
    VkPhysicalDeviceMemoryProperties bad = memory;
    bad.memoryTypes[1].propertyFlags |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    assert(!ps5vk_physical_profile_valid(&properties, &bad, 65536, VK_QUEUE_COMPUTE_BIT, 0, 0));
    bad = memory; bad.memoryTypes[1].heapIndex = 1;
    assert(!ps5vk_physical_profile_valid(&properties, &bad, 65536, VK_QUEUE_COMPUTE_BIT, 0, 0));
    bad = memory; bad.memoryTypes[0].propertyFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    assert(!ps5vk_physical_profile_valid(&properties, &bad, 65536, VK_QUEUE_COMPUTE_BIT, 0, 0));
    bad = memory; bad.memoryTypeCount = 3;
    assert(!ps5vk_physical_profile_valid(&properties, &bad, 65536, VK_QUEUE_COMPUTE_BIT, 0, 0));
}

static VkDeviceMemory allocate(VkDevice d, uint32_t type, VkDeviceSize size)
{
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = size, .memoryTypeIndex = type};
    VkDeviceMemory m = VK_NULL_HANDLE;
    assert(vkAllocateMemory(d, &info, NULL, &m) == VK_SUCCESS);
    return m;
}
static void *backing_of(VkDevice d, VkDeviceMemory m)
{
    /* The mock backing is the mapped allocation's base address. */
    void *base = NULL;
    assert(vkMapMemory(d, m, 0, VK_WHOLE_SIZE, 0, &base) == VK_SUCCESS);
    return base;
}
static void submit_and_wait(VkDevice d, VkCommandBuffer command)
{
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    assert(vkQueueSubmit(&d->queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(vkQueueWaitIdle(&d->queue) == VK_SUCCESS);
}

static void driver_maintenance(void)
{
    struct VkPhysicalDevice_T physical = {0};
    const struct ps5vk_physical_profile_info info = {.name = "host mock, not a GPU",
        .heap_size = 65536, .allocation_granularity = 1, .buffer_image_granularity = 1};
    ps5vk_physical_profile_init(&physical.platform.properties,
                                &physical.platform.memory_properties, &info);
    struct fixture f = {0};
    struct VkDevice_T d = {.physical = &physical,
        .memory = {NULL, memory_allocate, memory_release, memory_flush, memory_invalidate},
        .buffer_alignment = 256, .noncoherent_atom = 64, .max_allocation = 65536,
        .progress = {&f, ps5vk_queue_poll, clock_ns, pause_wait},
        .submit_backend = {prepare, launch, poll_backend, release}};
    d.queue.device = &d; d.queue.next_serial = 1;

    /* Without the coherent type only type 0 exists. */
    VkMemoryAllocateInfo second = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 256, .memoryTypeIndex = 1};
    VkDeviceMemory refused = VK_NULL_HANDLE;
    assert(vkAllocateMemory(&d, &second, NULL, &refused) != VK_SUCCESS && !refused);
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 100, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b;
    assert(vkCreateBuffer(&d, &buffer_info, NULL, &b) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(&d, b, &requirements);
    assert(requirements.memoryTypeBits == 1);

    ps5vk_profile_add_coherent_type(&physical.platform.memory_properties);
    vkGetBufferMemoryRequirements(&d, b, &requirements);
    assert(requirements.memoryTypeBits == 3 && requirements.size == 256);
    second.memoryTypeIndex = 2;
    assert(vkAllocateMemory(&d, &second, NULL, &refused) != VK_SUCCESS && !refused);

    VkDeviceMemory plain = allocate(&d, 0, 4096), coherent = allocate(&d, 1, 8192);
    assert(vkBindBufferMemory(&d, b, coherent, 512) == VK_SUCCESS);
    /* Mapping type 0 does nothing to caches; mapping coherent memory
     * invalidates exactly the mapped range. */
    event_count = 0;
    void *plain_base = backing_of(&d, plain);
    assert(event_count == 0);
    vkUnmapMemory(&d, plain);
    assert(event_count == 0);
    void *coherent_base = NULL;
    assert(vkMapMemory(&d, coherent, 1024, 2048, 0, &coherent_base) == VK_SUCCESS);
    assert(event_count == 1 && events[0].kind == 'I' && events[0].offset == 1024 &&
           events[0].size == 2048);
    void *backing = (unsigned char *)coherent_base - 1024;
    plain_base = backing_of(&d, plain);

    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool;
    assert(vkCreateCommandPool(&d, &pool_info, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    assert(vkAllocateCommandBuffers(&d, &command_info, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);

    /* A GPU submission: writeback of the mapped coherent range (only), then
     * launch; after the observed completion, invalidate, then release. */
    event_count = 0;
    submit_and_wait(&d, command);
    assert(event_count == 4);
    assert(events[0].kind == 'F' && events[0].backing == backing &&
           events[0].offset == 1024 && events[0].size == 2048);
    assert(events[1].kind == 'L');
    assert(events[2].kind == 'I' && events[2].backing == backing &&
           events[2].offset == 1024 && events[2].size == 2048);
    assert(events[3].kind == 'R');
    for (unsigned n = 0; n < event_count; ++n) assert(events[n].backing != plain_base);

    /* Unmapping coherent memory writes the range back; afterwards nothing is
     * mapped, so a submission performs no coherent maintenance. */
    event_count = 0;
    vkUnmapMemory(&d, coherent);
    assert(event_count == 1 && events[0].kind == 'F' && events[0].offset == 1024 &&
           events[0].size == 2048);
    event_count = 0;
    assert(ps5vk_coherent_host_writeback(&d) == VK_SUCCESS &&
           ps5vk_coherent_host_invalidate(&d) == VK_SUCCESS && event_count == 0);
    /* Direct hook contract with the range mapped again. */
    assert(vkMapMemory(&d, coherent, 0, VK_WHOLE_SIZE, 0, &coherent_base) == VK_SUCCESS);
    event_count = 0;
    assert(ps5vk_coherent_host_writeback(&d) == VK_SUCCESS);
    assert(ps5vk_coherent_host_invalidate(&d) == VK_SUCCESS);
    assert(event_count == 2 && events[0].kind == 'F' && events[1].kind == 'I' &&
           events[0].offset == 0 && events[0].size == 8192 && events[1].size == 8192);

    vkFreeCommandBuffers(&d, pool, 1, &command);
    vkDestroyCommandPool(&d, pool, NULL);
    vkDestroyBuffer(&d, b, NULL);
    vkFreeMemory(&d, plain, NULL);
    vkFreeMemory(&d, coherent, NULL);
    assert(!d.memories && !d.buffers);
}

/* The queue hook itself: a mock GPU submission of an empty command buffer. */
static void queue_hooks(void)
{
    struct VkPhysicalDevice_T physical = {0};
    const struct ps5vk_physical_profile_info info = {.name = "host mock, not a GPU",
        .heap_size = 65536, .allocation_granularity = 1, .buffer_image_granularity = 1};
    ps5vk_physical_profile_init(&physical.platform.properties,
                                &physical.platform.memory_properties, &info);
    ps5vk_profile_add_coherent_type(&physical.platform.memory_properties);
    struct fixture f = {0};
    struct VkDevice_T d = {.physical = &physical,
        .memory = {NULL, memory_allocate, memory_release, memory_flush, memory_invalidate},
        .buffer_alignment = 256, .noncoherent_atom = 64, .max_allocation = 65536,
        .progress = {&f, ps5vk_queue_poll, clock_ns, pause_wait},
        .submit_backend = {prepare, launch, poll_backend, release}};
    d.queue.device = &d; d.queue.next_serial = 1;
    VkDeviceMemory coherent = allocate(&d, 1, 4096);
    void *base = backing_of(&d, coherent);
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool;
    assert(vkCreateCommandPool(&d, &pool_info, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    assert(vkAllocateCommandBuffers(&d, &command_info, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    assert(vkEndCommandBuffer(command) == VK_SUCCESS);
    event_count = 0;
    submit_and_wait(&d, command);
    assert(event_count == 4);
    assert(events[0].kind == 'F' && events[0].backing == base && events[0].size == 4096);
    assert(events[1].kind == 'L');
    assert(events[2].kind == 'I' && events[2].backing == base && events[2].size == 4096);
    assert(events[3].kind == 'R');
    vkFreeCommandBuffers(&d, pool, 1, &command);
    vkDestroyCommandPool(&d, pool, NULL);
    vkFreeMemory(&d, coherent, NULL);
}

int main(void)
{
    profile();
    driver_maintenance();
    queue_hooks();
    puts("HOST_COHERENT memory: second type, map/unmap and submission-boundary "
         "CPU cache maintenance (host mocks only, no GPU evidence)");
    return 0;
}
