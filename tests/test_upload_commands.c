#include "upload_commands_ps5.h"
#include <assert.h>
#include <string.h>

static _Alignas(64) unsigned char source[128],destination[256];
static unsigned flushes,plans;
static VkResult span_result,plan_result;
VkResult ps5vk_buffer_span(VkDevice d,VkBuffer b,VkDeviceSize offset,VkDeviceSize size,void **p,VkDeviceSize *n)
{
    assert(d && b && offset<sizeof(source));
    *p=source+offset;*n=size==VK_WHOLE_SIZE?sizeof(source)-offset:size;
    return span_result;
}
VkResult ps5vk_image_span(VkDevice d,VkImage image,void **p,VkDeviceSize *n)
{ assert(d && image);*p=destination;*n=sizeof(destination);return span_result; }
VkResult ps5vk_texture_copy_plan_for_image(VkImage image,VkDeviceSize src,VkDeviceSize dst,
    const VkBufferImageCopy *region,struct ps5vk_texture_copy *out)
{
    assert(image && src==sizeof(source) && dst==sizeof(destination) && region);
    ++plans;
    *out=(struct ps5vk_texture_copy){0,0,16,32,8,2,64,96,2};
    return plan_result;
}
static const void *flushed;static size_t flushed_bytes;
static void flush(const void *p,size_t n)
{
    const unsigned char *at=p;
    assert((at>=source && n<=(size_t)(source+sizeof(source)-at)) ||
           (at>=destination && n<=(size_t)(destination+sizeof(destination)-at)));
    flushed=p;flushed_bytes=n;++flushes;
}
int main(void)
{
    struct VkDevice_T device={0};struct VkImage_T image={0};
    image.info=(VkImageCreateInfo){.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_BC1_RGBA_UNORM_BLOCK,.extent={128,64,1},
        .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer=(VkBuffer)(uintptr_t)1; /* Opaque span-stub identity. */
    /* Mirrors vkImageUtil.cpp::copyBufferToImage's combined host->transfer
     * dependency around the CTS's full-size, tightly packed BC upload. */
    struct ps5vk_operation ops[4]={
        {.type=PS5VK_BARRIER,.src_stage=VK_PIPELINE_STAGE_HOST_BIT,
         .dst_stage=VK_PIPELINE_STAGE_TRANSFER_BIT,.src_access=VK_ACCESS_HOST_WRITE_BIT,
         .dst_access=VK_ACCESS_TRANSFER_READ_BIT,
         .buffer_barrier={.buffer=buffer,.offset=16,.size=32}},
        {.type=PS5VK_IMAGE_BARRIER,.image_barrier={.image=&image,
         .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT}},
        {.type=PS5VK_COPY_BUFFER_IMAGE,.copy_source=buffer,.copy_image=&image,
         .copy_layout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .copy_region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
             .imageExtent={128,64,1}}},
        {.type=PS5VK_IMAGE_BARRIER,.image_barrier={.image=&image,
         .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT}}
    };
    uint32_t words[256]={0},*cursor=words;struct ps5vk_layout_state layouts={0};
    memset(destination,0xa5,sizeof(destination));
    assert(ps5vk_upload_commands(&device,ops,4,NULL,&layouts,&cursor,words+256,flush)==VK_SUCCESS);
    assert(flushes==2 && plans==1 && cursor-words==3*PS5VK_GRAPHICS_ACQUIRE_WORDS+28);
    /* Real DMA emitter, four rows across two layers, DMA_SYNC on the final row. */
    unsigned start=2*PS5VK_GRAPHICS_ACQUIRE_WORDS;
    assert(words[start]==0xc0055000 && words[start+6]==8);
    assert(!(words[start+1]&0x80000000u) && (words[start+22]&0x80000000u));
    assert(words[start+16]==(uint32_t)(uintptr_t)source+64);
    assert(words[start+18]==(uint32_t)(uintptr_t)destination+96);
    for(unsigned i=0;i<sizeof(destination);++i)assert(destination[i]==0xa5);
    assert(image.layout==VK_IMAGE_LAYOUT_UNDEFINED && layouts.count==1);
    assert(ps5vk_layout_require(&layouts,&image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_commit(&layouts)==VK_SUCCESS);
    assert(image.layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    /* The same prelude may be discarded on any failure without publishing a
     * layout transition or touching image data. */
    image.layout=VK_IMAGE_LAYOUT_UNDEFINED;
    for(unsigned failure=0;failure<4;++failure) {
        layouts=(struct ps5vk_layout_state){0};cursor=words;
        span_result=failure==0?VK_ERROR_MEMORY_MAP_FAILED:VK_SUCCESS;
        plan_result=failure==1?VK_ERROR_UNKNOWN:VK_SUCCESS;
        unsigned count=failure==3?3:4;
        if(failure==3)ops[2].type=PS5VK_DISPATCH;
        assert(ps5vk_upload_commands(&device,ops,count,NULL,&layouts,&cursor,
            words+(failure==2?21:256),flush)!=VK_SUCCESS);
        assert(image.layout==VK_IMAGE_LAYOUT_UNDEFINED);
        for(unsigned i=0;i<sizeof(destination);++i)assert(destination[i]==0xa5);
    }
    ops[2].type=PS5VK_COPY_BUFFER_IMAGE;
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,ops+2,1,NULL,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    ops[0].dst_access=VK_ACCESS_SHADER_WRITE_BIT;
    assert(ps5vk_upload_commands(&device,ops,1,NULL,&layouts,&cursor,words+256,flush)==VK_ERROR_FEATURE_NOT_PRESENT);
    /* Exact post-render dependency from the focused upstream fragment-side-
     * effect cases: the SSBO write must reach the host after submission
     * completion.  The range is resolved and host cache lines are flushed
     * before emitting the conservative graphics cache packet. */
    struct ps5vk_operation fragment_host={.type=PS5VK_BARRIER,
        .src_stage=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dst_stage=VK_PIPELINE_STAGE_HOST_BIT,
        .src_access=VK_ACCESS_SHADER_WRITE_BIT,
        .dst_access=VK_ACCESS_HOST_READ_BIT,
        .buffer_barrier={.buffer=buffer,.offset=8,.size=64}};
    struct ps5vk_operation fragment_host_pair[2]={fragment_host,
        {.type=PS5VK_BARRIER,
         .src_stage=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         .dst_stage=VK_PIPELINE_STAGE_HOST_BIT}};
    cursor=words;flushes=0;flushed=NULL;flushed_bytes=0;
    assert(ps5vk_upload_commands(&device,fragment_host_pair,2,NULL,&layouts,
        &cursor,words+256,flush)==VK_SUCCESS);
    assert(cursor-words==2*PS5VK_GRAPHICS_ACQUIRE_WORDS && flushes==1 &&
        flushed==source+8 && flushed_bytes==64);
    /* The zero-access aggregate is meaningful only directly after the exact
     * validated buffer dependency.  Never admit it on its own or after a
     * different stage/access pair. */
    cursor=words;
    assert(ps5vk_upload_commands(&device,&fragment_host_pair[1],1,NULL,&layouts,
        &cursor,words+256,flush)==VK_ERROR_FEATURE_NOT_PRESENT && cursor==words);
    fragment_host_pair[0].src_access=VK_ACCESS_SHADER_READ_BIT;cursor=words;
    assert(ps5vk_upload_commands(&device,fragment_host_pair,2,NULL,&layouts,
        &cursor,words+256,flush)==VK_ERROR_FEATURE_NOT_PRESENT && cursor==words);
    fragment_host_pair[0]=fragment_host;
    fragment_host.dst_stage=VK_PIPELINE_STAGE_TRANSFER_BIT;cursor=words;
    assert(ps5vk_upload_commands(&device,&fragment_host,1,NULL,&layouts,
        &cursor,words+256,flush)==VK_ERROR_FEATURE_NOT_PRESENT && cursor==words);
    fragment_host.dst_stage=VK_PIPELINE_STAGE_HOST_BIT;
    fragment_host.src_access=VK_ACCESS_SHADER_READ_BIT;cursor=words;
    assert(ps5vk_upload_commands(&device,&fragment_host,1,NULL,&layouts,
        &cursor,words+256,flush)==VK_ERROR_FEATURE_NOT_PRESENT && cursor==words);
    /* A color transition must also execute without a render pass or target.
     * The real cache packet and tentative layout remain the same. */
    image.info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkAccessFlags scopes[]={0,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    struct ps5vk_operation color={.type=PS5VK_IMAGE_BARRIER,.image_barrier={.image=&image,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    for(unsigned i=0;i<sizeof(scopes)/sizeof(scopes[0]);++i) {
        for(unsigned standalone=0;standalone<2;++standalone) {
            layouts=(struct ps5vk_layout_state){0};cursor=words;
            color.image_barrier.dstAccessMask=scopes[i];
            assert(ps5vk_upload_commands(&device,&color,1,standalone?NULL:&image,
                &layouts,&cursor,words+256,flush)==VK_SUCCESS);
            assert(cursor-words==PS5VK_GRAPHICS_ACQUIRE_WORDS &&
                image.layout==VK_IMAGE_LAYOUT_UNDEFINED && layouts.count==1);
            assert(ps5vk_layout_require(&layouts,&image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
        }
    }
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    color.image_barrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    assert(ps5vk_upload_commands(&device,&color,1,NULL,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    assert(cursor==words && !layouts.count);
    color.image_barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image.info.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
    assert(ps5vk_upload_commands(&device,&color,1,NULL,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    assert(cursor==words && !layouts.count);

    /* Upstream winding re-records the same buffer for opposite culling after
     * reading its first image. The return transition must not be a discard. */
    image.info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    color.image_barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    color.image_barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    color.src_stage=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
    color.dst_stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&color,1,&image,&layouts,&cursor,words+256,flush)==VK_SUCCESS);
    assert(cursor-words==PS5VK_GRAPHICS_ACQUIRE_WORDS);
    assert(image.layout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(ps5vk_layout_require(&layouts,&image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    image.layout=VK_IMAGE_LAYOUT_UNDEFINED;
    assert(ps5vk_upload_commands(&device,&color,1,&image,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    color.image_barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    assert(!ps5vk_color_readback_reuse_barrier(&color.image_barrier));

    /* --- the pinned draw case's prelude -----------------------------------
     * The eight selected upstream draw cases initialise their colour target
     * with UNDEFINED -> GENERAL for a transfer write (TOP_OF_PIPE -> TRANSFER)
     * and then order that write against the colour-attachment stages with a
     * resource-less memory barrier. Both must be executable prelude work, and
     * every neighbouring shape must stay refused. */
    image.info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image.info.format=VK_FORMAT_R8G8B8A8_UNORM;
    image.info.imageType=VK_IMAGE_TYPE_2D;
    image.info.mipLevels=1;image.info.arrayLayers=1;
    image.info.extent.depth=1;image.info.samples=VK_SAMPLE_COUNT_1_BIT;
    image.info.tiling=VK_IMAGE_TILING_OPTIMAL;
    struct ps5vk_operation pinned={.type=PS5VK_IMAGE_BARRIER,
        .src_stage=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        .dst_stage=VK_PIPELINE_STAGE_TRANSFER_BIT,
        .image_barrier={.image=&image,.srcAccessMask=0,
            .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout=VK_IMAGE_LAYOUT_GENERAL}};
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&pinned,1,NULL,&layouts,&cursor,words+256,flush)==VK_SUCCESS);
    assert(cursor-words==PS5VK_GRAPHICS_ACQUIRE_WORDS && layouts.count==1);
    assert(ps5vk_layout_require(&layouts,&image,VK_IMAGE_LAYOUT_GENERAL)==VK_SUCCESS);
    /* Wrong stage, wrong access and a non-GENERAL target stay refused without
     * mutating the cursor or the tentative layout. */
    struct ps5vk_operation rejected=pinned;
    rejected.dst_stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&rejected,1,NULL,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    rejected=pinned;rejected.image_barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    assert(ps5vk_upload_commands(&device,&rejected,1,NULL,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    /* The GENERAL-to-GENERAL form belongs to the linear staging image, not to
     * the colour attachment; and UNDEFINED to SHADER_READ_ONLY is the sampled
     * upload's shape, which starts at TRANSFER_DST_OPTIMAL. */
    rejected=pinned;rejected.image_barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
    assert(ps5vk_upload_commands(&device,&rejected,1,NULL,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    rejected=pinned;rejected.image_barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    assert(ps5vk_upload_commands(&device,&rejected,1,NULL,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    /* A two-sample colour target is not the single-sample role either: the
     * pinned shape is the only one that may take this transition. */
    rejected=pinned;image.info.samples=VK_SAMPLE_COUNT_2_BIT;
    assert(ps5vk_upload_commands(&device,&rejected,1,NULL,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    assert(cursor==words && !layouts.count);

    /* The resource-less barrier that orders that transfer write against the
     * colour-attachment stages. */
    struct ps5vk_operation color_prelude={.type=PS5VK_BARRIER,
        .src_stage=VK_PIPELINE_STAGE_TRANSFER_BIT,
        .dst_stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .src_access=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dst_access=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&color_prelude,1,NULL,&layouts,&cursor,words+256,flush)==VK_SUCCESS);
    assert(cursor-words==PS5VK_GRAPHICS_ACQUIRE_WORDS);
    struct ps5vk_operation bad_prelude=color_prelude;
    bad_prelude.dst_stage=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    cursor=words;
    assert(ps5vk_upload_commands(&device,&bad_prelude,1,NULL,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    bad_prelude=color_prelude;bad_prelude.src_access=VK_ACCESS_SHADER_WRITE_BIT;
    assert(ps5vk_upload_commands(&device,&bad_prelude,1,NULL,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    bad_prelude=color_prelude;bad_prelude.dst_access=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    assert(ps5vk_upload_commands(&device,&bad_prelude,1,NULL,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    assert(cursor==words);
    image.info.samples=VK_SAMPLE_COUNT_1_BIT;

    /* --- whole-subresource depth clear ------------------------------------
     * The emitted work is the uniform DWORD fill, over the whole allocation,
     * of the exact value the frontend recorded. The host writes nothing: the
     * destination bytes are still the guard pattern afterwards. */
    struct VkImage_T depth={.info={.format=VK_FORMAT_D32_SFLOAT,
        .imageType=VK_IMAGE_TYPE_2D,.extent={8,4,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT},
        .layout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
    struct ps5vk_operation clear={.type=PS5VK_CLEAR_DEPTH_STENCIL_IMAGE,
        .image_destination=&depth,
        .image_destination_layout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .clear_word=0x3f800000u,.image_region_count=1};
    layouts=(struct ps5vk_layout_state){0};cursor=words;flushes=0;
    memset(destination,0xa5,sizeof(destination));
    assert(ps5vk_upload_commands(&device,&clear,1,NULL,&layouts,&cursor,words+256,flush)==VK_SUCCESS);
    /* One DMA_DATA packet covers the 256-byte span: immediate DWORD source,
     * L2 destination, DMA_SYNC on the final packet. */
    assert(cursor-words==7);
    assert(words[0]==0xc0055000u && words[1]==(0x40300000u|0x80000000u));
    assert(words[2]==0x3f800000u && words[3]==0);
    assert(words[4]==(uint32_t)(uintptr_t)destination && words[6]==sizeof(destination));
    assert(flushes==1 && flushed==(const void *)destination &&
           flushed_bytes==sizeof(destination));
    for(unsigned i=0;i<sizeof(destination);++i)assert(destination[i]==0xa5);
    /* The emitter refuses a target that is not the advertised depth role, and
     * refuses a layout the recorded operation did not establish. */
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    depth.info.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    assert(ps5vk_upload_commands(&device,&clear,1,NULL,&layouts,&cursor,words+256,flush)==
           VK_ERROR_FEATURE_NOT_PRESENT);
    assert(cursor==words);
    depth.info.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    depth.info.samples=VK_SAMPLE_COUNT_4_BIT;
    assert(ps5vk_upload_commands(&device,&clear,1,NULL,&layouts,&cursor,words+256,flush)==
           VK_ERROR_FEATURE_NOT_PRESENT);
    depth.info.samples=VK_SAMPLE_COUNT_1_BIT;
    depth.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    assert(ps5vk_upload_commands(&device,&clear,1,NULL,&layouts,&cursor,words+256,flush)!=VK_SUCCESS);
    assert(cursor==words);
    /* An arena too small to hold the fill fails instead of emitting a partial
     * clear, and the tentative layout state stays empty. */
    depth.layout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&clear,1,NULL,&layouts,&cursor,words+6,flush)==
           VK_ERROR_UNKNOWN);
    assert(cursor==words);

    /* --- the cleared target becoming a depth attachment --------------------
     * One conservative acquire, a tentative layout that is published only on
     * commit, and the same emission for either fragment-test stage. */
    struct ps5vk_operation to_attachment={.type=PS5VK_IMAGE_BARRIER,
        .src_stage=VK_PIPELINE_STAGE_TRANSFER_BIT,
        .dst_stage=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .image_barrier={.image=&depth,
            .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT}};
    const VkPipelineStageFlags fragment_stages[]={
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT};
    for(unsigned i=0;i<sizeof(fragment_stages)/sizeof(fragment_stages[0]);++i) {
        layouts=(struct ps5vk_layout_state){0};cursor=words;
        to_attachment.dst_stage=fragment_stages[i];
        assert(ps5vk_upload_commands(&device,&to_attachment,1,NULL,&layouts,&cursor,
            words+256,flush)==VK_SUCCESS);
        assert(cursor-words==PS5VK_GRAPHICS_ACQUIRE_WORDS && layouts.count==1);
        assert(depth.layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        assert(ps5vk_layout_require(&layouts,&depth,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    }
    /* Outside the contract nothing is emitted: a stage that is not a fragment
     * test, an incomplete access pair, and a target that is not a depth role. */
    to_attachment.dst_stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&to_attachment,1,NULL,&layouts,&cursor,
        words+256,flush)==VK_ERROR_FEATURE_NOT_PRESENT);
    assert(cursor==words && !layouts.count);
    to_attachment.dst_stage=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    to_attachment.image_barrier.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    assert(ps5vk_upload_commands(&device,&to_attachment,1,NULL,&layouts,&cursor,
        words+256,flush)==VK_ERROR_FEATURE_NOT_PRESENT);
    to_attachment.image_barrier.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|
                                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth.info.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    assert(ps5vk_upload_commands(&device,&to_attachment,1,NULL,&layouts,&cursor,
        words+256,flush)==VK_ERROR_FEATURE_NOT_PRESENT);
    assert(cursor==words && !layouts.count);

    /* Array colour clear is real DMA work, not the legacy linear CPU clear.
     * Preparation preserves bytes and commits no layout. */
    image.info.arrayLayers=6;
    image.info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    image.layout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageSubresourceRange all={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,6};
    struct ps5vk_operation array_clear={.type=PS5VK_CLEAR_COLOR_IMAGE,
        .image_destination=&image,.image_destination_layout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .owned_payload=&all,.owned_payload_size=sizeof(all),.image_region_count=1,
        .clear_word=0x12345678};
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&array_clear,1,NULL,&layouts,&cursor,
        words+256,flush)==VK_SUCCESS);
    assert(cursor-words==7 && words[0]==0xc0055000 && words[2]==0x12345678 &&
        words[6]==sizeof(destination));
    for(unsigned i=0;i<sizeof(destination);++i)assert(destination[i]==0xa5);
    assert(image.layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for(unsigned bad=0;bad<3;++bad) {
        all=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,6};
        if(bad==0)all.layerCount=5;
        if(bad==1)all.baseArrayLayer=1;
        if(bad==2)all.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
        cursor=words;
        assert(ps5vk_upload_commands(&device,&array_clear,1,NULL,&layouts,&cursor,
            words+256,flush)!=VK_SUCCESS && cursor==words);
    }
    /* Original TextureRenderer image setup for compressed sampling: after the
     * target clear, TRANSFER_WRITE becomes COLOR_ATTACHMENT_WRITE with
     * ALL_COMMANDS as the destination stage. */
    struct VkImage_T texture_test_target={0};
    texture_test_target.info=(VkImageCreateInfo){.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={128,64,1},.mipLevels=1,
        .arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
               VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    texture_test_target.layout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    assert(ps5vk_colour_transfer_image(&texture_test_target));
    struct ps5vk_operation texture_target_handover={.type=PS5VK_IMAGE_BARRIER,
        .src_stage=VK_PIPELINE_STAGE_TRANSFER_BIT,
        .dst_stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        .image_barrier={.image=&texture_test_target,
            .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT}};
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&texture_target_handover,1,NULL,&layouts,
        &cursor,words+256,flush)==VK_SUCCESS);
    assert(cursor-words==PS5VK_GRAPHICS_ACQUIRE_WORDS && layouts.count==1 &&
        ps5vk_layout_require(&layouts,&texture_test_target,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    texture_target_handover.image_barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&texture_target_handover,1,NULL,&layouts,
        &cursor,words+256,flush)==VK_ERROR_FEATURE_NOT_PRESENT && !layouts.count &&
        cursor==words);
    texture_test_target.layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    struct ps5vk_operation texture_target_return={.type=PS5VK_IMAGE_BARRIER,
        .src_stage=VK_PIPELINE_STAGE_TRANSFER_BIT,
        .dst_stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .image_barrier={.image=&texture_test_target,
            .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT}};
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&texture_target_return,1,NULL,&layouts,
        &cursor,words+256,flush)==VK_SUCCESS);
    assert(cursor-words==PS5VK_GRAPHICS_ACQUIRE_WORDS && layouts.count==1 &&
        ps5vk_layout_require(&layouts,&texture_test_target,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    texture_target_return.image_barrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&texture_target_return,1,NULL,&layouts,
        &cursor,words+256,flush)==VK_ERROR_FEATURE_NOT_PRESENT && !layouts.count &&
        cursor==words);
    struct ps5vk_operation color_transition={.type=PS5VK_IMAGE_BARRIER,
        .image_barrier={.image=&image,.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT}};
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&color_transition,1,NULL,&layouts,&cursor,
        words+256,flush)==VK_SUCCESS);
    assert(ps5vk_layout_require(&layouts,&image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    color_transition.image_barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_transition.image_barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    color_transition.image_barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    assert(ps5vk_upload_commands(&device,&color_transition,1,NULL,&layouts,&cursor,
        words+256,flush)==VK_SUCCESS);
    assert(ps5vk_layout_require(&layouts,&image,VK_IMAGE_LAYOUT_GENERAL)==VK_SUCCESS);
    assert(image.layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    struct VkImage_T d16={.device=&device,.layout=VK_IMAGE_LAYOUT_UNDEFINED,
        .info={.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_D16_UNORM,
        .extent={128,128,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT}};
    struct ps5vk_operation d16_transition={.type=PS5VK_IMAGE_BARRIER,
        .src_stage=VK_PIPELINE_STAGE_HOST_BIT,
        .dst_stage=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .image_barrier={.image=&d16,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT}};
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&d16_transition,1,NULL,&layouts,&cursor,
        words+256,flush)==VK_SUCCESS);
    assert(cursor-words==PS5VK_GRAPHICS_ACQUIRE_WORDS && layouts.count==1);
    assert(ps5vk_layout_require(&layouts,&d16,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    d16_transition.src_stage=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&d16_transition,1,NULL,&layouts,&cursor,
        words+256,flush)==VK_SUCCESS);
    d16_transition.src_stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    layouts=(struct ps5vk_layout_state){0};cursor=words;
    assert(ps5vk_upload_commands(&device,&d16_transition,1,NULL,&layouts,&cursor,
        words+256,flush)==VK_ERROR_FEATURE_NOT_PRESENT && !layouts.count && cursor==words);
    d16_transition.src_stage=VK_PIPELINE_STAGE_HOST_BIT;
    d16_transition.image_barrier.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    assert(ps5vk_upload_commands(&device,&d16_transition,1,NULL,&layouts,&cursor,
        words+256,flush)==VK_ERROR_FEATURE_NOT_PRESENT && !layouts.count && cursor==words);
}
