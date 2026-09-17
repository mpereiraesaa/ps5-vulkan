#include "draw_prepare_ps5.h"
#include "vertex_fetch.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static unsigned allocations, releases, targets;
static VkResult target_rc, flush_rc;
static VkDeviceSize fail_allocation_size;
static uintptr_t latest_address;
static _Alignas(4096) unsigned char storage[32768];
static _Alignas(4) unsigned char vertex_source[32];
static size_t expected_bytes=sizeof(struct ps5vk_draw_state);
static VkResult fetch_rc;
static VkResult texture_rc;
static VkImageView texture_fail_view;
static unsigned texture_calls, resource_calls;
static VkResult resource_rc;
static VkImageView resource_fail_view;
static VkResult buffer_rc;
static VkBuffer buffer_fail;
static VkDeviceSize last_buffer_dynamic, first_set_buffer_dynamic;
static unsigned buffer_calls;
static struct ps5vk_runtime_draw_abi runtime;
VkResult ps5vk_texture_descriptor(VkDevice d,VkImageView v,VkSampler s,uint32_t out[12])
{(void)d;(void)s;++texture_calls;for(unsigned i=0;i<12;++i)out[i]=100+i+256*(uintptr_t)v;
 return texture_fail_view && v==texture_fail_view?VK_ERROR_UNKNOWN:texture_rc;}
/* The audited eight-word resource record is produced by the resource-only
 * encoder, whose own test pins a record of exactly 8 DWORDs and no sampler
 * words. This stub owns the placement question instead: which words of which
 * record the input-attachment role writes, and that the combined entry point is
 * never the one asked to write them. It has no sampler parameter at all, which
 * is the same reason the real entry point cannot read the ignored
 * VkDescriptorImageInfo::sampler of an input attachment. */
VkResult ps5vk_image_resource_descriptor(VkDevice d,VkImageView v,uint32_t out[8])
{(void)d;++resource_calls;for(unsigned i=0;i<8;++i)out[i]=300+i+256*(uintptr_t)v;
 return resource_fail_view && v==resource_fail_view?VK_ERROR_UNKNOWN:resource_rc;}
/* The audited GFX1013 buffer record is exercised by its own encoder test; this
 * fixture owns placement, ordering and fail-closed behaviour in the graphics
 * table. */
VkResult ps5vk_buffer_descriptor(VkDevice d,const VkDescriptorBufferInfo *info,
    VkDeviceSize dynamic,uint32_t out[4])
{(void)d;unsigned index=(unsigned)(uintptr_t)info->buffer;
 ++buffer_calls;last_buffer_dynamic=dynamic;
 if((uintptr_t)info->buffer==1)first_set_buffer_dynamic=dynamic;
 for(unsigned i=0;i<4;++i)out[i]=200+i+256*index;
 return buffer_fail && info->buffer==buffer_fail?VK_ERROR_UNKNOWN:buffer_rc;}
