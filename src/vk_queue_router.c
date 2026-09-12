#include "vk_queue.h"
#include <stdlib.h>

/* One native job per submission; interleaving submissions is supported, mixing
 * dispatch and rendering within one submission is explicitly not yet supported.
 * Snapshot callbacks into the job so retirement always uses its owning backend. */
struct routed_job { struct ps5vk_queue_backend backend; void *child; };
static VkResult prepare(VkDevice d, const struct ps5vk_submission *s, void **out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    *out = NULL;
    if (!d || !s || !s->count || s->count > PS5VK_MAX_SUBMITTED_BUFFERS)
        return VK_ERROR_UNKNOWN;
    unsigned compute=0, graphics=0;
    for (unsigned i=0; i<s->count; ++i) {
        VkCommandBuffer c=s->buffers[i];
        if (!c || c->operation_count>PS5VK_MAX_OPERATIONS) return VK_ERROR_UNKNOWN;
        for (unsigned k=0; k<c->operation_count; ++k) {
            switch (c->operations[k].type) {
            case PS5VK_DISPATCH: compute=1; break;
            case PS5VK_BARRIER: break;
            case PS5VK_BEGIN_RENDER_PASS: case PS5VK_END_RENDER_PASS:
            case PS5VK_DRAW: case PS5VK_DRAW_INDEXED:
            case PS5VK_COPY_BUFFER_IMAGE: case PS5VK_IMAGE_BARRIER:
                graphics=1; break;
            default: return VK_ERROR_FEATURE_NOT_PRESENT;
            }
        }
    }
    if (compute && graphics) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_queue_backend b=graphics ? d->graphics_backend : d->compute_backend;
    if (!b.prepare || !b.launch || !b.poll || !b.release)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct routed_job *j=calloc(1,sizeof(*j));
    if (!j) return VK_ERROR_OUT_OF_HOST_MEMORY;
    j->backend=b;
    VkResult rc=b.prepare(d,s,&j->child);
    if (rc!=VK_SUCCESS) { free(j); return rc; }
    *out=j; return VK_SUCCESS;
}
static VkResult launch(VkDevice d, void *opaque)
{ struct routed_job *j=opaque; return j->backend.launch(d,j->child); }
static VkResult poll(VkDevice d, void *opaque, uint64_t *completed)
{ struct routed_job *j=opaque; return j->backend.poll(d,j->child,completed); }
static void release(VkDevice d, void *opaque)
{ struct routed_job *j=opaque; j->backend.release(d,j->child); free(j); }
void ps5vk_queue_router_configure(VkDevice d, struct ps5vk_queue_backend compute,
                                struct ps5vk_queue_backend graphics)
{
    d->compute_backend=compute; d->graphics_backend=graphics;
    d->submit_backend=(struct ps5vk_queue_backend){prepare,launch,poll,release};
}
