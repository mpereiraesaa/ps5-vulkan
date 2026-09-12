#include "present_ps5.h"
#include "presentation_format_ps5.h"
#include "command_arena_ps5.h"
#include "ps5_videoout.h"
#include "ps5_present.h"
#include "ps5_agc_writer.h"
#include "ps5_agc.h"
#include "ps5_agc_driver.h"
#include "submit_suspend_ps5.h"
#include "ps5_event_adapter.h"
#include "ps5log.h"
#include <time.h>
#include <unistd.h>
#include <string.h>
static const struct ps5_videoout_ops ops={sceVideoOutOpen,sceVideoOutClose,sceVideoOutSetFlipRate,
    sceKernelCreateEqueue,sceKernelDeleteEqueue,sceVideoOutAddFlipEvent,sceVideoOutDeleteFlipEvent,
    sceVideoOutSetBufferAttribute2,sceVideoOutRegisterBuffers2,sceVideoOutUnregisterBuffers};
static void retain(const char *stage,int rc)
{ ps5log_printf(PS5LOG_ERR,"PS5VK_VIDEO_RETAIN stage=%s rc=%d",stage,rc);ps5log_close("video-retained");for(;;)sleep(1); }
static uint64_t now(void)
{ struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec; }
static void cache(const void *p,size_t n)
{
    uintptr_t end=(uintptr_t)p+n;
    for(uintptr_t a=(uintptr_t)p&~(uintptr_t)63;a<end;a+=64)__asm__ volatile("clflush (%0)"::"r"(a):"memory");
    __asm__ volatile("mfence":::"memory");
}
static int flip(uint32_t **c,uint32_t capacity,uint32_t mode,int32_t handle,int32_t index,uint32_t fm,uint64_t token)
{ return ps5_agc_writer_set_flip(c,capacity,mode,handle,index,fm,token,sceAgcDcbSetFlip); }
/* Synchronous native interop, NOT a Vulkan surface/swapchain. Registration
 * retains both images until close. Each frame requires completed rendering;
 * the current display image stays unwritable until the next matching flip. */