static const void *fetch_address=vertex_source;
static unsigned fetch_count=1;
VkResult ps5vk_vertex_fetch_used_spans(VkDevice d,const struct ps5vk_graphics_key *k,
    const struct ps5vk_operation *op,uint32_t mask,struct ps5vk_vertex_fetch_table *out)
{
    (void)d;(void)k;(void)op;(void)mask;
    if(fetch_rc==VK_SUCCESS) {
        *out=(struct ps5vk_vertex_fetch_table){.count=fetch_count};
        out->bindings[0]=(struct ps5vk_vertex_fetch){fetch_address,16,4,4};
        if(fetch_count>1)out->bindings[fetch_count-1]=out->bindings[0];
    }
    return fetch_rc;
}
static VkResult allocate(void *c, VkDeviceSize n, void **a, void **b)
{
    (void)c;
    assert(n<=sizeof(storage));
    if(n==fail_allocation_size)return VK_ERROR_OUT_OF_HOST_MEMORY;
    ++allocations; *a = *b = storage; latest_address=(uintptr_t)*a;return VK_SUCCESS;
}
static void release(void *c, void *b) { (void)c; assert(b==storage);++releases; }
static VkResult flush(void *c, void *b, VkDeviceSize o, VkDeviceSize n)
{ (void)c; assert(b && !o && n == expected_bytes); return flush_rc; }
VkResult ps5vk_native_target(VkDevice d, VkImageView v, const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],
    struct ps5vk_target_registers *out)
{ (void)defaults; assert(v->device == d); ++targets; out->count = 16; return target_rc; }
VkResult ps5vk_native_draw_state(VkPipeline p, const VkViewport *viewport,
    const VkRect2D *scissor, const struct ps5vk_raster_state *raster,
    const struct ps5vk_target_registers *color,
    const struct ps5vk_target_registers *depth, const VkRect2D *area,
    uint32_t width, uint32_t height, struct ps5vk_draw_state *out)
{
    /* The prepared draw hands the operation's OWN raster snapshot to the state
     * builder, never the pipeline's static copy. */
    assert(p && viewport->width == 4 && scissor->extent.width == 4 && raster &&
        raster->depth_bias_enable && raster->depth_bias_slope == 2.0f &&
        color->count == 16 && !depth && area->extent.width == 4 && width == 4 && height == 4);
    *out = (struct ps5vk_draw_state){.cx_count = 87, .modifier = 5,.runtime=runtime}; return VK_SUCCESS;
}
int main(void)
{
    /* Test the allocation bridge independently; target/state behavior has its
     * own tests. These callbacks do not build GPU commands. */
    struct VkDevice_T d = {.memory = {NULL, allocate, release, flush, flush}};
    struct VkImageView_T v = {.device = &d};
    struct VkFramebuffer_T fb = {.device = &d, .width = 4, .height = 4, .attachment_count = 1,
        .attachments = {&v}, .depth_attachment = VK_ATTACHMENT_UNUSED};
    struct ps5vk_subpass pass_subpasses[1] = {{.color = {.attachment = 0},
        .depth = {.attachment = VK_ATTACHMENT_UNUSED}}};
    struct VkRenderPass_T pass = {.device = &d, .subpass_count = 1,
        .subpasses = pass_subpasses};
    struct VkPipeline_T p = {.device = &d};
    struct ps5vk_operation op = {.type = PS5VK_DRAW, .pipeline = &p, .framebuffer = &fb,
        .render_pass = &pass, .viewport = {0,0,4,4,0,1}, .scissor = {{0,0},{4,4}},
        .raster = {.depth_bias_enable = VK_TRUE, .depth_bias_slope = 2.0f}};
    VkRect2D area = {{0,0},{4,4}};
    struct ps5vk_prepared_draw prepared = {0};
    target_rc = VK_ERROR_FORMAT_NOT_SUPPORTED;
    assert(ps5vk_native_prepare_draw(&d, &op, &area, NULL, &prepared) == target_rc && !allocations);
    target_rc = VK_SUCCESS; flush_rc = VK_ERROR_MEMORY_MAP_FAILED;
    assert(ps5vk_native_prepare_draw(&d, &op, &area, NULL, &prepared) == flush_rc);
    assert(allocations == releases && !prepared.backing);
    flush_rc = VK_SUCCESS;
    assert(ps5vk_native_prepare_draw(&d, &op, &area, NULL, &prepared) == VK_SUCCESS);
    assert(prepared.state->modifier == 5 && prepared.bytes == sizeof(*prepared.state));
    unsigned before = targets;
    assert(ps5vk_native_prepare_draw(&d, &op, &area, NULL, &prepared) != VK_SUCCESS && targets == before);
    ps5vk_native_release_draw(&prepared); ps5vk_native_release_draw(&prepared);
    assert(allocations == releases && !prepared.state);
    expected_bytes=((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+16;
    /* Fixed aligned allocation fixture makes the aperture test deterministic. */
    uint64_t shader_address=latest_address;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==VK_SUCCESS);
    assert(prepared.vertex_table && (uintptr_t)prepared.vertex_table%16==0 && !prepared.vertex_bounce);
    assert(prepared.vertex_table[0]==(uint32_t)(uintptr_t)vertex_source &&
        (prepared.vertex_table[1]&0xffff)==(uintptr_t)vertex_source>>32 &&
        prepared.vertex_table[1]>>16==4 && prepared.vertex_table[2]==4);
    assert(prepared.bytes==expected_bytes);
    ps5vk_native_release_draw(&prepared);
    for(unsigned i=0;i<sizeof(vertex_source);++i)vertex_source[i]=(unsigned char)(i+1);
    fetch_address=vertex_source+1;
    expected_bytes=((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+16+16;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==VK_SUCCESS);
    assert(prepared.vertex_bounce && (uintptr_t)prepared.vertex_bounce%4==0 &&
        prepared.vertex_bounce_bytes==16 && !memcmp(prepared.vertex_bounce,vertex_source+1,16));
    assert(prepared.vertex_table[0]==(uint32_t)(uintptr_t)prepared.vertex_bounce &&
        (prepared.vertex_table[1]&0xffff)==(uintptr_t)prepared.vertex_bounce>>32);
    ps5vk_native_release_draw(&prepared);
    /* An unencodable byte-granular source may need an aligned bounce.  Failure
     * to allocate it is deterministic and leaves no partial prepared draw. */
    unsigned bounce_allocated=allocations;
    fail_allocation_size=expected_bytes;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==
        VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(allocations==bounce_allocated && releases==allocations &&
        !prepared.backing && !prepared.vertex_bounce);
    fail_allocation_size=0;fetch_address=vertex_source;
    /* Compiler mask {0,15} requires two packed SRDs, not sixteen sparse SRDs.
     * Each unaligned source owns its bounce until draw retirement. */
    fetch_count=16;fetch_address=vertex_source+1;
    expected_bytes=((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+32+32;
    assert(ps5vk_native_prepare_vertex_draw_masked(&d,&op,&area,NULL,NULL,shader_address,0x8001,&prepared)==VK_SUCCESS);
    assert(prepared.vertex_bounce_bytes==32);
    assert(prepared.vertex_table[0]!=prepared.vertex_table[4]);
    assert(prepared.vertex_table[6]==4);
    ps5vk_native_release_draw(&prepared);
    assert(allocations==releases);
    fetch_count=1;fetch_address=vertex_source;
    expected_bytes=((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+16;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address^(UINT64_C(1)<<32),&prepared)==VK_ERROR_MEMORY_MAP_FAILED);
    assert(allocations==releases && !prepared.backing);
    unsigned allocated=allocations;fetch_rc=VK_ERROR_UNKNOWN;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==fetch_rc && allocations==allocated);
    fetch_rc=VK_SUCCESS;
    struct VkDescriptorPool_T pool={.device=&d};
    struct VkDescriptorSet_T set={.pool=&pool,.generation=7};
    set.signature.count=1;set.signature.binding[0].count=1;set.signature.type[0]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;set.defined[0]=VK_TRUE;
    set.signature.binding[0].stages=VK_SHADER_STAGE_FRAGMENT_BIT;
    for(unsigned b=1;b<PS5VK_MAX_BINDINGS;++b)set.signature.binding[b].first=1;
    p.sets[0]=set.signature;
    op.pipeline->set_count=1;op.sets[0]=&set;op.generations[0]=7;
    expected_bytes+=48;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==VK_SUCCESS);
    assert(prepared.texture_table==prepared.vertex_table+4 && prepared.bytes==expected_bytes);
    for(unsigned i=0;i<12;++i)assert(prepared.texture_table[i]==100+i);
    ps5vk_native_release_draw(&prepared);assert(!prepared.texture_table && allocations==releases);
    allocated=allocations;set.signature.binding[1].first=0;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)!=VK_SUCCESS &&
        allocations==allocated && !prepared.backing);
    set.signature.binding[1].first=1;
    set.pool=NULL;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)!=VK_SUCCESS);
    set.pool=&pool;
    allocated=allocations;op.generations[0]=6;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)!=VK_SUCCESS && allocations==allocated);
    op.generations[0]=7;texture_rc=VK_ERROR_UNKNOWN;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==texture_rc &&
        allocations==allocated+1 && releases==allocations && !prepared.backing);
    texture_rc=VK_SUCCESS;flush_rc=VK_ERROR_MEMORY_MAP_FAILED;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==flush_rc && allocations==releases);
    flush_rc=VK_SUCCESS;
    struct VkDescriptorSet_T sets[4]={0};
    p.set_count=4;runtime.enabled=1;
    for(unsigned s=0;s<4;++s) {
        sets[s].pool=&pool;sets[s].generation=11+s;
        for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
            struct ps5vk_binding *binding=&sets[s].signature.binding[b];
            binding->first=sets[s].signature.count;
            if(b==3 || b==7) {
                binding->count=b==3?2:22;binding->stages=VK_SHADER_STAGE_FRAGMENT_BIT;
                sets[s].signature.type[b]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                sets[s].signature.count+=binding->count;
            }
        }
        for(unsigned e=0;e<24;++e) {
            sets[s].defined[e]=VK_TRUE;
            sets[s].images[e].imageView=(VkImageView)(uintptr_t)(1+24*s+e);
        }
        p.sets[s]=sets[s].signature;op.sets[s]=sets+s;op.generations[s]=sets[s].generation;
        runtime.fragment_descriptor_valid[s]=1;
    }
    expected_bytes=((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+16+4*24*48;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==VK_SUCCESS);
    for(unsigned s=0;s<4;++s) {
        assert(prepared.descriptor_bytes[s]==24*48);
        assert(prepared.descriptor_tables[s]==prepared.vertex_table+4+s*24*12);
        for(unsigned e=0;e<24;++e)for(unsigned w=0;w<12;++w)
            assert(prepared.descriptor_tables[s][12*e+w]==100+w+256*(1+24*s+e));
    }
    ps5vk_native_release_draw(&prepared);assert(allocations==releases && !prepared.descriptor_tables[3]);
    allocated=allocations;sets[3].defined[23]=VK_FALSE;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)!=VK_SUCCESS &&
        allocations==allocated && !prepared.backing);
    sets[3].defined[23]=VK_TRUE;op.generations[3]--;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)!=VK_SUCCESS && allocations==allocated);
    op.generations[3]++;texture_fail_view=sets[3].images[23].imageView;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)!=VK_SUCCESS &&
        allocations==allocated+1 && releases==allocations && !prepared.backing && !prepared.descriptor_tables[0]);
    texture_fail_view=NULL;
    /* Share sets0/3 between stages, set1 only VS and set2 only FS. Each
     * table is allocated once and keeps its canonical element addresses. */
    for(unsigned s=0;s<4;++s) {
        VkShaderStageFlags stages=s==1?VK_SHADER_STAGE_VERTEX_BIT:
            s==2?VK_SHADER_STAGE_FRAGMENT_BIT:
            VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
        sets[s].signature.binding[3].stages=stages;
        sets[s].signature.binding[7].stages=stages;
        p.sets[s]=sets[s].signature;
        runtime.vertex_descriptor_valid[s]=s!=2;
        runtime.fragment_descriptor_valid[s]=s!=1;
    }
    /* One shared table can also contain bindings with distinct visibility;
     * stage filtering must not compact their offsets. */
    sets[0].signature.binding[3].stages=VK_SHADER_STAGE_VERTEX_BIT;
    sets[0].signature.binding[7].stages=VK_SHADER_STAGE_FRAGMENT_BIT;
    p.sets[0]=sets[0].signature;
    expected_bytes-=16;
    allocated=allocations;
    assert(ps5vk_native_prepare_resource_draw(&d,&op,&area,NULL,shader_address,&prepared)==VK_SUCCESS);
    assert(!prepared.vertex_table && prepared.descriptor_tables[3] &&
           prepared.bytes==expected_bytes && allocations==allocated+1);
    for(unsigned s=0;s<4;++s) {
        assert(prepared.descriptor_bytes[s]==24*48);
        assert((uintptr_t)prepared.descriptor_tables[s]==
               latest_address+((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+s*24*48);
        for(unsigned e=0;e<24;++e)for(unsigned w=0;w<12;++w)
            assert(prepared.descriptor_tables[s][12*e+w]==100+w+256*(1+24*s+e));
    }
    ps5vk_native_release_draw(&prepared);assert(allocations==releases);
    const VkShaderStageFlags visibility[]={VK_SHADER_STAGE_ALL,VK_SHADER_STAGE_ALL_GRAPHICS,
        VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT};
    for(unsigned i=0;i<sizeof(visibility)/sizeof(visibility[0]);++i) {
        sets[1].signature.binding[3].stages=visibility[i];p.sets[1]=sets[1].signature;
        assert(ps5vk_native_prepare_resource_draw(&d,&op,&area,NULL,shader_address,&prepared)==VK_SUCCESS);
        assert(prepared.bytes==expected_bytes);
        for(unsigned s=0;s<4;++s) {
            assert(prepared.descriptor_bytes[s]==24*48);
            for(unsigned e=0;e<24;++e)for(unsigned w=0;w<12;++w)
                assert(prepared.descriptor_tables[s][12*e+w]==100+w+256*(1+24*s+e));
        }
        assert(sets[1].signature.binding[3].stages==visibility[i]);
        ps5vk_native_release_draw(&prepared);assert(allocations==releases);
    }
    sets[1].signature.binding[3].stages=UINT32_C(0x40000000);p.sets[1]=sets[1].signature;
    allocated=allocations;
    assert(ps5vk_native_prepare_resource_draw(&d,&op,&area,NULL,shader_address,&prepared)!=VK_SUCCESS &&
           allocations==allocated && !prepared.backing);
    sets[1].signature.binding[3].stages=VK_SHADER_STAGE_VERTEX_BIT;p.sets[1]=sets[1].signature;
    /* A vertex-only set is just as mandatory as a fragment set. */
    allocated=allocations;op.sets[1]=NULL;
    assert(ps5vk_native_prepare_resource_draw(&d,&op,&area,NULL,shader_address,&prepared)!=VK_SUCCESS &&
           allocations==allocated && !prepared.backing);
    op.sets[1]=sets+1;op.generations[1]--;
    assert(ps5vk_native_prepare_resource_draw(&d,&op,&area,NULL,shader_address,&prepared)!=VK_SUCCESS &&
           allocations==allocated && !prepared.backing);
    op.generations[1]++;sets[1].defined[23]=VK_FALSE;
    assert(ps5vk_native_prepare_resource_draw(&d,&op,&area,NULL,shader_address,&prepared)!=VK_SUCCESS &&
           allocations==allocated && !prepared.backing);
    sets[1].defined[23]=VK_TRUE;texture_fail_view=sets[1].images[23].imageView;
    assert(ps5vk_native_prepare_resource_draw(&d,&op,&area,NULL,shader_address,&prepared)!=VK_SUCCESS &&
           allocations==allocated+1 && allocations==releases && !prepared.backing);
    texture_fail_view=NULL;
    sets[1].signature.binding[3].stages=VK_SHADER_STAGE_COMPUTE_BIT;
    p.sets[1]=sets[1].signature;allocated=allocations;
    assert(ps5vk_native_prepare_resource_draw(&d,&op,&area,NULL,shader_address,&prepared)==VK_ERROR_FEATURE_NOT_PRESENT &&
           allocations==allocated && !prepared.backing);
    sets[1].signature.binding[3].stages=VK_SHADER_STAGE_VERTEX_BIT;
    p.sets[1]=sets[1].signature;
    /* Synthetic ABI with inactive sets: no allocation or dereference.
     * Actual PSBC currently conservatively reserves option-visible sets. */
    runtime.vertex_descriptor_valid[1]=0;
    runtime.fragment_descriptor_valid[1]=runtime.fragment_descriptor_valid[2]=0;
    op.sets[1]=op.sets[2]=NULL;expected_bytes-=2*24*48;
    assert(ps5vk_native_prepare_resource_draw(&d,&op,&area,NULL,shader_address,&prepared)==VK_SUCCESS);
    assert(!prepared.descriptor_tables[1] && !prepared.descriptor_tables[2] && prepared.descriptor_tables[3]);
    ps5vk_native_release_draw(&prepared);assert(allocations==releases);
    /* Mixed resource delivery: a mandatory uniform buffer coexists with the
     * sampler array inside the same set and the same four-set ABI. The sparse
     * binding numbers (5 and 7) are what a real layout carries. */
    {
        struct VkDescriptorPool_T mixed_pool={.device=&d};
        struct VkPipeline_T mixed_pipeline={.device=&d};
        struct VkDescriptorSet_T mixed[4]={0};
        struct ps5vk_runtime_draw_abi mixed_runtime={.enabled=1};
        struct ps5vk_operation mixed_op=op;
        mixed_op.pipeline=&mixed_pipeline;mixed_op.pipeline->set_count=4;
        for(unsigned s=0;s<4;++s) {
            uint32_t prefix=0;
            for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
                struct ps5vk_binding *binding=&mixed[s].signature.binding[b];
                binding->first=prefix;
                if(b==5) {
                    binding->count=1;binding->stages=VK_SHADER_STAGE_FRAGMENT_BIT;
                    mixed[s].signature.type[b]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;prefix+=1;
                } else if(b==7) {
                    binding->count=24;binding->stages=VK_SHADER_STAGE_FRAGMENT_BIT;
                    mixed[s].signature.type[b]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    prefix+=24;
                }
            }
            mixed[s].signature.count=prefix;mixed[s].pool=&mixed_pool;
            mixed[s].generation=21+s;
            for(unsigned e=0;e<25;++e)mixed[s].defined[e]=VK_TRUE;
            /* Buffer handles are opaque here; the encoder stub owns indexing. */
            mixed[s].buffers[0]=(VkDescriptorBufferInfo){
                (VkBuffer)(uintptr_t)(1+s),0,64};
            for(unsigned e=0;e<24;++e)
                mixed[s].images[1+e].imageView=(VkImageView)(uintptr_t)(1+24*s+e);
            mixed_pipeline.sets[s]=mixed[s].signature;
            mixed_op.sets[s]=mixed+s;mixed_op.generations[s]=mixed[s].generation;
            mixed_runtime.fragment_descriptor_valid[s]=1;
        }
        unsigned mixed_allocated=allocations;
        expected_bytes=((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+4*(16+24*48);
        unsigned mixed_buffer_calls=buffer_calls;
        /* The runtime draw ABI is what the native path receives. */
        runtime=mixed_runtime;
        assert(ps5vk_native_prepare_resource_draw(&d,&mixed_op,&area,NULL,shader_address,&prepared)==
            VK_SUCCESS);
        assert(prepared.bytes==expected_bytes && allocations==mixed_allocated+1);
        /* Four buffer records were really encoded, each with no dynamic offset. */
        assert(buffer_calls==mixed_buffer_calls+4 && last_buffer_dynamic==0 &&
            first_set_buffer_dynamic==0);
        for(unsigned s=0;s<4;++s) {
            assert(prepared.descriptor_bytes[s]==16+24*48);
            for(unsigned w=0;w<4;++w)
                assert(prepared.descriptor_tables[s][w]==200+w+256*(unsigned)(1+s));
            for(unsigned e=0;e<24;++e)for(unsigned w=0;w<12;++w)
                assert(prepared.descriptor_tables[s][4+12*e+w]==100+w+256*(1+24*s+e));
        }
        ps5vk_native_release_draw(&prepared);assert(allocations==releases);
        mixed_allocated=allocations;
        /* An undefined mandatory uniform buffer fails closed before any
         * allocation, exactly like an undefined sampler element. */
        mixed[2].defined[0]=VK_FALSE;
        assert(ps5vk_native_prepare_resource_draw(&d,&mixed_op,&area,NULL,shader_address,&prepared)!=
            VK_SUCCESS && allocations==mixed_allocated && !prepared.backing);
        mixed[2].defined[0]=VK_TRUE;
        /* A null or foreign buffer must not be encoded as a base address. */
        mixed[2].buffers[0].buffer=NULL;
        assert(ps5vk_native_prepare_resource_draw(&d,&mixed_op,&area,NULL,shader_address,&prepared)!=
            VK_SUCCESS && allocations==mixed_allocated && !prepared.backing);
        mixed[2].buffers[0].buffer=(VkBuffer)(uintptr_t)3;
        /* A descriptor type outside the bounded profile stays unsupported
         * rather than being half-delivered. */
        mixed[1].signature.type[5]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        mixed_pipeline.sets[1]=mixed[1].signature;
        assert(ps5vk_native_prepare_resource_draw(&d,&mixed_op,&area,NULL,shader_address,&prepared)==
            VK_ERROR_FEATURE_NOT_PRESENT && allocations==mixed_allocated && !prepared.backing);
        mixed[1].signature.type[5]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        mixed_pipeline.sets[1]=mixed[1].signature;
        /* Encoder failure releases the whole prepared block. */
        buffer_fail=(VkBuffer)(uintptr_t)4;
        assert(ps5vk_native_prepare_resource_draw(&d,&mixed_op,&area,NULL,shader_address,&prepared)!=
            VK_SUCCESS && allocations==mixed_allocated+1 && allocations==releases &&
            !prepared.backing && !prepared.descriptor_tables[0]);
        buffer_fail=NULL;
        /* A dynamic uniform buffer adds its recorded offset to the record. */
        mixed[0].signature.type[5]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        mixed_pipeline.sets[0]=mixed[0].signature;
        mixed_op.descriptor_dynamic_offsets[0]=256;
        assert(ps5vk_native_prepare_resource_draw(&d,&mixed_op,&area,NULL,shader_address,&prepared)==
            VK_SUCCESS);
        assert(first_set_buffer_dynamic==256);
        for(unsigned w=0;w<4;++w)
            assert(prepared.descriptor_tables[0][w]==200+w+256*(unsigned)1);
        ps5vk_native_release_draw(&prepared);assert(allocations==releases);
        runtime.enabled=1;
    }

    /* Only the bindings the compiled stages name are a requirement. Binding 3
     * is declared in every set but no stage dereferences it, so its elements
     * need no descriptor, no view and no layout; its records stay zero while
     * the used binding 7 is still encoded. */
    for(unsigned s=0;s<4;++s) {
        runtime.vertex_descriptor_valid[s]=0;
        runtime.fragment_descriptor_valid[s]=1;
        runtime.vertex_used_bindings[s]=0;
        runtime.fragment_used_bindings[s]=UINT64_C(1)<<7;
        sets[s].defined[0]=sets[s].defined[1]=VK_FALSE;
        sets[s].images[0].imageView=sets[s].images[1].imageView=NULL;
        p.sets[s]=sets[s].signature;op.sets[s]=sets+s;op.generations[s]=sets[s].generation;
    }
    expected_bytes=((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+4*24*48;
    allocated=allocations;
    assert(ps5vk_native_prepare_resource_draw(&d,&op,&area,NULL,shader_address,&prepared)==VK_SUCCESS &&
           prepared.bytes==expected_bytes);
    for(unsigned s=0;s<4;++s) {
        for(unsigned w=0;w<12;++w)
            assert(prepared.descriptor_tables[s][w]==0 && prepared.descriptor_tables[s][12+w]==0);
        for(unsigned e=2;e<24;++e)for(unsigned w=0;w<12;++w)
            assert(prepared.descriptor_tables[s][12*e+w]==100+w+256*(1+24*s+e));
    }
    ps5vk_native_release_draw(&prepared);assert(allocations==releases);
    /* The same undefined element fails closed once a stage names that binding. */
    runtime.fragment_used_bindings[0]=UINT64_C(1)<<3;
    allocated=allocations;
    assert(ps5vk_native_prepare_resource_draw(&d,&op,&area,NULL,shader_address,&prepared)!=VK_SUCCESS &&
           allocations==allocated && !prepared.backing);

    /* Resource-only input attachments. The table gives every element eight
     * DWORDs, so a mixed buffer/input/combined set keeps its canonical offsets,
     * the input records can never receive sampler words, and the role is
     * dispatched to the resource-only encoder - whose signature has no sampler
     * parameter and which is never the combined entry point. */
    {
        struct VkDescriptorPool_T in_pool={.device=&d};
        struct VkPipeline_T in_pipeline={.device=&d};
        struct ps5vk_operation in_op=op;
        struct VkDevice_T foreign={0};
        struct VkDescriptorSet_T in={0};
        uint32_t prefix=0;
        for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
            struct ps5vk_binding *binding=&in.signature.binding[b];
            binding->first=prefix;
            if(b==1) {
                binding->count=1;binding->stages=VK_SHADER_STAGE_FRAGMENT_BIT;
                in.signature.type[b]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;prefix+=1;
            } else if(b==3) {
                binding->count=2;binding->stages=VK_SHADER_STAGE_FRAGMENT_BIT;
                in.signature.type[b]=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;prefix+=2;
            } else if(b==5) {
                binding->count=1;binding->stages=VK_SHADER_STAGE_FRAGMENT_BIT;
                in.signature.type[b]=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;prefix+=1;
            } else if(b==7) {
                binding->count=1;binding->stages=VK_SHADER_STAGE_FRAGMENT_BIT;
                in.signature.type[b]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;prefix+=1;
            }
        }
        /* Canonical byte offsets: 16 buffer, 32/32 input, 32 unused, 48 combined. */
        in.signature.count=prefix;in.pool=&in_pool;in.generation=31;
        for(unsigned e=0;e<prefix;++e)in.defined[e]=VK_TRUE;
        in.buffers[0]=(VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)1,0,64};
        /* Input attachments carry no sampler: both accepted layouts work with
         * VkDescriptorImageInfo::sampler left null, because nothing reads it. */
        in.images[1]=(VkDescriptorImageInfo){NULL,(VkImageView)(uintptr_t)101,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        in.images[2]=(VkDescriptorImageInfo){NULL,(VkImageView)(uintptr_t)102,
            VK_IMAGE_LAYOUT_GENERAL};
        /* Binding 5 is declared with count 1 but no compiled stage names it, so
         * it stays undefined and viewless and its record must stay zero. */
        in.defined[3]=VK_FALSE;
        in.images[3]=(VkDescriptorImageInfo){NULL,NULL,VK_IMAGE_LAYOUT_UNDEFINED};
        in.images[4]=(VkDescriptorImageInfo){(VkSampler)(uintptr_t)9,(VkImageView)(uintptr_t)103,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        in_pipeline.set_count=1;in_pipeline.sets[0]=in.signature;
        in_op.pipeline=&in_pipeline;in_op.sets[0]=&in;in_op.generations[0]=in.generation;
        runtime=(struct ps5vk_runtime_draw_abi){.enabled=1};
        runtime.fragment_descriptor_valid[0]=1;
        runtime.fragment_used_bindings[0]=(UINT64_C(1)<<1)|(UINT64_C(1)<<3)|(UINT64_C(1)<<7);
        expected_bytes=((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+160;
        unsigned in_allocated=allocations;
        unsigned in_texture_calls=texture_calls,in_resource_calls=resource_calls;
        assert(ps5vk_native_prepare_resource_draw(&d,&in_op,&area,NULL,shader_address,&prepared)==
            VK_SUCCESS);
        assert(prepared.bytes==expected_bytes && prepared.descriptor_bytes[0]==160 &&
            prepared.texture_table==prepared.descriptor_tables[0] && allocations==in_allocated+1);
        const uint32_t *t=prepared.descriptor_tables[0];
        for(unsigned i=0;i<4;++i)assert(t[i]==200+i+256);
        for(unsigned i=0;i<8;++i)assert(t[4+i]==300+i+256*101);   /* input element 0 */
        for(unsigned i=0;i<8;++i)assert(t[12+i]==300+i+256*102);  /* input element 1 */
        for(unsigned i=0;i<8;++i)assert(t[20+i]==0);              /* unused declaration */
        for(unsigned i=0;i<12;++i)assert(t[28+i]==100+i+256*103); /* combined record */
        assert(resource_calls==in_resource_calls+2 && texture_calls==in_texture_calls+1);
        /* Releasing twice is idempotent and clears the published result. */
        ps5vk_native_release_draw(&prepared);ps5vk_native_release_draw(&prepared);
        assert(allocations==releases && !prepared.state && !prepared.backing &&
            !prepared.descriptor_tables[0] && !prepared.texture_table);
        /* Each refusal below leaves the caller's prepared draw untouched, and
         * the three pre-allocation ones never even allocate. */
        in.images[1].imageView=NULL;
        assert(ps5vk_native_prepare_resource_draw(&d,&in_op,&area,NULL,shader_address,&prepared)!=
            VK_SUCCESS && allocations==in_allocated+1 && releases==allocations &&
            !prepared.backing && !prepared.state);
        in.images[1].imageView=(VkImageView)(uintptr_t)101;
        in.images[1].imageLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        assert(ps5vk_native_prepare_resource_draw(&d,&in_op,&area,NULL,shader_address,&prepared)!=
            VK_SUCCESS && allocations==in_allocated+1 && !prepared.backing);
        in.images[1].imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        assert(ps5vk_native_prepare_resource_draw(&d,&in_op,&area,NULL,shader_address,&prepared)!=
            VK_SUCCESS && allocations==in_allocated+1 && !prepared.backing);
        in.images[1].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        /* No visibility at all is refused by the canonical layout, and a
         * visibility the layout may not declare for this role - vertex-only,
         * vertex+fragment or the ALL convenience mask - is refused here. */
        in.signature.binding[3].stages=0;in_pipeline.sets[0]=in.signature;
        assert(ps5vk_native_prepare_resource_draw(&d,&in_op,&area,NULL,shader_address,&prepared)!=
            VK_SUCCESS && allocations==in_allocated+1 && !prepared.backing);
        const VkShaderStageFlags refused[]={VK_SHADER_STAGE_VERTEX_BIT,
            VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,VK_SHADER_STAGE_ALL,
            VK_SHADER_STAGE_COMPUTE_BIT};
        for(unsigned i=0;i<sizeof(refused)/sizeof(refused[0]);++i) {
            in.signature.binding[3].stages=refused[i];in_pipeline.sets[0]=in.signature;
            assert(ps5vk_native_prepare_resource_draw(&d,&in_op,&area,NULL,shader_address,&prepared)==
                VK_ERROR_FEATURE_NOT_PRESENT && allocations==in_allocated+1 && !prepared.backing);
        }
        in.signature.binding[3].stages=VK_SHADER_STAGE_FRAGMENT_BIT;in_pipeline.sets[0]=in.signature;
        /* A descriptor type outside the profile stays unsupported. */
        in.signature.type[3]=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;in_pipeline.sets[0]=in.signature;
        assert(ps5vk_native_prepare_resource_draw(&d,&in_op,&area,NULL,shader_address,&prepared)==
            VK_ERROR_FEATURE_NOT_PRESENT && allocations==in_allocated+1 && !prepared.backing);
        in.signature.type[3]=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;in_pipeline.sets[0]=in.signature;
        /* Ownership, generation and signature identity are all mandatory. */
        in_pool.device=&foreign;
        assert(ps5vk_native_prepare_resource_draw(&d,&in_op,&area,NULL,shader_address,&prepared)!=
            VK_SUCCESS && allocations==in_allocated+1 && !prepared.backing);
        in_pool.device=&d;in.generation=32;
        assert(ps5vk_native_prepare_resource_draw(&d,&in_op,&area,NULL,shader_address,&prepared)!=
            VK_SUCCESS && allocations==in_allocated+1 && !prepared.backing);
        /* Signature drift alone is refused: the pipeline declares a different
         * visibility for a binding the live set still carries. */
        in.generation=31;in_pipeline.sets[0].binding[7].stages=VK_SHADER_STAGE_VERTEX_BIT;
        assert(ps5vk_native_prepare_resource_draw(&d,&in_op,&area,NULL,shader_address,&prepared)!=
            VK_SUCCESS && allocations==in_allocated+1 && !prepared.backing);
        in_pipeline.sets[0]=in.signature;
        /* Encoder failure and flush failure both release the whole block. */
        resource_fail_view=(VkImageView)(uintptr_t)101;
        unsigned hits=allocations;
        assert(ps5vk_native_prepare_resource_draw(&d,&in_op,&area,NULL,shader_address,&prepared)==
            VK_ERROR_UNKNOWN && allocations==hits+1 && releases==allocations &&
            !prepared.backing && !prepared.state && !prepared.descriptor_tables[0]);
        resource_fail_view=NULL;resource_rc=VK_ERROR_FEATURE_NOT_PRESENT;hits=allocations;
        assert(ps5vk_native_prepare_resource_draw(&d,&in_op,&area,NULL,shader_address,&prepared)==
            VK_ERROR_FEATURE_NOT_PRESENT && allocations==hits+1 && releases==allocations &&
            !prepared.backing);
        resource_rc=VK_SUCCESS;flush_rc=VK_ERROR_MEMORY_MAP_FAILED;hits=allocations;
        assert(ps5vk_native_prepare_resource_draw(&d,&in_op,&area,NULL,shader_address,&prepared)==
            VK_ERROR_MEMORY_MAP_FAILED && allocations==hits+1 && releases==allocations &&
            !prepared.backing);
        flush_rc=VK_SUCCESS;
        /* The precompiled single-table ABI (no runtime draw ABI) delivers the
         * same resource-only record through the texture-table slot. */
        struct VkPipeline_T single_pipeline={.device=&d};
        struct ps5vk_operation single_op=in_op;
        struct VkDescriptorSet_T single={0};
        single.pool=&in_pool;single.generation=41;
        single.signature.count=1;
        single.signature.binding[0]=(struct ps5vk_binding){.count=1,
            .stages=VK_SHADER_STAGE_FRAGMENT_BIT};
        single.signature.type[0]=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        for(unsigned b=1;b<PS5VK_MAX_BINDINGS;++b)single.signature.binding[b].first=1;
        single.defined[0]=VK_TRUE;
        single.images[0]=(VkDescriptorImageInfo){NULL,(VkImageView)(uintptr_t)111,
            VK_IMAGE_LAYOUT_GENERAL};
        single_pipeline.set_count=1;single_pipeline.sets[0]=single.signature;
        single_op.pipeline=&single_pipeline;single_op.sets[0]=&single;
        single_op.generations[0]=single.generation;
        runtime=(struct ps5vk_runtime_draw_abi){0};
        expected_bytes=((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+32;
        in_texture_calls=texture_calls;in_resource_calls=resource_calls;hits=allocations;
        assert(ps5vk_native_prepare_resource_draw(&d,&single_op,&area,NULL,shader_address,&prepared)==
            VK_SUCCESS);
        assert(prepared.bytes==expected_bytes && prepared.descriptor_bytes[0]==32 &&
            prepared.texture_table==prepared.descriptor_tables[0] && allocations==hits+1);
        for(unsigned i=0;i<8;++i)assert(prepared.texture_table[i]==300+i+256*111);
        assert(resource_calls==in_resource_calls+1 && texture_calls==in_texture_calls);
        ps5vk_native_release_draw(&prepared);
        assert(allocations==releases && !prepared.descriptor_tables[0] && !prepared.state);
    }
}
