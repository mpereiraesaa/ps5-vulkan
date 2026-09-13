#include "vk_queue.h"
#include <assert.h>
#include <stdio.h>

static unsigned prepared[2], launched[2], polled[2], released[2];
static int identities[2];
static VkResult prepare_rc, launch_rc, poll_rc;
static uint64_t token;
static uint32_t observed_first, observed_count;
static unsigned observed_event_operations;
static VkResult prep(unsigned n, void **out)
{ ++prepared[n]; *out=prepare_rc ? NULL : &identities[n]; return prepare_rc; }
static VkResult pc(VkDevice d,const struct ps5vk_submission *s,void **out)
{
    (void)d; assert(s->serial==7);
    observed_first=ps5vk_submission_first_operation(s,0);
    observed_count=ps5vk_submission_operation_count(s,0);
    observed_event_operations=0;
    for(uint32_t i=observed_first;i<observed_first+observed_count;++i)
        observed_event_operations+=s->buffers[0]->operations[i].type>=PS5VK_EVENT_SET;
    return prep(0,out);
}
static VkResult pg(VkDevice d,const struct ps5vk_submission *s,void **out)
{ (void)d; assert(s->serial==7); return prep(1,out); }
static unsigned identity(void *p)
{ assert(p==&identities[0] || p==&identities[1]); return p==&identities[1]; }
static VkResult launch(VkDevice d,void *p)
{ (void)d; ++launched[identity(p)]; return launch_rc; }
static VkResult poll(VkDevice d,void *p,uint64_t *out)
{ (void)d; ++polled[identity(p)]; *out=token; return poll_rc; }
static void release(VkDevice d,void *p)
{ (void)d; ++released[identity(p)]; }
static struct VkDevice_T device;
static struct VkCommandBuffer_T command, second;
int main(void)
{
    struct ps5vk_queue_backend compute={pc,launch,poll,release};
    struct ps5vk_queue_backend graphics={pg,launch,poll,release};
    struct ps5vk_submission s={.serial=7,.count=1,.buffers={&command}};
    command.operation_count=1;
    /* Alternate native backend kinds on the same device, with pinned ownership. */
    for (unsigned n=0;n<6;++n) {
        unsigned kind=n%2;
        ps5vk_queue_router_configure(&device,compute,graphics);
        command.operations[0].type=kind ? PS5VK_DRAW_INDEXED : PS5VK_DISPATCH;
        void *job=NULL;
        assert(device.submit_backend.prepare(&device,&s,&job)==VK_SUCCESS && job);
        device.compute_backend=(struct ps5vk_queue_backend){0};
        device.graphics_backend=(struct ps5vk_queue_backend){0};
        assert(device.submit_backend.launch(&device,job)==VK_SUCCESS);
        uint64_t completed=99; token=0;
        assert(device.submit_backend.poll(&device,job,&completed)==VK_SUCCESS && !completed);
        token=7;
        assert(device.submit_backend.poll(&device,job,&completed)==VK_SUCCESS && completed==7);
        device.submit_backend.release(&device,job);
    }
    assert(prepared[0]==3 && prepared[1]==3 && launched[0]==3 && launched[1]==3);
    assert(polled[0]==6 && polled[1]==6 && released[0]==3 && released[1]==3);
    ps5vk_queue_router_configure(&device,compute,graphics);
    command.operations[0].type=PS5VK_DISPATCH;
    command.operation_count=2; command.operations[1].type=PS5VK_BEGIN_RENDER_PASS;
    void *job=(void *)1;
    assert(device.submit_backend.prepare(&device,&s,&job)==VK_ERROR_FEATURE_NOT_PRESENT && !job);
    command.operation_count=1; s.count=2; s.buffers[1]=&second;
    second.operation_count=1; second.operations[0].type=PS5VK_DRAW;
    assert(device.submit_backend.prepare(&device,&s,&job)==VK_ERROR_FEATURE_NOT_PRESENT);
    assert(prepared[0]==3 && prepared[1]==3); /* No speculative child prepare. */
    s.count=1; prepare_rc=VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(device.submit_backend.prepare(&device,&s,&job)==prepare_rc && !job);
    prepare_rc=VK_SUCCESS;
    assert(device.submit_backend.prepare(&device,&s,&job)==VK_SUCCESS);
    launch_rc=VK_ERROR_DEVICE_LOST;
    assert(device.submit_backend.launch(&device,job)==launch_rc);
    assert(released[0]==3); /* Router must not free after launch failure. */
    poll_rc=VK_ERROR_DEVICE_LOST; uint64_t completed=0;
    assert(device.submit_backend.poll(&device,job,&completed)==poll_rc);
    assert(released[0]==3);
    /* Mock cleanup only: this test submitted no hardware work. */
    device.submit_backend.release(&device,job);

    /* A partial range reaches the selected backend unchanged.  Event opcodes
     * on either side are never copied into, or observed by, the child job. */
    command.operation_count=4;
    command.operations[0].type=PS5VK_EVENT_SET;
    command.operations[1].type=PS5VK_BARRIER;
    command.operations[2].type=PS5VK_DISPATCH;
    command.operations[3].type=PS5VK_EVENT_RESET;
    s.first_operation[0]=1;s.operation_count[0]=2;
    job=NULL;launch_rc=poll_rc=VK_SUCCESS;
    assert(device.submit_backend.prepare(&device,&s,&job)==VK_SUCCESS && job);
    assert(observed_first==1 && observed_count==2 && !observed_event_operations);
    device.submit_backend.release(&device,job);
    s.first_operation[0]=4;s.operation_count[0]=1;job=(void *)1;
    assert(device.submit_backend.prepare(&device,&s,&job)==VK_ERROR_UNKNOWN && !job);
    puts("Compute/graphics router: pass (mock ownership and error paths only)");
}