VkResult ps5vk_native_present_open(struct ps5vk_native_present *p,VkDevice d,VkImage image,VkImage spare)
{
    if(!p || p->open || p->device || !d || d->lost || d->submission || !image || image->device!=d || image->pending || image->display_busy ||
        image->info.format!=VK_FORMAT_B8G8R8A8_UNORM || image->info.extent.width!=1920 || image->info.extent.height!=1080)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    void *address;VkDeviceSize bytes;
    VkResult result=ps5vk_image_span(d,image,&address,&bytes);if(result!=VK_SUCCESS)return result;
    if((uintptr_t)address%131072 || bytes<UINT64_C(8912896))return VK_ERROR_UNKNOWN;
    if(!spare || spare==image || spare->device!=d || spare->pending || spare->display_busy ||
       spare->info.format!=image->info.format || spare->info.extent.width!=1920 || spare->info.extent.height!=1080)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    void *spare_address;VkDeviceSize spare_bytes;
    result=ps5vk_image_span(d,spare,&spare_address,&spare_bytes);if(result!=VK_SUCCESS)return result;
    if((uintptr_t)spare_address%131072 || spare_bytes<bytes ||
       (uintptr_t)spare_address!=(uintptr_t)address+UINT64_C(0x04000000))return VK_ERROR_UNKNOWN;
    struct ps5vk_command_arena arena={0};result=ps5vk_command_arena_create(&arena);
    if(result==VK_ERROR_DEVICE_LOST)retain("arena",result);
    if(result!=VK_SUCCESS)return result;
    ++image->pending;
    ++spare->pending;
    struct ps5_videoout video={.handle=-1};
    int rc=0;
    const char *stage="open";
    video.handle=ops.open(0xff,0,0,NULL);if(video.handle<0){rc=video.handle;goto fail;}
    stage="create-equeue";
    rc=ops.create_equeue(&video.equeue,"ps5vk-flip");if(rc)goto fail;
    stage="add-flip-event";
    rc=ops.add_flip_event(video.equeue,video.handle,NULL);if(rc)goto fail;video.event_added=1;
    stage="set-flip-rate";
    rc=ops.set_flip_rate(video.handle,0);if(rc)goto fail;
    struct ps5_video_attribute attribute={{0}};
    /* This function already rejects all other image formats before allocation.
     * Keep Vulkan BGRA bytes unchanged; VideoOut must interpret them correctly. */
    const uint64_t format_word=ps5vk_native_video_format(image->info.format);
    ops.set_attribute(&attribute,format_word,0,1920,1080,0,0,0);
    struct ps5_video_buffer buffers[2]={{address,0,0,0},{spare_address,0,0,0}};
    stage="register-buffers";
    rc=ops.register_buffers(video.handle,0,0,buffers,2,&attribute,0,NULL);if(rc)goto fail;
    video.buffers_registered=1;
    ps5log_printf(PS5LOG_MARK,"PS5VK_VIDEO_REGISTER handle=%d buffers=2 image_bytes=%llu format_word=%016llx",video.handle,(unsigned long long)bytes,(unsigned long long)format_word);
    *p=(struct ps5vk_native_present){.device=d,.images={image,spare},.arena=arena,
        .video=video,.displayed=-1,.open=1};
    return VK_SUCCESS;
fail:
    ps5log_printf(PS5LOG_ERR,"PS5VK_VIDEO_SETUP_FAILED stage=%s rc=%d handle=%d image_bytes=%llu",stage,rc,video.handle,(unsigned long long)bytes);
    if(ps5_videoout_close(&video,&ops))retain("setup-rollback",rc);
    if(ps5vk_command_arena_release(&arena)!=VK_SUCCESS)retain("setup-arena",rc);
    --image->pending;--spare->pending;return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult ps5vk_native_present_frame(struct ps5vk_native_present *p,unsigned slot,uint64_t token,unsigned seconds)
{
    if(!p || !p->open || !p->device || p->device->lost || p->device->submission || slot>=2 ||
       !token || token<=p->token || seconds>30 || p->images[slot]->pending!=1 ||
       p->images[slot]->display_busy)return VK_ERROR_UNKNOWN;
    volatile uint64_t *label=ps5vk_command_arena_label(&p->arena);*label=1;
    uint32_t *commands=p->arena.address;
    struct ps5_present_stream stream={commands,commands+PS5VK_COMMAND_ARENA_WORDS,commands,0};
    int rc=ps5_present_compose_flip_and_fence(&stream,flip,p->video.handle,(int32_t)slot,1,token,(uintptr_t)label);
    if(rc)retain("compose",rc);
    cache(p->arena.address,PS5VK_COMMAND_ARENA_BYTES);
    struct ps5_agc_submit submit={commands,(uint32_t)(stream.cursor-commands),0,{0,0,0}};
    struct ps5vk_submit_result result=ps5vk_submit_suspend(&submit);
    rc=result.submit_rc;
    ps5log_printf(PS5LOG_MARK,"PS5VK_VIDEO_SUBMIT token=%llu rc=%d",(unsigned long long)token,rc);
    if(rc)retain("submit",rc);
    rc=result.suspend_rc;
    ps5log_printf(PS5LOG_MARK,"PS5VK_VIDEO_SUSPEND_POINT token=%llu rc=%d",(unsigned long long)token,rc);
    if(rc)retain("suspend-point",rc);
    uint64_t start=now();if(!start)retain("clock",-1);
    while(1) {
        cache((const void *)label,8);
        if(__atomic_load_n(label,__ATOMIC_ACQUIRE)==0)break;
        if(now()-start>UINT64_C(3000000000))retain("fence-timeout",-1);
        usleep(1000);
    }
    struct ps5_frame_completion completion;
    ps5_frame_completion_begin(&completion,token,1);
    uint64_t events[4]={0};start=now();
    for(;;) {
        const struct ps5_event_poll poll={p->video.equeue,events,label,10000,0,now()-start>UINT64_C(3000000000),
            sceKernelWaitEqueue,sceVideoOutGetEventData,NULL};
        rc=ps5_event_poll_completion(&completion,&poll);
        if(rc==PS5_FRAME_DONE)break;
        if(rc<0)retain("flip-event",rc);
    }
    ps5log_printf(PS5LOG_MARK,"PS5VK_VIDEO_PRESENTED token=%llu fence=0 matching_event=1 hold_seconds=%u",(unsigned long long)token,seconds);
    if(p->displayed>=0)p->images[p->displayed]->display_busy=VK_FALSE;
    p->images[slot]->display_busy=VK_TRUE;
    p->displayed=(int)slot;p->token=token;
    sleep(seconds);
    return VK_SUCCESS;
}

VkResult ps5vk_native_present_close(struct ps5vk_native_present *p)
{
    if(!p || !p->open || !p->device || p->device->submission ||
       p->images[0]->pending!=1 || p->images[1]->pending!=1)return VK_ERROR_UNKNOWN;
    int rc=ps5_videoout_close(&p->video,&ops);if(rc)retain("close",rc);
    ps5log_printf(PS5LOG_MARK,"PS5VK_VIDEO_CLOSED token=%llu deferred=%d",(unsigned long long)p->token,p->video.unregister_deferred_to_close);
    VkResult result=ps5vk_command_arena_release(&p->arena);if(result!=VK_SUCCESS)retain("arena-release",result);
    for(unsigned i=0;i<2;++i){p->images[i]->display_busy=VK_FALSE;--p->images[i]->pending;}
    memset(p,0,sizeof(*p));return VK_SUCCESS;
}

VkResult ps5vk_native_present_once(VkDevice d,VkImage image,VkImage spare,uint64_t token,unsigned seconds)
{
    struct ps5vk_native_present p={0};
    VkResult rc=ps5vk_native_present_open(&p,d,image,spare);if(rc!=VK_SUCCESS)return rc;
    rc=ps5vk_native_present_frame(&p,0,token,seconds);
    VkResult closed=ps5vk_native_present_close(&p);
    return rc!=VK_SUCCESS ? rc : closed;
}
