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
static struct ps5vk_runtime_draw_abi runtime;
VkResult ps5vk_texture_descriptor(VkDevice d,VkImageView v,VkSampler s,uint32_t out[12])
{(void)d;(void)s;for(unsigned i=0;i<12;++i)out[i]=100+i+256*(uintptr_t)v;
 return texture_fail_view && v==texture_fail_view?VK_ERROR_UNKNOWN:texture_rc;}
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
    const VkRect2D *scissor, const struct ps5vk_target_registers *color,
    const struct ps5vk_target_registers *depth, const VkRect2D *area,
    uint32_t width, uint32_t height, struct ps5vk_draw_state *out)
{
    assert(p && viewport->width == 4 && scissor->extent.width == 4 &&
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
    struct VkRenderPass_T pass = {.device = &d};
    struct VkPipeline_T p = {.device = &d};
    struct ps5vk_operation op = {.type = PS5VK_DRAW, .pipeline = &p, .framebuffer = &fb,
        .render_pass = &pass, .viewport = {0,0,4,4,0,1}, .scissor = {{0,0},{4,4}}};
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
}
