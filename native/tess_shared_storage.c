#include "tess_shared_storage.h"
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>

struct ps5vk_tess_storage {
    void *owner, *address, *backing;
    struct ps5vk_memory_backend memory;
    VkDeviceSize bytes;
    ps5vk_tess_storage_init initialize;
    unsigned references;
    struct ps5vk_tess_storage *next;
};
/* Serializes first initialization and last release, including backend calls.
 * No registry entry can be observed half initialized or resurrected during
 * destruction. This is a host lifetime lock, never held across GPU execution. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct ps5vk_tess_storage *objects;

static int same_backend(const struct ps5vk_memory_backend *a,
                        const struct ps5vk_memory_backend *b)
{
    return a->context == b->context && a->allocate == b->allocate &&
        a->release == b->release && a->flush == b->flush &&
        a->invalidate == b->invalidate;
}

VkResult ps5vk_tess_storage_acquire(void *owner,
    const struct ps5vk_memory_backend *memory, VkDeviceSize bytes,
    ps5vk_tess_storage_init initialize, struct ps5vk_tess_storage **out)
{
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = NULL;
    if (!owner || !memory || !memory->allocate || !memory->release ||
        !memory->flush || !bytes || !initialize)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pthread_mutex_lock(&lock)) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult rc = VK_SUCCESS;
    struct ps5vk_tess_storage *p;
    for (p = objects; p; p = p->next) {
        if (p->owner != owner) continue;
        if (p->bytes != bytes || p->initialize != initialize ||
            !same_backend(&p->memory, memory)) {
            rc = VK_ERROR_INITIALIZATION_FAILED;
        } else if (p->references == UINT_MAX) {
            rc = VK_ERROR_TOO_MANY_OBJECTS;
        } else {
            ++p->references;
            *out = p;
        }
        goto done;
    }
    p = calloc(1, sizeof(*p));
    if (!p) { rc = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    p->owner = owner; p->memory = *memory; p->bytes = bytes;
    p->initialize = initialize;
    rc = memory->allocate(memory->context, bytes, &p->address, &p->backing);
    if (rc == VK_SUCCESS && (!p->address || !p->backing))
        rc = VK_ERROR_MEMORY_MAP_FAILED;
    if (rc == VK_SUCCESS)
        rc = initialize(memory, p->address, p->backing, bytes);
    if (rc != VK_SUCCESS) {
        if (p->backing) memory->release(memory->context, p->backing);
        free(p);
        goto done;
    }
    p->references = 1; p->next = objects; objects = p; *out = p;
done:
    pthread_mutex_unlock(&lock);
    return rc;
}

void *ps5vk_tess_storage_address(const struct ps5vk_tess_storage *p)
{ return p ? p->address : NULL; }
void *ps5vk_tess_storage_backing(const struct ps5vk_tess_storage *p)
{ return p ? p->backing : NULL; }

void ps5vk_tess_storage_release(struct ps5vk_tess_storage **reference)
{
    if (!reference || !*reference) return;
    /* Valid references guarantee membership; do not accept arbitrary handles. */
    if (pthread_mutex_lock(&lock)) abort();
    struct ps5vk_tess_storage *p = *reference;
    *reference = NULL;
    if (!--p->references) {
        struct ps5vk_tess_storage **link = &objects;
        while (*link && *link != p) link = &(*link)->next;
        if (!*link) abort();
        *link = p->next;
        p->memory.release(p->memory.context, p->backing);
        free(p);
    }
    pthread_mutex_unlock(&lock);
}
